#include "offline_dict.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_set>
#include <zlib.h>

namespace OfflineDict {

namespace {

static inline uint16_t RdU16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t RdU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::vector<uint32_t> Utf8ToCps(const std::string &s) {
    std::vector<uint32_t> cps;
    size_t i = 0, n = s.size();
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x80) { cps.push_back(c); ++i; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cps.push_back(((uint32_t)(c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cps.push_back(((uint32_t)(c & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F));
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cps.push_back(((uint32_t)(c & 0x07) << 18) | (((uint8_t)s[i + 1] & 0x3F) << 12) | (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F));
            i += 4;
        } else { cps.push_back('?'); ++i; }
    }
    return cps;
}

static std::string CpsToUtf8(const std::vector<uint32_t> &cps, size_t start, size_t count) {
    std::string out;
    size_t end = std::min(cps.size(), start + count);
    for (size_t i = start; i < end; ++i) {
        uint32_t cp = cps[i];
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

static bool IsKanji(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || cp == 0x3005;
}

static bool IsJapaneseChar(uint32_t cp) {
    return IsKanji(cp) || (cp >= 0x3041 && cp <= 0x3096) || (cp >= 0x30A1 && cp <= 0x30FA) || cp == 0x30FC;
}

static bool EndsWith(const std::string &s, const char *suf) {
    size_t sl = std::strlen(suf);
    return s.size() >= sl && s.compare(s.size() - sl, sl, suf) == 0;
}

static std::string ReplaceSuffix(const std::string &s, const char *from, const char *to) {
    size_t fl = std::strlen(from);
    return s.substr(0, s.size() - fl) + to;
}

enum PosMask : uint16_t {
    POS_V1     = 1 << 0,
    POS_V5     = 1 << 1,
    POS_ADJ_I  = 1 << 2,
    POS_ADJ_NA = 1 << 3,
    POS_VK     = 1 << 4,
    POS_VS     = 1 << 5,
    POS_V_TE   = 1 << 6,
    POS_V_DE   = 1 << 7,
    POS_ANY    = 0xFFFF
};

struct DeinflectRule {
    const char *kanaIn;
    const char *kanaOut;
    const char *reason;
    uint16_t reqMask;
    uint16_t outMask;
};

static const DeinflectRule kRules[] = {
    // -te / -de auxiliary chains
    {"ても", "て", "even if", POS_ANY, POS_V_TE},
    {"でも", "で", "even if", POS_ANY, POS_V_DE},
    {"ては", "て", "-te wa", POS_ANY, POS_V_TE},
    {"では", "で", "-de wa", POS_ANY, POS_V_DE},
    {"ている", "て", "continuous", POS_ANY, POS_V_TE},
    {"ていた", "て", "continuous past", POS_ANY, POS_V_TE},
    {"ていない", "て", "continuous neg", POS_ANY, POS_V_TE},
    {"ていなかった", "て", "continuous neg past", POS_ANY, POS_V_TE},
    {"ています", "て", "continuous polite", POS_ANY, POS_V_TE},
    {"ていました", "て", "continuous polite past", POS_ANY, POS_V_TE},
    {"ていません", "て", "continuous polite neg", POS_ANY, POS_V_TE},
    {"てる", "て", "continuous", POS_ANY, POS_V_TE},
    {"てた", "て", "continuous past", POS_ANY, POS_V_TE},
    {"てない", "て", "continuous neg", POS_ANY, POS_V_TE},
    {"てなかった", "て", "continuous neg past", POS_ANY, POS_V_TE},
    {"ておる", "て", "continuous (humble)", POS_ANY, POS_V_TE},
    {"ており", "て", "continuous (humble)", POS_ANY, POS_V_TE},
    {"ております", "て", "continuous polite", POS_ANY, POS_V_TE},
    {"でいる", "で", "continuous", POS_ANY, POS_V_DE},
    {"でいた", "で", "continuous past", POS_ANY, POS_V_DE},
    {"でいない", "で", "continuous neg", POS_ANY, POS_V_DE},
    {"でいなかった", "で", "continuous neg past", POS_ANY, POS_V_DE},
    {"でいます", "で", "continuous polite", POS_ANY, POS_V_DE},
    {"でいました", "で", "continuous polite past", POS_ANY, POS_V_DE},
    {"でいません", "で", "continuous polite neg", POS_ANY, POS_V_DE},
    {"でる", "で", "continuous", POS_ANY, POS_V_DE},
    {"でた", "で", "continuous past", POS_ANY, POS_V_DE},
    {"でない", "で", "continuous neg", POS_ANY, POS_V_DE},
    {"でなかった", "で", "continuous neg past", POS_ANY, POS_V_DE},
    {"てくれる", "て", "-te kureru", POS_ANY, POS_V_TE},
    {"てくれた", "て", "-te kureru past", POS_ANY, POS_V_TE},
    {"てくれない", "て", "-te kureru neg", POS_ANY, POS_V_TE},
    {"てくれ", "て", "-te kureru imp", POS_ANY, POS_V_TE},
    {"でくれる", "で", "-te kureru", POS_ANY, POS_V_DE},
    {"でくれた", "で", "-te kureru past", POS_ANY, POS_V_DE},
    {"てください", "て", "polite request", POS_ANY, POS_V_TE},
    {"でください", "で", "polite request", POS_ANY, POS_V_DE},
    {"ていく", "て", "-te iku", POS_ANY, POS_V_TE},
    {"ていった", "て", "-te iku past", POS_ANY, POS_V_TE},
    {"てく", "て", "-te iku", POS_ANY, POS_V_TE},
    {"でいく", "で", "-te iku", POS_ANY, POS_V_DE},
    {"てくる", "て", "-te kuru", POS_ANY, POS_V_TE},
    {"てきた", "て", "-te kuru past", POS_ANY, POS_V_TE},
    {"てこない", "て", "-te kuru neg", POS_ANY, POS_V_TE},
    {"でくる", "で", "-te kuru", POS_ANY, POS_V_DE},
    {"できた", "で", "-te kuru past", POS_ANY, POS_V_DE},
    {"てしまう", "て", "completion", POS_ANY, POS_V_TE},
    {"てしまった", "て", "completion past", POS_ANY, POS_V_TE},
    {"ちゃう", "て", "completion", POS_ANY, POS_V_TE},
    {"ちゃった", "て", "completion past", POS_ANY, POS_V_TE},
    {"でしまう", "で", "completion", POS_ANY, POS_V_DE},
    {"でしまった", "で", "completion past", POS_ANY, POS_V_DE},
    {"じゃう", "で", "completion", POS_ANY, POS_V_DE},
    {"じゃった", "で", "completion past", POS_ANY, POS_V_DE},
    {"ておく", "て", "preparatory", POS_ANY, POS_V_TE},
    {"ておいた", "て", "preparatory past", POS_ANY, POS_V_TE},
    {"とく", "て", "preparatory", POS_ANY, POS_V_TE},
    {"といた", "て", "preparatory past", POS_ANY, POS_V_TE},
    {"てみる", "て", "try (-te miru)", POS_ANY, POS_V_TE},
    {"てみた", "て", "try past", POS_ANY, POS_V_TE},
    {"てみない", "て", "try neg", POS_ANY, POS_V_TE},
    {"でみる", "で", "try (-te miru)", POS_ANY, POS_V_DE},
    {"でみた", "で", "try past", POS_ANY, POS_V_DE},

    // Polite (-masu / -mashita / -masen / -mashou)
    {"ました", "る", "polite past", POS_ANY, POS_V1},
    {"ました", "する", "polite past", POS_ANY, POS_VS},
    {"ました", "くる", "polite past", POS_ANY, POS_VK},
    {"ませんでした", "る", "polite past neg", POS_ANY, POS_V1},
    {"ませんでした", "する", "polite past neg", POS_ANY, POS_VS},
    {"ません", "る", "polite neg", POS_ANY, POS_V1},
    {"ません", "する", "polite neg", POS_ANY, POS_VS},
    {"ます", "る", "polite", POS_ANY, POS_V1},
    {"ます", "する", "polite", POS_ANY, POS_VS},
    {"ましょう", "る", "polite volitional", POS_ANY, POS_V1},
    {"ましょう", "する", "polite volitional", POS_ANY, POS_VS},

    {"いました", "う", "polite past", POS_ANY, POS_V5},
    {"きました", "く", "polite past", POS_ANY, POS_V5},
    {"きました", "くる", "polite past", POS_ANY, POS_VK},
    {"ぎました", "ぐ", "polite past", POS_ANY, POS_V5},
    {"しました", "す", "polite past", POS_ANY, POS_V5},
    {"しました", "する", "polite past", POS_ANY, POS_VS},
    {"ちました", "つ", "polite past", POS_ANY, POS_V5},
    {"にました", "ぬ", "polite past", POS_ANY, POS_V5},
    {"びました", "ぶ", "polite past", POS_ANY, POS_V5},
    {"みました", "む", "polite past", POS_ANY, POS_V5},
    {"りました", "る", "polite past", POS_ANY, POS_V5},

    {"いません", "う", "polite neg", POS_ANY, POS_V5},
    {"きません", "く", "polite neg", POS_ANY, POS_V5},
    {"きません", "くる", "polite neg", POS_ANY, POS_VK},
    {"ぎません", "ぐ", "polite neg", POS_ANY, POS_V5},
    {"しません", "す", "polite neg", POS_ANY, POS_V5},
    {"しません", "する", "polite neg", POS_ANY, POS_VS},
    {"ちません", "つ", "polite neg", POS_ANY, POS_V5},
    {"にません", "ぬ", "polite neg", POS_ANY, POS_V5},
    {"びません", "ぶ", "polite neg", POS_ANY, POS_V5},
    {"みません", "む", "polite neg", POS_ANY, POS_V5},
    {"りません", "る", "polite neg", POS_ANY, POS_V5},

    {"いませんでした", "う", "polite past neg", POS_ANY, POS_V5},
    {"きませんでした", "く", "polite past neg", POS_ANY, POS_V5},
    {"ぎませんでした", "ぐ", "polite past neg", POS_ANY, POS_V5},
    {"しませんでした", "す", "polite past neg", POS_ANY, POS_V5},
    {"しませんでした", "する", "polite past neg", POS_ANY, POS_VS},
    {"ちませんでした", "つ", "polite past neg", POS_ANY, POS_V5},
    {"にませんでした", "ぬ", "polite past neg", POS_ANY, POS_V5},
    {"びませんでした", "ぶ", "polite past neg", POS_ANY, POS_V5},
    {"みませんでした", "む", "polite past neg", POS_ANY, POS_V5},
    {"りませんでした", "る", "polite past neg", POS_ANY, POS_V5},

    {"います", "う", "polite", POS_ANY, POS_V5},
    {"きます", "く", "polite", POS_ANY, POS_V5},
    {"きます", "くる", "polite", POS_ANY, POS_VK},
    {"ぎます", "ぐ", "polite", POS_ANY, POS_V5},
    {"します", "す", "polite", POS_ANY, POS_V5},
    {"します", "する", "polite", POS_ANY, POS_VS},
    {"ちます", "つ", "polite", POS_ANY, POS_V5},
    {"にます", "ぬ", "polite", POS_ANY, POS_V5},
    {"びます", "ぶ", "polite", POS_ANY, POS_V5},
    {"みます", "む", "polite", POS_ANY, POS_V5},
    {"ります", "る", "polite", POS_ANY, POS_V5},

    // i-adjective & na-adjective
    {"くなかった", "い", "i-adj neg past", POS_ANY, POS_ADJ_I},
    {"くない", "い", "i-adj neg", POS_ANY, POS_ADJ_I},
    {"かった", "い", "i-adj past", POS_ANY, POS_ADJ_I},
    {"くて", "い", "i-adj te", POS_ANY, POS_ADJ_I},
    {"くても", "い", "even if", POS_ANY, POS_ADJ_I},
    {"かったら", "い", "i-adj cond", POS_ANY, POS_ADJ_I},
    {"ければ", "い", "i-adj ba", POS_ANY, POS_ADJ_I},
    {"く", "い", "i-adj adv", POS_ANY, POS_ADJ_I},
    {"そう", "い", "looks like", POS_ANY, POS_ADJ_I},
    {"すぎる", "い", "too much", POS_ANY, POS_ADJ_I},
    {"な", "", "na-adj", POS_ANY, POS_ADJ_NA},

    // Negative / Past / Te-form
    {"なかった", "る", "past neg", POS_ANY, POS_V1},
    {"わなかった", "う", "past neg", POS_ANY, POS_V5},
    {"かなかった", "く", "past neg", POS_ANY, POS_V5},
    {"がなかった", "ぐ", "past neg", POS_ANY, POS_V5},
    {"さなかった", "す", "past neg", POS_ANY, POS_V5},
    {"たなかった", "つ", "past neg", POS_ANY, POS_V5},
    {"ななかった", "ぬ", "past neg", POS_ANY, POS_V5},
    {"ばなかった", "ぶ", "past neg", POS_ANY, POS_V5},
    {"まなかった", "む", "past neg", POS_ANY, POS_V5},
    {"らなかった", "る", "past neg", POS_ANY, POS_V5},
    {"しなかった", "する", "past neg", POS_ANY, POS_VS},
    {"こなかった", "くる", "past neg", POS_ANY, POS_VK},

    {"ない", "る", "neg", POS_ANY, POS_V1},
    {"わない", "う", "neg", POS_ANY, POS_V5},
    {"かない", "く", "neg", POS_ANY, POS_V5},
    {"がない", "ぐ", "neg", POS_ANY, POS_V5},
    {"さない", "す", "neg", POS_ANY, POS_V5},
    {"たない", "つ", "neg", POS_ANY, POS_V5},
    {"なない", "ぬ", "neg", POS_ANY, POS_V5},
    {"ばない", "ぶ", "neg", POS_ANY, POS_V5},
    {"まない", "む", "neg", POS_ANY, POS_V5},
    {"らない", "る", "neg", POS_ANY, POS_V5},
    {"しない", "する", "neg", POS_ANY, POS_VS},
    {"こない", "くる", "neg", POS_ANY, POS_VK},

    {"ず", "る", "neg (-zu)", POS_ANY, POS_V1},
    {"わず", "う", "neg (-zu)", POS_ANY, POS_V5},
    {"かず", "く", "neg (-zu)", POS_ANY, POS_V5},
    {"がず", "ぐ", "neg (-zu)", POS_ANY, POS_V5},
    {"さず", "す", "neg (-zu)", POS_ANY, POS_V5},
    {"たず", "つ", "neg (-zu)", POS_ANY, POS_V5},
    {"まず", "む", "neg (-zu)", POS_ANY, POS_V5},
    {"らず", "る", "neg (-zu)", POS_ANY, POS_V5},
    {"せず", "する", "neg (-zu)", POS_ANY, POS_VS},

    {"った", "う", "past", POS_ANY, POS_V5},
    {"った", "つ", "past", POS_ANY, POS_V5},
    {"った", "る", "past", POS_ANY, POS_V5},
    {"いた", "く", "past", POS_ANY, POS_V5},
    {"いだ", "ぐ", "past", POS_ANY, POS_V5},
    {"した", "す", "past", POS_ANY, POS_V5},
    {"した", "する", "past", POS_ANY, POS_VS},
    {"んだ", "ぬ", "past", POS_ANY, POS_V5},
    {"んだ", "ぶ", "past", POS_ANY, POS_V5},
    {"んだ", "む", "past", POS_ANY, POS_V5},
    {"た", "る", "past", POS_ANY, POS_V1},
    {"きた", "くる", "past", POS_ANY, POS_VK},

    {"って", "う", "te-form", POS_V_TE, POS_V5},
    {"って", "つ", "te-form", POS_V_TE, POS_V5},
    {"って", "る", "te-form", POS_V_TE, POS_V5},
    {"いて", "く", "te-form", POS_V_TE, POS_V5},
    {"いで", "ぐ", "te-form", POS_V_DE, POS_V5},
    {"して", "す", "te-form", POS_V_TE, POS_V5},
    {"して", "する", "te-form", POS_V_TE, POS_VS},
    {"んで", "ぬ", "te-form", POS_V_DE, POS_V5},
    {"んで", "ぶ", "te-form", POS_V_DE, POS_V5},
    {"んで", "む", "te-form", POS_V_DE, POS_V5},
    {"て", "る", "te-form", POS_V_TE, POS_V1},
    {"きて", "くる", "te-form", POS_V_TE, POS_VK},

    // Passive / Causative / Potential / Conditional / Volitional / Desire / Imperative
    {"られる", "る", "potential/passive", POS_V1, POS_V1},
    {"れる", "る", "potential/passive", POS_V1, POS_V1},
    {"われる", "う", "passive", POS_V1, POS_V5},
    {"かれる", "く", "passive", POS_V1, POS_V5},
    {"がれる", "ぐ", "passive", POS_V1, POS_V5},
    {"される", "す", "passive", POS_V1, POS_V5},
    {"される", "する", "passive", POS_V1, POS_VS},
    {"たれる", "つ", "passive", POS_V1, POS_V5},
    {"なれる", "ぬ", "passive", POS_V1, POS_V5},
    {"ばれる", "ぶ", "passive", POS_V1, POS_V5},
    {"まれる", "む", "passive", POS_V1, POS_V5},
    {"られる", "る", "passive", POS_V1, POS_V5},
    {"こられる", "くる", "potential/passive", POS_V1, POS_VK},

    {"させる", "る", "causative", POS_V1, POS_V1},
    {"させる", "する", "causative", POS_V1, POS_VS},
    {"わせる", "う", "causative", POS_V1, POS_V5},
    {"かせる", "く", "causative", POS_V1, POS_V5},
    {"がせる", "ぐ", "causative", POS_V1, POS_V5},
    {"させる", "す", "causative", POS_V1, POS_V5},
    {"たせる", "つ", "causative", POS_V1, POS_V5},
    {"なせる", "ぬ", "causative", POS_V1, POS_V5},
    {"ばせる", "ぶ", "causative", POS_V1, POS_V5},
    {"ませる", "む", "causative", POS_V1, POS_V5},
    {"らせる", "る", "causative", POS_V1, POS_V5},
    {"こさせる", "くる", "causative", POS_V1, POS_VK},

    {"える", "う", "potential", POS_V1, POS_V5},
    {"ける", "く", "potential", POS_V1, POS_V5},
    {"げる", "ぐ", "potential", POS_V1, POS_V5},
    {"せる", "す", "potential", POS_V1, POS_V5},
    {"てる", "つ", "potential", POS_V1, POS_V5},
    {"ねる", "ぬ", "potential", POS_V1, POS_V5},
    {"べる", "ぶ", "potential", POS_V1, POS_V5},
    {"める", "む", "potential", POS_V1, POS_V5},
    {"れる", "る", "potential", POS_V1, POS_V5},

    {"たい", "る", "desire", POS_ADJ_I, POS_V1},
    {"いたい", "う", "desire", POS_ADJ_I, POS_V5},
    {"きたい", "く", "desire", POS_ADJ_I, POS_V5},
    {"ぎたい", "ぐ", "desire", POS_ADJ_I, POS_V5},
    {"したい", "す", "desire", POS_ADJ_I, POS_V5},
    {"したい", "する", "desire", POS_ADJ_I, POS_VS},
    {"ちたい", "つ", "desire", POS_ADJ_I, POS_V5},
    {"にたい", "ぬ", "desire", POS_ADJ_I, POS_V5},
    {"びたい", "ぶ", "desire", POS_ADJ_I, POS_V5},
    {"みたい", "む", "desire", POS_ADJ_I, POS_V5},
    {"りたい", "る", "desire", POS_ADJ_I, POS_V5},
    {"きたい", "くる", "desire", POS_ADJ_I, POS_VK},

    {"れば", "る", "conditional", POS_ANY, POS_V1},
    {"えば", "う", "conditional", POS_ANY, POS_V5},
    {"けば", "く", "conditional", POS_ANY, POS_V5},
    {"げば", "ぐ", "conditional", POS_ANY, POS_V5},
    {"せば", "す", "conditional", POS_ANY, POS_V5},
    {"すれば", "する", "conditional", POS_ANY, POS_VS},
    {"てば", "つ", "conditional", POS_ANY, POS_V5},
    {"ねば", "ぬ", "conditional", POS_ANY, POS_V5},
    {"べば", "ぶ", "conditional", POS_ANY, POS_V5},
    {"めば", "む", "conditional", POS_ANY, POS_V5},
    {"くれば", "くる", "conditional", POS_ANY, POS_VK},
    {"たら", "た", "conditional", POS_ANY, POS_ANY},
    {"だら", "だ", "conditional", POS_ANY, POS_ANY},

    {"よう", "る", "volitional", POS_ANY, POS_V1},
    {"おう", "う", "volitional", POS_ANY, POS_V5},
    {"こう", "く", "volitional", POS_ANY, POS_V5},
    {"ごう", "ぐ", "volitional", POS_ANY, POS_V5},
    {"そう", "す", "volitional", POS_ANY, POS_V5},
    {"しよう", "する", "volitional", POS_ANY, POS_VS},
    {"とう", "つ", "volitional", POS_ANY, POS_V5},
    {"のう", "ぬ", "volitional", POS_ANY, POS_V5},
    {"ぼう", "ぶ", "volitional", POS_ANY, POS_V5},
    {"もう", "む", "volitional", POS_ANY, POS_V5},
    {"ろう", "る", "volitional", POS_ANY, POS_V5},
    {"こよう", "くる", "volitional", POS_ANY, POS_VK},

    {"なさい", "る", "imperative", POS_ANY, POS_V1},
    {"いなさい", "う", "imperative", POS_ANY, POS_V5},
    {"きなさい", "く", "imperative", POS_ANY, POS_V5},
    {"ぎなさい", "ぐ", "imperative", POS_ANY, POS_V5},
    {"しなさい", "す", "imperative", POS_ANY, POS_V5},
    {"しなさい", "する", "imperative", POS_ANY, POS_VS},
    {"ちなさい", "つ", "imperative", POS_ANY, POS_V5},
    {"みなさい", "む", "imperative", POS_ANY, POS_V5},
    {"りなさい", "る", "imperative", POS_ANY, POS_V5},
    {"ろ", "る", "imperative", POS_ANY, POS_V1},
    {"よ", "る", "imperative", POS_ANY, POS_V1},
    {"しろ", "する", "imperative", POS_ANY, POS_VS},
    {"こい", "くる", "imperative", POS_ANY, POS_VK},
};

struct CandidateTerm {
    std::string term;
    std::string reason;
    uint16_t reqPos;
};

static bool MatchesPosMask(const std::string &pos, uint16_t reqPos) {
    if (reqPos == POS_ANY || reqPos == (POS_V_TE | POS_V_DE)) return true;
    std::string p = pos;
    for (char &ch : p) {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    }
    if ((reqPos & POS_V1) && (p.find("ichidan") != std::string::npos || p.find("v1") != std::string::npos)) return true;
    if ((reqPos & POS_V5) && (p.find("godan") != std::string::npos || p.find("v5") != std::string::npos)) return true;
    if ((reqPos & POS_ADJ_I) && (p.find("keiyoushi") != std::string::npos || p.find("i-adj") != std::string::npos || p.find("adj-i") != std::string::npos)) return true;
    if ((reqPos & POS_ADJ_NA) && (p.find("adjectival") != std::string::npos || p.find("keiyodoshi") != std::string::npos || p.find("adj-na") != std::string::npos)) return true;
    if ((reqPos & POS_VK) && (p.find("kuru") != std::string::npos || p.find("vk") != std::string::npos)) return true;
    if ((reqPos & POS_VS) && (p.find("suru") != std::string::npos || p.find("vs") != std::string::npos)) return true;
    return false;
}

static bool HasKatakana(const std::string &s) {
    for (uint32_t cp : Utf8ToCps(s)) {
        if (cp >= 0x30A1 && cp <= 0x30F6) return true;
    }
    return false;
}

static void GenerateDeinflections(const std::string &surface, std::vector<CandidateTerm> &out) {
    out.clear();
    out.push_back({surface, "", POS_ANY});
    std::unordered_set<std::string> seenOut;
    seenOut.insert(surface + "|65535");

    auto cps = Utf8ToCps(surface);
    if (cps.empty()) return;
    uint32_t lastCp = cps.back();
    if (lastCp < 0x3041 || lastCp > 0x3096) return;

    struct QueueNode {
        std::string term;
        std::string reason;
        uint16_t curPos;
    };
    std::deque<QueueNode> q;
    q.push_back({surface, "", POS_ANY});
    std::unordered_set<std::string> visited;
    visited.insert(surface + "|65535");

    size_t expansions = 0;
    while (!q.empty() && expansions < 256) {
        QueueNode cur = std::move(q.front());
        q.pop_front();
        ++expansions;

        for (const auto &r : kRules) {
            if ((cur.curPos & r.reqMask) == 0) continue;
            if (!EndsWith(cur.term, r.kanaIn)) continue;
            if (cur.term.size() <= std::strlen(r.kanaIn) &&
                (r.outMask & (POS_VS | POS_VK | POS_V_TE | POS_V_DE)) == 0) {
                continue;
            }
            std::string cand = ReplaceSuffix(cur.term, r.kanaIn, r.kanaOut);
            if (cand.empty()) continue;

            std::string rsn = cur.reason.empty() ? r.reason : (std::string(r.reason) + " -> " + cur.reason);
            std::string key = cand + "|" + std::to_string(r.outMask);
            if (visited.insert(key).second) {
                q.push_back({cand, rsn, r.outMask});
            }
            if ((r.outMask & (POS_V_TE | POS_V_DE)) == 0 && seenOut.insert(key).second) {
                out.push_back({cand, rsn, r.outMask});
            }
        }
    }
}

static uint32_t LargeToSmallKana(uint32_t cp) {
    switch (cp) {
        case 0x3042: return 0x3041; // あ -> ぁ
        case 0x3044: return 0x3043; // い -> ぃ
        case 0x3046: return 0x3045; // う -> ぅ
        case 0x3048: return 0x3047; // え -> ぇ
        case 0x304A: return 0x3049; // お -> ぉ
        case 0x3064: return 0x3063; // つ -> っ
        case 0x3084: return 0x3083; // や -> ゃ
        case 0x3086: return 0x3085; // ゆ -> ゅ
        case 0x3088: return 0x3087; // よ -> ょ
        case 0x30A2: return 0x30A1; // ア -> ァ
        case 0x30A4: return 0x30A3; // イ -> ィ
        case 0x30A6: return 0x30A5; // ウ -> ゥ
        case 0x30A8: return 0x30A7; // エ -> ェ
        case 0x30AA: return 0x30A9; // オ -> ォ
        case 0x30C4: return 0x30C3; // ツ -> ッ
        case 0x30E4: return 0x30E3; // や -> ャ
        case 0x30E6: return 0x30E5; // ユ -> ュ
        case 0x30E8: return 0x30E7; // ヨ -> ョ
        default: return 0;
    }
}

static uint32_t UnvoiceRendaku(uint32_t cp) {
    // Hiragana dakuten -> unvoiced: か行, さ行, た行, は行
    if (cp == 0x304C || cp == 0x304E || cp == 0x3050 || cp == 0x3052 || cp == 0x3054) return cp - 1;
    if (cp == 0x3056 || cp == 0x3058 || cp == 0x305A || cp == 0x305C || cp == 0x305E) return cp - 1;
    if (cp == 0x3060 || cp == 0x3062 || cp == 0x3065 || cp == 0x3067 || cp == 0x3069) return cp - 1;
    if (cp == 0x3070 || cp == 0x3073 || cp == 0x3076 || cp == 0x3079 || cp == 0x307C) return cp - 1;
    // Hiragana handakuten -> unvoiced: は行 (ぱ->は, etc.)
    if (cp == 0x3071 || cp == 0x3074 || cp == 0x3077 || cp == 0x307A || cp == 0x307D) return cp - 2;
    // Katakana dakuten & handakuten (+0x60)
    if (cp == 0x30AC || cp == 0x30AE || cp == 0x30B0 || cp == 0x30B2 || cp == 0x30B4) return cp - 1;
    if (cp == 0x30B6 || cp == 0x30B8 || cp == 0x30BA || cp == 0x30BC || cp == 0x30BE) return cp - 1;
    if (cp == 0x30C0 || cp == 0x30C2 || cp == 0x30C5 || cp == 0x30C7 || cp == 0x30C9) return cp - 1;
    if (cp == 0x30D0 || cp == 0x30D3 || cp == 0x30D6 || cp == 0x30D9 || cp == 0x30DC) return cp - 1;
    if (cp == 0x30D1 || cp == 0x30D4 || cp == 0x30D7 || cp == 0x30DA || cp == 0x30DD) return cp - 2;
    return 0;
}

enum CoarsePos : uint8_t {
    CPOS_BOS = 0,
    CPOS_NOUN,
    CPOS_PROPER,
    CPOS_PREFIX,
    CPOS_SUFFIX,
    CPOS_VERB,
    CPOS_ADJ_I,
    CPOS_ADJ_NA,
    CPOS_ADV,
    CPOS_PARTICLE,
    CPOS_COPULA,
    CPOS_RENDAKU_NOUN,
    CPOS_UNK,
    CPOS_COUNT
};

static CoarsePos ClassifyPos(const DictEntry &e, bool isRendaku, bool isParticle, bool isDeinflected, const std::string &surf) {
    if (isParticle) {
        if (surf == "です" || surf == "ます" || surf == "だ") return CPOS_COPULA;
        return CPOS_PARTICLE;
    }
    if (isRendaku) return CPOS_RENDAKU_NOUN;
    std::string p = e.pos;
    for (char &ch : p) {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    }
    if (p.find("pokémon") != std::string::npos || p.find("pokemon") != std::string::npos ||
        p.find("custom") != std::string::npos || p.find("name (") != std::string::npos ||
        p.find("proper name") != std::string::npos) {
        return CPOS_PROPER;
    }
    if (p.find("prefix") != std::string::npos) return CPOS_PREFIX;
    if ((p.find("suffix") != std::string::npos || p.find("counter") != std::string::npos) &&
        p.find("noun") == std::string::npos) {
        return CPOS_SUFFIX;
    }
    if (p.find("particle") != std::string::npos) return CPOS_PARTICLE;
    if ((p.find("ichidan") != std::string::npos || p.find("godan") != std::string::npos ||
         p.find("kuru") != std::string::npos || p.find("verb") != std::string::npos) &&
        (p.find("noun") == std::string::npos || isDeinflected)) {
        return CPOS_VERB;
    }
    if (p.find("keiyoushi") != std::string::npos || p.find("i-adj") != std::string::npos ||
        p.find("adj-i") != std::string::npos) {
        return CPOS_ADJ_I;
    }
    if (p.find("keiyodoshi") != std::string::npos || p.find("adjectival") != std::string::npos ||
        p.find("adj-na") != std::string::npos) {
        return CPOS_ADJ_NA;
    }
    if (p.find("adverb") != std::string::npos || p.find("conjunction") != std::string::npos ||
        p.find("interjection") != std::string::npos || p.find("expression") != std::string::npos) {
        return CPOS_ADV;
    }
    return CPOS_NOUN;
}

static int TransCost(CoarsePos prev, CoarsePos cur) {
    if (prev == CPOS_BOS) {
        if (cur == CPOS_SUFFIX) return 40;
        if (cur == CPOS_PARTICLE) return 30;
        if (cur == CPOS_RENDAKU_NOUN) return 120;
        return 0;
    }
    if (cur == CPOS_RENDAKU_NOUN) {
        if (prev == CPOS_NOUN || prev == CPOS_PREFIX || prev == CPOS_ADJ_I ||
            prev == CPOS_ADJ_NA) {
            return -20;
        }
        if (prev == CPOS_PARTICLE || prev == CPOS_COPULA || prev == CPOS_UNK) {
            return 80;
        }
        return 0;
    }
    if (prev == CPOS_NOUN || prev == CPOS_PROPER || prev == CPOS_RENDAKU_NOUN) {
        if (cur == CPOS_PARTICLE) return -30;
        if (cur == CPOS_COPULA) return -25;
        if (prev != CPOS_RENDAKU_NOUN && cur == CPOS_SUFFIX) return -25;
        return 0;
    }
    if (prev == CPOS_PREFIX && cur == CPOS_NOUN) return -25;
    if ((prev == CPOS_ADJ_I || prev == CPOS_ADJ_NA) && cur == CPOS_NOUN) return -15;
    if (prev == CPOS_PARTICLE) {
        if (cur == CPOS_PARTICLE) return 15;
        if (cur == CPOS_NOUN || cur == CPOS_VERB || cur == CPOS_ADJ_I ||
            cur == CPOS_ADJ_NA || cur == CPOS_ADV || cur == CPOS_PROPER) {
            return -15;
        }
        return 0;
    }
    if (prev == CPOS_VERB && (cur == CPOS_PARTICLE || cur == CPOS_COPULA || cur == CPOS_NOUN)) return -10;
    return 0;
}

static int NodeCost(const DictEntry &e, int len, uint32_t firstCp,
                    bool isParticle, bool isDeinflected, bool isRendaku)
{
    if (isParticle) {
        return (len == 1) ? 45 : (100 - 38 * len);
    }
    int cost = 100;
    if (len == 1) {
        cost += IsKanji(firstCp) ? 30 : 160;
    } else {
        cost -= 38 * len;
    }
    bool isCustomOrPoke = (e.pos.find("Pokémon") != std::string::npos ||
                           e.pos.find("Pokemon") != std::string::npos ||
                           e.pos.find("Custom") != std::string::npos);
    bool isProper = (!isCustomOrPoke &&
                     (e.pos.compare(0, 6, "Name (") == 0 ||
                      e.pos.compare(0, 11, "Proper Name") == 0));
    if (isCustomOrPoke) {
        cost -= 90;
    } else if (isProper) {
        cost += 65;
    } else if (e.freq > 0) {
        if (e.freq <= 500) cost -= 45;
        else if (e.freq <= 2000) cost -= 35;
        else if (e.freq <= 6000) cost -= 25;
        else if (e.freq <= 15000) cost -= 15;
        else if (e.freq <= 40000) cost -= 5;
        else cost += 5;
    } else {
        cost += 25;
    }
    if (isDeinflected) cost -= 10;
    if (isRendaku) cost += (len >= 3) ? 45 : 90;
    return cost;
}

} // namespace

std::string KatakanaToHiragana(const std::string &utf8) {
    std::vector<uint32_t> cps = Utf8ToCps(utf8);
    for (uint32_t &cp : cps) {
        if (cp >= 0x30A1 && cp <= 0x30F6) cp -= 0x60;
    }
    return CpsToUtf8(cps, 0, cps.size());
}

std::vector<std::string> WrapPopupLines(const std::string &text, int maxCols) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string para = (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);
        if (para.empty()) {
            lines.push_back("");
        } else {
            std::vector<uint32_t> cps = Utf8ToCps(para);
            size_t pos = 0;
            while (pos < cps.size()) {
                int cols = 0;
                size_t end = pos;
                size_t lastSpace = std::string::npos;
                while (end < cps.size()) {
                    int w = (cps[end] > 0x7E) ? 2 : 1;
                    if (cols + w > maxCols && end > pos) break;
                    if (cps[end] == ' ') lastSpace = end;
                    cols += w;
                    ++end;
                }
                if (end < cps.size() && lastSpace != std::string::npos && lastSpace > pos) {
                    lines.push_back(CpsToUtf8(cps, pos, lastSpace - pos));
                    pos = lastSpace + 1;
                } else {
                    if (end == pos) end = pos + 1;
                    lines.push_back(CpsToUtf8(cps, pos, end - pos));
                    pos = end;
                }
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

bool DictionaryEngine::Open(const char *jmdictPath, const char *kanjidicPath, std::string &err) {
    Close();
    if (!jmdictFile_.OpenRead(jmdictPath)) {
        err = "Missing " + std::string(jmdictPath);
        return false;
    }
    uint8_t hdr[28];
    if (!jmdictFile_.ReadExact(hdr, 28) || std::memcmp(hdr, "JMDICTOF", 8) != 0) {
        jmdictFile_.Close();
        err = "Invalid JMdict header";
        return false;
    }
    uint32_t ver = RdU32(hdr + 8);
    numBlocks_ = RdU32(hdr + 12);
    uint32_t numSparse = RdU32(hdr + 16);
    uint32_t sparseBytes = RdU32(hdr + 20);
    keyBlobBytes_ = RdU32(hdr + 24);
    if (ver != 1 || numSparse == 0) {
        jmdictFile_.Close();
        err = "Unsupported JMdict version";
        return false;
    }

    sparseFileOff_ = 28;
    keyBlobFileOff_ = sparseFileOff_ + sparseBytes;
    blockOffTableFileOff_ = keyBlobFileOff_ + keyBlobBytes_;

    std::vector<uint8_t> sparseBuf(sparseBytes);
    if (!jmdictFile_.ReadExact(sparseBuf.data(), sparseBytes)) {
        jmdictFile_.Close();
        err = "Failed to read JMdict sparse index";
        return false;
    }
    sparseKeys_.reserve(numSparse);
    size_t sp = 0;
    for (uint32_t i = 0; i < numSparse && sp + 5 <= sparseBytes; ++i) {
        SparseKey sk;
        sk.keyOff = RdU32(sparseBuf.data() + sp);
        uint8_t kl = sparseBuf[sp + 4];
        sp += 5;
        if (sp + kl > sparseBytes) break;
        sk.key.assign((const char *)(sparseBuf.data() + sp), kl);
        sp += kl;
        sparseKeys_.push_back(std::move(sk));
    }

    cache_.resize(16);
    keyCache_.resize(16);

    if (kanjiFile_.OpenRead(kanjidicPath)) {
        uint8_t khdr[16];
        if (kanjiFile_.ReadExact(khdr, 16) &&
            (std::memcmp(khdr, "KANJIOF2", 8) == 0 || std::memcmp(khdr, "KANJIOFF", 8) == 0)) {
            kanjiCount_ = RdU32(khdr + 12);
        } else {
            kanjiFile_.Close();
        }
    }
    return true;
}

void DictionaryEngine::Close() {
    jmdictFile_.Close();
    kanjiFile_.Close();
    sparseKeys_.clear();
    cache_.clear();
    keyCache_.clear();
    numBlocks_ = 0;
    kanjiCount_ = 0;
}

bool DictionaryEngine::ReadBlock(uint32_t blockId, const std::vector<uint8_t> *&outData) {
    if (!jmdictFile_.IsOpen() || blockId >= numBlocks_) return false;
    ++cacheStamp_;
    int lruIdx = 0;
    uint32_t minStamp = 0xFFFFFFFFu;
    for (size_t i = 0; i < cache_.size(); ++i) {
        if (cache_[i].blockId == blockId) {
            cache_[i].stamp = cacheStamp_;
            outData = &cache_[i].data;
            return true;
        }
        if (cache_[i].stamp < minStamp) {
            minStamp = cache_[i].stamp;
            lruIdx = (int)i;
        }
    }

    uint8_t offBuf[8];
    if (!jmdictFile_.SeekSet(blockOffTableFileOff_ + (uint64_t)blockId * 4) ||
        !jmdictFile_.ReadExact(offBuf, 8)) return false;
    uint32_t bStart = RdU32(offBuf);
    uint32_t bEnd = RdU32(offBuf + 4);
    if (bEnd <= bStart || bEnd - bStart > 32768) return false;

    std::vector<uint8_t> comp(bEnd - bStart);
    if (!jmdictFile_.SeekSet(bStart) || !jmdictFile_.ReadExact(comp.data(), comp.size())) return false;

    std::vector<uint8_t> raw(16384);
    uLongf destLen = (uLongf)raw.size();
    if (uncompress(raw.data(), &destLen, comp.data(), (uLong)comp.size()) != Z_OK) {
        return false;
    }
    raw.resize((size_t)destLen);

    cache_[lruIdx].blockId = blockId;
    cache_[lruIdx].stamp = cacheStamp_;
    cache_[lruIdx].data = std::move(raw);
    outData = &cache_[lruIdx].data;
    return true;
}

bool DictionaryEngine::DecodeEntryAt(uint32_t blockId, uint16_t offInBlock, DictEntry &out) {
    const std::vector<uint8_t> *blk = nullptr;
    if (!ReadBlock(blockId, blk) || !blk) return false;
    if ((size_t)offInBlock + 10 > blk->size()) return false;
    const uint8_t *p = blk->data() + offInBlock;
    uint32_t freq = RdU32(p);
    uint8_t hwLen = p[4], rdLen = p[5], posLen = p[6], jlptLen = p[7];
    uint16_t glLen = RdU16(p + 8);
    size_t pos = offInBlock + 10;
    if (pos + hwLen + rdLen + posLen + jlptLen + glLen > blk->size()) return false;

    out.headword.assign((const char *)(blk->data() + pos), hwLen); pos += hwLen;
    out.reading.assign((const char *)(blk->data() + pos), rdLen);  pos += rdLen;
    out.pos.assign((const char *)(blk->data() + pos), posLen);      pos += posLen;
    std::string jlpt((const char *)(blk->data() + pos), jlptLen);   pos += jlptLen;
    std::string gloss((const char *)(blk->data() + pos), glLen);

    out.freq = freq;
    if (!jlpt.empty() && freq > 0) out.rankJlpt = jlpt + " #" + std::to_string(freq);
    else if (!jlpt.empty()) out.rankJlpt = jlpt;
    else if (freq > 0) out.rankJlpt = "#" + std::to_string(freq);
    else out.rankJlpt.clear();

    out.wrappedLines = WrapPopupLines(gloss, 38);
    out.found = true;
    return true;
}

bool DictionaryEngine::LookupSurface(const std::string &surface, std::vector<DictEntry> &out, int maxResults) {
    if (!jmdictFile_.IsOpen() || surface.empty() || sparseKeys_.empty()) return false;

    // Binary search sparseKeys_ for largest index i where sparseKeys_[i].key <= surface
    size_t lo = 0, hi = sparseKeys_.size();
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (sparseKeys_[mid].key <= surface) lo = mid;
        else hi = mid;
    }

    uint32_t startOff = sparseKeys_[lo].keyOff;
    uint32_t endOff = (lo + 1 < sparseKeys_.size()) ? sparseKeys_[lo + 1].keyOff : keyBlobBytes_;
    if (endOff <= startOff || endOff - startOff > 65536) return false;

    const std::vector<uint8_t> *chunkPtr = nullptr;
    ++cacheStamp_;
    int lruIdx = 0;
    uint32_t minStamp = 0xFFFFFFFFu;
    for (size_t i = 0; i < keyCache_.size(); ++i) {
        if (keyCache_[i].blockId == (uint32_t)lo) {
            keyCache_[i].stamp = cacheStamp_;
            chunkPtr = &keyCache_[i].data;
            break;
        }
        if (keyCache_[i].stamp < minStamp) {
            minStamp = keyCache_[i].stamp;
            lruIdx = (int)i;
        }
    }
    if (!chunkPtr) {
        std::vector<uint8_t> tmp(endOff - startOff);
        if (!jmdictFile_.SeekSet(keyBlobFileOff_ + startOff) ||
            !jmdictFile_.ReadExact(tmp.data(), tmp.size())) return false;
        keyCache_[lruIdx].blockId = (uint32_t)lo;
        keyCache_[lruIdx].stamp = cacheStamp_;
        keyCache_[lruIdx].data = std::move(tmp);
        chunkPtr = &keyCache_[lruIdx].data;
    }
    const std::vector<uint8_t> &chunk = *chunkPtr;

    size_t p = 0;
    while (p + 2 <= chunk.size()) {
        uint8_t kl = chunk[p++];
        if (p + kl + 1 > chunk.size()) break;
        std::string key((const char *)(chunk.data() + p), kl);
        p += kl;
        uint8_t cnt = chunk[p++];
        if (p + (size_t)cnt * 6 > chunk.size()) break;

        if (key == surface) {
            int added = 0;
            for (uint8_t i = 0; i < cnt && added < maxResults; ++i) {
                uint32_t bid = RdU32(chunk.data() + p + i * 6);
                uint16_t off = RdU16(chunk.data() + p + i * 6 + 4);
                DictEntry e;
                if (DecodeEntryAt(bid, off, e)) {
                    e.matchedSurface = surface;
                    out.push_back(std::move(e));
                    ++added;
                }
            }
            return added > 0;
        }
        if (key > surface) break;
        p += (size_t)cnt * 6;
    }
    return false;
}

bool DictionaryEngine::LookupKanjiDetails(uint32_t cp, KanjiDetails &out) {
    out = KanjiDetails();
    if (!kanjiFile_.IsOpen() || kanjiCount_ == 0) return false;
    uint32_t lo = 0, hi = kanjiCount_;
    uint8_t idxBuf[10];
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (!kanjiFile_.SeekSet(16 + (uint64_t)mid * 10) || !kanjiFile_.ReadExact(idxBuf, 10)) return false;
        uint32_t mcp = RdU32(idxBuf);
        if (mcp == cp) {
            uint32_t dataOff = RdU32(idxBuf + 4);
            uint16_t recLen = RdU16(idxBuf + 8);
            if (recLen < 21 || recLen > 20480) return false;
            std::vector<uint8_t> rec(recLen);
            if (!kanjiFile_.SeekSet(dataOff) || !kanjiFile_.ReadExact(rec.data(), recLen)) return false;

            const uint8_t *r = rec.data();
            out.cp = cp;
            out.literal = CpsToUtf8({cp}, 0, 1);
            out.freq = RdU16(r + 0);
            out.radNum = RdU16(r + 2);
            out.strokes = r[4];
            out.grade = r[5];
            out.jlpt = r[6];
            uint8_t rcL = r[7], rbL = r[8], rrL = r[9], rmL = r[10];
            uint8_t onL = r[11], kunL = r[12], nanL = r[13], phL = r[14];
            uint16_t mnL = RdU16(r + 15), cpL = RdU16(r + 17), stZ = RdU16(r + 19);
            size_t p = 21;
            if (p + rcL + rbL + rrL + rmL + onL + kunL + nanL + phL + mnL + cpL + stZ > recLen) return false;

            out.radChar.assign((const char *)(r + p), rcL); p += rcL;
            out.radBase.assign((const char *)(r + p), rbL); p += rbL;
            out.radReading.assign((const char *)(r + p), rrL); p += rrL;
            out.radMeaning.assign((const char *)(r + p), rmL); p += rmL;
            out.onReadings.assign((const char *)(r + p), onL); p += onL;
            out.kunReadings.assign((const char *)(r + p), kunL); p += kunL;
            out.nanori.assign((const char *)(r + p), nanL); p += nanL;
            out.phonetic.assign((const char *)(r + p), phL); p += phL;
            out.meanings.assign((const char *)(r + p), mnL); p += mnL;
            std::string compJoined((const char *)(r + p), cpL); p += cpL;

            size_t cs = 0;
            while (cs < compJoined.size()) {
                size_t sep = compJoined.find('\x1f', cs);
                if (sep == std::string::npos) {
                    out.components.push_back(compJoined.substr(cs));
                    break;
                }
                if (sep > cs) out.components.push_back(compJoined.substr(cs, sep - cs));
                cs = sep + 1;
            }

            if (stZ > 0) {
                std::vector<uint8_t> rawBits(14400);
                uLongf dlen = 14400;
                if (uncompress(rawBits.data(), &dlen, r + p, stZ) == Z_OK && (dlen == 14400 || dlen == 1800)) {
                    rawBits.resize((size_t)dlen);
                    out.strokeBits120 = std::move(rawBits);
                }
            }
            out.found = true;
            return true;
        }
        if (mcp < cp) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

bool DictionaryEngine::LookupKanji(uint32_t cp, DictEntry &outEntry) {
    KanjiDetails kd;
    if (!LookupKanjiDetails(cp, kd)) return false;
    outEntry.headword = kd.literal;
    outEntry.matchedSurface = kd.literal;
    outEntry.contextText = kd.literal;
    outEntry.reading = kd.onReadings.empty() ? kd.kunReadings : kd.onReadings;
    outEntry.pos = "Kanji (" + std::to_string(kd.strokes) + " strokes"
                 + (kd.grade > 0 ? (", Grade " + std::to_string(kd.grade)) : "") + ")";
    if (kd.jlpt > 0) outEntry.rankJlpt = "N" + std::to_string(kd.jlpt);
    else if (kd.freq > 0) outEntry.rankJlpt = "#" + std::to_string(kd.freq);
    else outEntry.rankJlpt.clear();

    std::string body;
    if (!kd.meanings.empty()) body += "• Meanings: " + kd.meanings + "\n";
    if (!kd.onReadings.empty()) body += "音: " + kd.onReadings + "\n";
    if (!kd.kunReadings.empty()) body += "訓: " + kd.kunReadings + "\n";
    if (!kd.components.empty()) {
        body += "Components: ";
        for (size_t i = 0; i < kd.components.size(); ++i) {
            if (i > 0) body += "  ";
            body += kd.components[i];
        }
    }
    outEntry.wrappedLines = WrapPopupLines(body, 38);
    outEntry.found = true;
    return true;
}

bool DictionaryEngine::LookupWordAt(const std::string &lineUtf8, int charIdx,
                                    std::vector<DictEntry> &outEntries)
{
    outEntries.clear();
    std::vector<uint32_t> cps = Utf8ToCps(lineUtf8);
    if (cps.empty()) return false;
    if (charIdx < 0) charIdx = 0;
    if (charIdx >= (int)cps.size()) charIdx = (int)cps.size() - 1;

    // Try charIdx, then charIdx-1, then charIdx+1 if needed
    int starts[3] = {charIdx, std::max(0, charIdx - 1), std::min((int)cps.size() - 1, charIdx + 1)};
    std::unordered_set<std::string> seenHw;

    for (int sIdx = 0; sIdx < 3; ++sIdx) {
        int pos = starts[sIdx];
        if (sIdx > 0 && !outEntries.empty()) break;

        int maxLen = (int)(cps.size() - pos);
        for (int len = maxLen; len >= 1; --len) {
            std::string sub = CpsToUtf8(cps, (size_t)pos, (size_t)len);
            if (sub.size() > 120) continue;

            auto trySurface = [&](const std::string &qSurf, const char *extraTag) {
                std::vector<CandidateTerm> cands;
                GenerateDeinflections(qSurf, cands);
                for (const auto &c : cands) {
                    std::vector<DictEntry> hits;
                    if (LookupSurface(c.term, hits, 64)) {
                        for (auto &h : hits) {
                            if (!c.reason.empty() && !MatchesPosMask(h.pos, c.reqPos)) continue;
                            std::string key = h.headword + "|" + h.reading;
                            if (!seenHw.insert(key).second) continue;
                            h.matchedSurface = sub;
                            h.contextText = lineUtf8;
                            std::string prefix;
                            if (!c.reason.empty()) {
                                prefix = "[" + c.reason + "]";
                            }
                            if (extraTag && *extraTag) {
                                std::string tagStr = std::string("[") + extraTag + "]";
                                if (h.pos.find(tagStr) == std::string::npos) {
                                    prefix = prefix.empty() ? tagStr : (prefix + " " + tagStr);
                                }
                            }
                            if (!prefix.empty()) {
                                h.pos = h.pos.empty() ? prefix : (prefix + " " + h.pos);
                            }
                            outEntries.push_back(std::move(h));
                        }
                    }
                }
            };

            size_t beforeDirect = outEntries.size();
            trySurface(sub, nullptr);
            if (HasKatakana(sub)) {
                std::string hira = KatakanaToHiragana(sub);
                if (hira != sub) trySurface(hira, nullptr);
            }
            if (outEntries.size() == beforeDirect && len >= 2) {
                uint32_t unv = UnvoiceRendaku(cps[pos]);
                if (unv != 0) {
                    std::vector<uint32_t> rCps(cps.begin() + pos, cps.begin() + pos + len);
                    rCps[0] = unv;
                    std::string rSub = CpsToUtf8(rCps, 0, rCps.size());
                    trySurface(rSub, "rendaku");
                    if (HasKatakana(rSub)) {
                        std::string rHira = KatakanaToHiragana(rSub);
                        if (rHira != rSub) trySurface(rHira, "rendaku");
                    }
                }
            }

            if (len >= 2 && len <= 12) {
                std::vector<int> candIdx;
                for (int k = 1; k < len; ++k) {
                    if (LargeToSmallKana(cps[pos + k]) != 0) candIdx.push_back(k);
                }
                if (!candIdx.empty()) {
                    std::vector<uint32_t> subCps(cps.begin() + pos, cps.begin() + pos + len);
                    auto tryVar = [&](const std::vector<uint32_t> &vCps) {
                        std::string vSub = CpsToUtf8(vCps, 0, vCps.size());
                        trySurface(vSub, nullptr);
                        if (HasKatakana(vSub)) {
                            std::string vHira = KatakanaToHiragana(vSub);
                            if (vHira != vSub) trySurface(vHira, nullptr);
                        }
                    };
                    for (size_t i = 0; i < candIdx.size(); ++i) {
                        int k = candIdx[i];
                        uint32_t orig = subCps[k];
                        subCps[k] = LargeToSmallKana(orig);
                        tryVar(subCps);
                        subCps[k] = orig;
                    }
                    if (len <= 10 && candIdx.size() >= 2) {
                        int pairs = 0;
                        for (size_t i = 0; i < candIdx.size() && pairs < 15; ++i) {
                            for (size_t j = i + 1; j < candIdx.size() && pairs < 15; ++j) {
                                int k1 = candIdx[i], k2 = candIdx[j];
                                uint32_t o1 = subCps[k1], o2 = subCps[k2];
                                subCps[k1] = LargeToSmallKana(o1);
                                subCps[k2] = LargeToSmallKana(o2);
                                tryVar(subCps);
                                subCps[k1] = o1;
                                subCps[k2] = o2;
                                ++pairs;
                            }
                        }
                    }
                }
            }
        }
    }

    // Also append Kanji breakdown for kanji characters in the matched surface (or under cursor)
    std::string targetForKanji = outEntries.empty() ? CpsToUtf8(cps, (size_t)charIdx, 1) : outEntries[0].matchedSurface;
    for (uint32_t cp : Utf8ToCps(targetForKanji)) {
        if (IsKanji(cp)) {
            DictEntry ke;
            if (LookupKanji(cp, ke)) {
                ke.contextText = lineUtf8;
                outEntries.push_back(std::move(ke));
            }
        }
    }

    if (outEntries.empty()) {
        DictEntry nf;
        nf.headword = CpsToUtf8(cps, (size_t)charIdx, 1);
        nf.matchedSurface = nf.headword;
        nf.contextText = lineUtf8;
        nf.pos = "No offline match";
        nf.wrappedLines = WrapPopupLines("OCR: " + lineUtf8 + "\nNo dictionary entry found.", 38);
        nf.found = false;
        outEntries.push_back(std::move(nf));
    }
    return true;
}

bool DictionaryEngine::GlossRegion(const std::vector<std::string> &linesUtf8,
                                   std::vector<DictEntry> &outEntries)
{
    outEntries.clear();
    std::string fullText;
    for (size_t i = 0; i < linesUtf8.size(); ++i) {
        if (i > 0) fullText += " ";
        fullText += linesUtf8[i];
    }

    std::vector<DictEntry> vocabEntries;
    std::unordered_set<std::string> seenHw;
    std::string glossBody = "── OCR Text ──\n" + fullText + "\n── Word Gloss ──\n";

    struct ParticleGloss { const char *surf; const char *gloss; };
    static const ParticleGloss kParticles[] = {
        {"から", "[from / because]"}, {"まで", "[until / to]"}, {"より", "[than / from]"},
        {"です", "[polite copula]"}, {"ます", "[polite verb]"},
        {"は", "[topic]"}, {"が", "[subject]"}, {"を", "[object]"}, {"に", "[to / at / in]"},
        {"へ", "[toward]"}, {"で", "[at / by / with]"}, {"と", "[and / with / quote]"},
        {"も", "[also / too]"}, {"の", "[of / 's]"}, {"か", "[question]"},
        {"ね", "[right?]"}, {"よ", "[emphasis]"}, {"だ", "[copula]"}, {"な", "[adj / don't]"},
    };

    struct LatticeEdge {
        int endPos;
        CoarsePos cpos;
        int cost;
        bool isParticle;
        bool isRendaku;
        std::string surface;
        std::string particleGloss;
        DictEntry entry;
    };

    for (const std::string &line : linesUtf8) {
        std::vector<uint32_t> cps = Utf8ToCps(line);
        size_t pos = 0;
        while (pos < cps.size()) {
            if (!IsJapaneseChar(cps[pos])) {
                ++pos;
                continue;
            }
            size_t spanEnd = pos;
            while (spanEnd < cps.size() && IsJapaneseChar(cps[spanEnd]) && (spanEnd - pos) < 64) {
                ++spanEnd;
            }
            int N = (int)(spanEnd - pos);
            std::vector<std::vector<LatticeEdge>> edges((size_t)N);

            for (int i = 0; i < N; ++i) {
                int maxLen = std::min(12, N - i);
                bool hasOneChar = false;
                for (int len = 1; len <= maxLen; ++len) {
                    std::string sub = CpsToUtf8(cps, pos + (size_t)i, (size_t)len);
                    if (len <= 2) {
                        for (const auto &pg : kParticles) {
                            if (sub == pg.surf) {
                                LatticeEdge pe;
                                pe.endPos = i + len;
                                pe.cpos = ClassifyPos(DictEntry(), false, true, false, sub);
                                pe.cost = NodeCost(DictEntry(), len, cps[pos + (size_t)i], true, false, false);
                                pe.isParticle = true;
                                pe.isRendaku = false;
                                pe.surface = sub;
                                pe.particleGloss = pg.gloss;
                                edges[i].push_back(std::move(pe));
                                if (len == 1) hasOneChar = true;
                                break;
                            }
                        }
                    }

                    auto addBestCandidate = [&](const std::string &qSurf, bool isRendaku) -> bool {
                        std::vector<CandidateTerm> cands;
                        GenerateDeinflections(qSurf, cands);
                        bool foundAny = false;
                        LatticeEdge bestEdge;
                        bestEdge.cost = 1000000000;
                        for (const auto &c : cands) {
                            std::vector<DictEntry> hits;
                            if (!LookupSurface(c.term, hits, 6)) continue;
                            for (auto &h : hits) {
                                if (!c.reason.empty() && !MatchesPosMask(h.pos, c.reqPos)) continue;
                                bool isDeinf = !c.reason.empty();
                                CoarsePos cpCat = ClassifyPos(h, isRendaku, false, isDeinf, sub);
                                if (isRendaku) {
                                    CoarsePos baseCat = ClassifyPos(h, false, false, isDeinf, sub);
                                    if (baseCat == CPOS_PROPER || baseCat == CPOS_PARTICLE) continue;
                                    if (h.pos.find("expression") != std::string::npos ||
                                        h.pos.find("Expressions") != std::string::npos) continue;
                                }
                                int sc = NodeCost(h, len, cps[pos + (size_t)i], false, isDeinf, isRendaku);
                                if (sc < bestEdge.cost) {
                                    std::string prefix;
                                    if (isDeinf) prefix = "[" + c.reason + "]";
                                    if (isRendaku && h.pos.find("[rendaku]") == std::string::npos) {
                                        prefix = prefix.empty() ? "[rendaku]" : (prefix + " [rendaku]");
                                    }
                                    if (!prefix.empty()) {
                                        h.pos = h.pos.empty() ? prefix : (prefix + " " + h.pos);
                                    }
                                    bestEdge.endPos = i + len;
                                    bestEdge.cpos = cpCat;
                                    bestEdge.cost = sc;
                                    bestEdge.isParticle = false;
                                    bestEdge.isRendaku = isRendaku;
                                    bestEdge.surface = sub;
                                    bestEdge.entry = std::move(h);
                                    foundAny = true;
                                }
                            }
                        }
                        if (foundAny) {
                            edges[i].push_back(std::move(bestEdge));
                            if (len == 1) hasOneChar = true;
                        }
                        return foundAny;
                    };

                    bool directHit = addBestCandidate(sub, false);
                    if (!directHit && HasKatakana(sub)) {
                        std::string hira = KatakanaToHiragana(sub);
                        if (hira != sub) directHit = addBestCandidate(hira, false);
                    }
                    if (!directHit && len >= 2 && i > 0) {
                        uint32_t unv = UnvoiceRendaku(cps[pos + (size_t)i]);
                        if (unv != 0) {
                            std::vector<uint32_t> rCps(cps.begin() + (ptrdiff_t)(pos + (size_t)i),
                                                       cps.begin() + (ptrdiff_t)(pos + (size_t)i + (size_t)len));
                            rCps[0] = unv;
                            std::string rSub = CpsToUtf8(rCps, 0, rCps.size());
                            if (!addBestCandidate(rSub, true) && HasKatakana(rSub)) {
                                std::string rHira = KatakanaToHiragana(rSub);
                                if (rHira != rSub) addBestCandidate(rHira, true);
                            }
                        }
                    }
                }
                if (!hasOneChar) {
                    LatticeEdge unk;
                    unk.endPos = i + 1;
                    unk.cpos = CPOS_UNK;
                    unk.cost = 260;
                    unk.isParticle = false;
                    unk.isRendaku = false;
                    unk.surface = CpsToUtf8(cps, pos + (size_t)i, 1);
                    edges[i].push_back(std::move(unk));
                }
            }

            const int INF = 1000000000;
            std::vector<std::vector<int>> dp((size_t)N + 1, std::vector<int>(CPOS_COUNT, INF));
            struct BackPtr { int prevPos; int prevCpos; int edgeIdx; };
            std::vector<std::vector<BackPtr>> bp((size_t)N + 1, std::vector<BackPtr>(CPOS_COUNT, {-1, -1, -1}));
            dp[0][CPOS_BOS] = 0;

            for (int i = 0; i < N; ++i) {
                for (int pc = 0; pc < CPOS_COUNT; ++pc) {
                    if (dp[i][pc] >= INF) continue;
                    for (int ei = 0; ei < (int)edges[i].size(); ++ei) {
                        const LatticeEdge &ed = edges[i][ei];
                        int candCost = dp[i][pc] + TransCost((CoarsePos)pc, ed.cpos) + ed.cost;
                        if (candCost < dp[ed.endPos][ed.cpos]) {
                            dp[ed.endPos][ed.cpos] = candCost;
                            bp[ed.endPos][ed.cpos] = {i, pc, ei};
                        }
                    }
                }
            }

            int bestC = 0;
            for (int c = 1; c < CPOS_COUNT; ++c) {
                if (dp[N][c] < dp[N][bestC]) bestC = c;
            }

            std::vector<const LatticeEdge *> chosen;
            int curPos = N, curC = bestC;
            while (curPos > 0 && curC >= 0) {
                BackPtr b = bp[curPos][curC];
                if (b.prevPos < 0) break;
                chosen.push_back(&edges[b.prevPos][b.edgeIdx]);
                curPos = b.prevPos;
                curC = b.prevCpos;
            }
            std::reverse(chosen.begin(), chosen.end());

            for (size_t ti = 0; ti < chosen.size(); ++ti) {
                const LatticeEdge *ed = chosen[ti];
                if (ed->cpos == CPOS_UNK) continue;
                if (ed->isParticle) {
                    glossBody += "• " + ed->surface + ": " + ed->particleGloss + "\n";
                    continue;
                }
                DictEntry best = ed->entry;
                std::string surf = ed->surface;
                // Merge trailing な onto na-adjective if the next token is the 1-char particle な
                if (ed->cpos == CPOS_ADJ_NA && ti + 1 < chosen.size() &&
                    chosen[ti + 1]->isParticle && chosen[ti + 1]->surface == "な") {
                    surf += "な";
                    ++ti;
                }
                best.matchedSurface = surf;
                best.contextText = fullText;
                std::string firstGloss = best.wrappedLines.empty() ? "" : best.wrappedLines[0];
                if (firstGloss.size() > 48) firstGloss = firstGloss.substr(0, 45) + "...";
                glossBody += "• " + surf;
                if (!best.reading.empty() && best.reading != surf) glossBody += " [" + best.reading + "]";
                glossBody += ": " + firstGloss + "\n";

                std::string key = best.headword + "|" + best.reading;
                if (seenHw.insert(key).second) {
                    vocabEntries.push_back(std::move(best));
                }
            }

            pos = spanEnd;
        }
    }

    DictEntry summary;
    summary.headword = "Selection Translation";
    summary.matchedSurface = fullText;
    summary.contextText = fullText;
    summary.pos = "Offline Word-by-Word Gloss (L/R for Vocab)";
    summary.wrappedLines = WrapPopupLines(glossBody, 38);
    summary.found = !vocabEntries.empty();
    outEntries.push_back(std::move(summary));

    for (auto &v : vocabEntries) {
        outEntries.push_back(std::move(v));
    }
    return true;
}

} // namespace OfflineDict
