#ifndef OFFLINE_FONT_HPP
#define OFFLINE_FONT_HPP

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "offline_fs.hpp"

namespace OfflineFont {

struct GlyphData {
    uint8_t width{0};
    uint8_t height{0};
    int8_t  offsetX{0};
    int8_t  offsetY{0};
    uint8_t advanceX{0};
    std::vector<uint8_t> packed;
};

class FontPack {
public:
    bool Open(const char *fontPackPath, std::string &err);
    void Close();
    bool IsOpen() const { return file_.IsOpen(); }

    // Load all unique glyphs in `text` for `fontId` (0, 1, or 2) into `outMap`
    void LoadGlyphsForText(uint8_t fontId, const std::string &text,
                           std::unordered_map<uint64_t, GlyphData> &outMap);

private:
    OfflineFile file_;
    uint32_t count_{0};
    std::vector<uint64_t> sparseKeys_;
    std::unordered_map<uint64_t, GlyphData> cache_;

    bool FindGlyph(uint64_t key, GlyphData &out);
};

} // namespace OfflineFont

#endif // OFFLINE_FONT_HPP
