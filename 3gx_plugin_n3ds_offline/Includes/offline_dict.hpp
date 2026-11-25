#ifndef OFFLINE_DICT_HPP
#define OFFLINE_DICT_HPP

#include <cstdint>
#include <string>
#include <vector>
#include "offline_fs.hpp"

namespace OfflineDict {

struct DictEntry {
    std::string matchedSurface;
    std::string contextText;
    std::string headword;
    std::string reading;
    std::string pos;
    std::string rankJlpt;
    uint32_t freq{0};
    std::vector<std::string> wrappedLines;
    bool found{false};
};

struct KanjiDetails {
    uint32_t cp{0};
    std::string literal;
    uint16_t freq{0};
    uint16_t radNum{0};
    uint8_t strokes{0};
    uint8_t grade{0};
    uint8_t jlpt{0};
    std::string radChar;
    std::string radBase;
    std::string radReading;
    std::string radMeaning;
    std::string onReadings;
    std::string kunReadings;
    std::string nanori;
    std::string phonetic;
    std::string meanings;
    std::vector<std::string> components;
    std::vector<uint8_t> strokeBits120; // 1800 bytes (120x120 1-bit MSB-first) if present
    bool found{false};
};

class DictionaryEngine {
public:
    bool Open(const char *jmdictPath, const char *kanjidicPath, std::string &err);
    void Close();
    bool IsOpen() const { return jmdictFile_.IsOpen(); }

    // Exact surface lookup (returns top matches sorted by frequency/priority)
    bool LookupSurface(const std::string &surface, std::vector<DictEntry> &out, int maxResults = 6);

    // Word mode: longest-match + deinflection starting at charIdx (with +/-1 fallback)
    bool LookupWordAt(const std::string &lineUtf8, int charIdx,
                      std::vector<DictEntry> &outEntries);

    // Region mode: Entry 0 = "Selection Translation" (OCR text + word-by-word gloss),
    // followed by full dictionary entries for each unique content word.
    bool GlossRegion(const std::vector<std::string> &linesUtf8,
                     std::vector<DictEntry> &outEntries);

    // Kanjidic2 lookup for a single Unicode codepoint
    bool LookupKanji(uint32_t cp, DictEntry &outEntry);
    bool LookupKanjiDetails(uint32_t cp, KanjiDetails &outDetails);

private:
    struct SparseKey {
        uint32_t keyOff;
        std::string key;
    };

    struct BlockCacheSlot {
        uint32_t blockId{0xFFFFFFFFu};
        uint32_t stamp{0};
        std::vector<uint8_t> data;
    };

    OfflineFile jmdictFile_;
    OfflineFile kanjiFile_;
    uint32_t numBlocks_{0};
    uint64_t sparseFileOff_{0};
    uint64_t keyBlobFileOff_{0};
    uint32_t keyBlobBytes_{0};
    uint64_t blockOffTableFileOff_{0};
    std::vector<SparseKey> sparseKeys_;
    std::vector<BlockCacheSlot> cache_;
    std::vector<BlockCacheSlot> keyCache_;
    uint32_t cacheStamp_{0};

    uint32_t kanjiCount_{0};

    bool ReadBlock(uint32_t blockId, const std::vector<uint8_t> *&outData);
    bool DecodeEntryAt(uint32_t blockId, uint16_t offInBlock, DictEntry &out);
};

// Utility: wrap text cleanly to ~36 half-width columns for 320px popup
std::vector<std::string> WrapPopupLines(const std::string &text, int maxCols = 38);

// Utility: convert katakana in UTF-8 to hiragana for deinflection/lookup normalization
std::string KatakanaToHiragana(const std::string &utf8);

} // namespace OfflineDict

#endif // OFFLINE_DICT_HPP
