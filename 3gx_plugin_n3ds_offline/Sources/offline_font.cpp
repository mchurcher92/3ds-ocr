#include "offline_font.hpp"

#include <cstdio>
#include <cstring>

namespace OfflineFont {

namespace {

static inline uint16_t RdU16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t RdU32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t RdU64(const uint8_t *p) {
    uint64_t lo = RdU32(p);
    uint64_t hi = RdU32(p + 4);
    return lo | (hi << 32);
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

} // namespace

bool FontPack::Open(const char *fontPackPath, std::string &err) {
    Close();
    if (!file_.OpenRead(fontPackPath)) {
        err = "Missing " + std::string(fontPackPath);
        return false;
    }
    uint8_t hdr[16];
    if (!file_.ReadExact(hdr, 16) || std::memcmp(hdr, "FONTPACK", 8) != 0) {
        file_.Close();
        err = "Invalid font_pack header";
        return false;
    }
    uint32_t ver = RdU32(hdr + 8);
    count_ = RdU32(hdr + 12);
    if (ver != 1 || count_ == 0) {
        file_.Close();
        err = "Unsupported font_pack version";
        return false;
    }

    // Build sparse index (every 64th record) in RAM (~5 KB) via sequential 14B*64*16 chunks
    sparseKeys_.clear();
    const uint32_t numSparse = (count_ + 63) / 64;
    sparseKeys_.reserve(numSparse);
    const size_t chunkRecs = 1024;
    std::vector<uint8_t> buf(chunkRecs * 14);
    for (uint32_t base = 0; base < count_; base += (uint32_t)chunkRecs) {
        uint32_t take = std::min<uint32_t>((uint32_t)chunkRecs, count_ - base);
        if (!file_.ReadExact(buf.data(), (size_t)take * 14)) {
            file_.Close();
            sparseKeys_.clear();
            err = "Failed reading font_pack index";
            return false;
        }
        for (uint32_t i = 0; i < take; i += 64) {
            sparseKeys_.push_back(RdU64(buf.data() + (size_t)i * 14));
        }
    }
    return true;
}

void FontPack::Close() {
    file_.Close();
    count_ = 0;
    sparseKeys_.clear();
    cache_.clear();
}

bool FontPack::FindGlyph(uint64_t key, GlyphData &out) {
    if (!file_.IsOpen() || count_ == 0 || sparseKeys_.empty()) return false;
    auto cit = cache_.find(key);
    if (cit != cache_.end()) {
        out = cit->second;
        return true;
    }

    // Binary search sparseKeys_ in RAM (0 SD seeks)
    size_t slo = 0, shi = sparseKeys_.size();
    while (slo + 1 < shi) {
        size_t smid = slo + (shi - slo) / 2;
        if (sparseKeys_[smid] <= key) slo = smid;
        else shi = smid;
    }

    uint32_t startIdx = (uint32_t)(slo * 64);
    uint32_t sliceCnt = std::min<uint32_t>(64, count_ - startIdx);
    uint8_t sliceBuf[64 * 14];
    if (!file_.SeekSet(16 + (uint64_t)startIdx * 14) ||
        !file_.ReadExact(sliceBuf, (size_t)sliceCnt * 14)) {
        return false;
    }

    uint32_t lo = 0, hi = sliceCnt;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *rec = sliceBuf + (size_t)mid * 14;
        uint64_t mkey = RdU64(rec);
        if (mkey == key) {
            uint32_t off = RdU32(rec + 8);
            uint16_t len = RdU16(rec + 12);
            if (len < 5 || len > 1024) return false;
            std::vector<uint8_t> blob(len);
            if (!file_.SeekSet(off) || !file_.ReadExact(blob.data(), len)) return false;
            out.width = blob[0];
            out.height = blob[1];
            out.offsetX = (int8_t)blob[2];
            out.offsetY = (int8_t)blob[3];
            out.advanceX = blob[4];
            out.packed.assign(blob.begin() + 5, blob.end());
            if (cache_.size() < 1024) {
                cache_.emplace(key, out);
            }
            return true;
        }
        if (mkey < key) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

void FontPack::LoadGlyphsForText(uint8_t fontId, const std::string &text,
                                 std::unordered_map<uint64_t, GlyphData> &outMap)
{
    if (!file_.IsOpen()) return;
    for (uint32_t cp : Utf8ToCps(text)) {
        uint64_t key = ((uint64_t)fontId << 32) | (uint64_t)cp;
        if (outMap.find(key) != outMap.end()) continue;
        GlyphData gd;
        if (FindGlyph(key, gd)) {
            outMap.emplace(key, std::move(gd));
        }
    }
}

} // namespace OfflineFont
