#include "CTRPluginFrameworkImpl/Graphics/Font.hpp"
#include "CTRPluginFrameworkImpl/Graphics/Renderer.hpp"
#include "CTRPluginFramework/System/Lock.hpp"
#include "3ds.h"
#include "ctrulibExtension.h"

#include <cstring>
#include <cmath>
#include <unordered_map>
#include "CTRPluginFramework/Utils/Utils.hpp"


namespace CTRPluginFramework
{
    extern "C" CFNT_s* g_sharedFont;
    extern "C" int g_charPerSheet;

    u32     g_fontAllocated = 0;
    u32     g_glyphAllocated = 0;
    Mutex  Font::_mutex;

    namespace
    {
        u8 *                         glyph = nullptr;
        Glyph *                      defaultGlyph = nullptr;
        std::unordered_map<u32, u32> defaultSysFont;
    }

    float Glyph::Width(void) const
    {
        return (xOffset + xAdvance);
    }

    // Stub game' call to APT_MapSharedFont as we do it already
    static void     PatchGameFontMapping(void)
    {
        std::vector<u32> pattern =
        {
            0xE3A03201, // mov r3, #0x10000000
            0xE3A02001, // mov r2, #1
            0xE3A01000, // mov r1, #0
            0xE5940004, // ldr r0, [r4, #4]
            0xEF00001F  // svc 0x1F
        };

        u32 addr = Utils::Search<u32>(0x00100000, Process::GetTextSize(), pattern);

        if (addr)
            *reinterpret_cast<vu32 *>(addr + 16) = 0xE3A00000; ///< mov r0, #0
    }

    void    Font::Initialize(void)
    {
        fontEnsureMappedExtension();
        PatchGameFontMapping();
        glyph = (u8 *)new u8[1000];
    }

    Glyph   *Font::GetGlyph(u8* &c)
    {
        u32     code;
        u32     glyphIndex;
        ssize_t units;

        units = decode_utf8(&code, c);
        if (units == -1)
            return (nullptr);

        c += units;
        if (code > 0)
        {
            glyphIndex = fontGlyphIndexFromCodePoint(nullptr, code);
            if (glyphIndex == 0xFFFF) // Glyph not found, return "?" instead
            {
                if (defaultGlyph == nullptr)
                    defaultGlyph = CacheGlyph(fontGlyphIndexFromCodePoint(nullptr, (u32)'?'));

                return defaultGlyph;
            }

            return CacheGlyph(glyphIndex);
        }
        return (nullptr);
    }

    Glyph   *Font::GetGlyph(char c)
    {
        u8 *s = (u8 *)&c;

        return (GetGlyph(s));
    }

    inline u8    GetAlphaValueFromData(u8* data, int dataPos, u16 format) {
        u8 res, byte;
        switch (format)
        {
        case GPU_A4:
            byte = data[dataPos / 2];
            res = ((byte >> ((dataPos & 1) * 4)) & 0x0F) * 0x11;
            break;
        case GPU_A8:
            res = data[dataPos];
            break;
        default: // The rest of the formats are not normally used with fonts
            res = 0;
            break;
        }
        return res;
    }

    // Original code by ObsidianX
    // https://github.com/ObsidianX/3dstools/blob/master/bffnt.py
    u8    *GetOriginalGlyph(u32 glyphIndex)
    {
        TGLP_s  *tglp = fontGetGlyphInfo(nullptr);
        u8      *data = (u8 *)fontGetGlyphSheetTex(nullptr, glyphIndex / g_charPerSheet);

        int     width = tglp->sheetWidth;
        int     height = tglp->sheetHeight;

        int     dataWidth = width;
        int     dataHeight = height;

        int     index = glyphIndex % g_charPerSheet;

        // Increase the size of the image to a power-of-two boundary, if necessary
        width = 1 << (int)(std::ceil(std::log2(width)));
        height = 1 << (int)(std::ceil(std::log2(height)));

        int tileWidth = width / 8;
        int tileHeight = height / 8;

        std::memset(glyph, 0, 1000);

        // Get the part we're interested in
        int glyphsPerRow = tglp->nRows;
        int glyphsPerColumn = tglp->nLines;
        int indexX = index % glyphsPerRow;
        int indexY = index / glyphsPerRow;

        int singleWx = std::round(width / glyphsPerRow);
        int singleHy = std::round(height / glyphsPerColumn);
        int startPx = std::round(indexX * singleWx);
        int endPx = startPx + singleWx;
        int startPy = std::round(indexY * singleHy);
        int endPy = startPy + singleHy;

        // Sheet is composed of 8x8 pixel tiles
        for (int tileY = 0; tileY < tileHeight; tileY++)
        {
            for (int tileX = 0; tileX < tileWidth; tileX++)
            {
                // Tile is composed of 2x2 sub-tiles
                for (int y = 0; y < 2; y++)
                {
                    for (int x = 0; x < 2; x++)
                    {
                        // Subtile is composed of 2x2 pixel groups
                        for (int yy = 0; yy < 2; yy++)
                        {
                            for (int xx = 0; xx < 2; xx++)
                            {
                                // Pixel group is composed of 2x2 pixels
                                for (int yyy = 0; yyy < 2; yyy++)
                                {
                                    for (int xxx = 0; xxx < 2; xxx++)
                                    {
                                        // If the final y value is beyond the input data's height then don't read it
                                        if (tileY + y + yy + yyy >= dataHeight)
                                            continue;
                                        // Same for the x and the input data width
                                        if (tileX + x + xx + xxx >= dataWidth)
                                            continue;

                                        int pixelX = (xxx + (xx * 2) + (x * 4) + ((tileX) * 8));
                                        int pixelY = (yyy + (yy * 2) + (y * 4) + (tileY * 8));

                                        int dataX = (xxx + (xx * 4) + (x * 16) + (tileX * 64));
                                        int dataY = ((yyy * 2) + (yy * 8) + (y * 32) + (tileY * width * 8));

                                        int dataPos = dataX + dataY;

                                        if (pixelY >= startPy && pixelY <= endPy)
                                            if (pixelX >= startPx && pixelX <= endPx)
                                            {
                                                u32 offset = ((pixelX - startPx) + (pixelY - startPy) * singleWx);
                                                if (offset < 1000)
                                                    *(glyph + offset) = GetAlphaValueFromData(data, dataPos, tglp->sheetFmt);
                                            }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        return (glyph);
    }

    u8    bilinearInterpolate(const u8* bitmap, int srcWidth, int srcHeight, float x, float y) {
        int x1 = static_cast<int>(std::floor(x));
        int y1 = static_cast<int>(std::floor(y));
        int x2 = std::min(x1 + 1, srcWidth - 1);
        int y2 = std::min(y1 + 1, srcHeight - 1);

        float dx = x - x1;
        float dy = y - y1;

        u8 p11 = bitmap[y1 * srcWidth + x1];
        u8 p12 = bitmap[y2 * srcWidth + x1];
        u8 p21 = bitmap[y1 * srcWidth + x2];
        u8 p22 = bitmap[y2 * srcWidth + x2];

        return static_cast<u8>((1 - dx) * (1 - dy) * p11 + dx * (1 - dy) * p21 + (1 - dx) * dy * p12 + dx * dy * p22);
    }

    void    ShrinkGlyph(u8* dest, const u8* src, int srcWidth, int srcHeight, int destWidth, int destHeight)
    {
        float scaleX = static_cast<float>(srcWidth - 1) / (destWidth - 1);
        float scaleY = static_cast<float>(srcHeight - 1) / (destHeight - 1);

        for (int y = 0; y < destHeight; ++y) {
            for (int x = 0; x < destWidth; ++x) {
                float srcX = x * scaleX;
                float srcY = y * scaleY;
                dest[y * destWidth + x] = bilinearInterpolate(src, srcWidth, srcHeight, srcX, srcY);
            }
        }
    }

    Glyph   *Font::CacheGlyph(u32 glyphIndex)
    {
        // Check if the glyph already exists
        {
            Glyph *glyph = reinterpret_cast<Glyph *>(defaultSysFont[glyphIndex]);

            if (glyph != nullptr)
                return glyph;
        }

        Lock    lock(_mutex);

        u8  *originalGlyph = GetOriginalGlyph(glyphIndex);
        // 16px * 14px = 224
        u8  *newGlyph = new u8[224];
        g_fontAllocated += 224;
        std::memset(newGlyph, 0, 224);

        u8 originalWidth = g_sharedFont->finf.width;
        u8 originalHeight = g_sharedFont->finf.height;
        // Shrink glyph data to the required size
        ShrinkGlyph(newGlyph, originalGlyph, originalWidth, originalHeight, 14, 16);

        // Allocate new Glyph
        Glyph *glyph = new Glyph;
        g_fontAllocated += sizeof(Glyph);
        g_glyphAllocated++;
        std::memset(glyph, 0, sizeof(Glyph));

        // Get Glyph data
        charWidthInfo_s     *cwi;
        fontGlyphPos_s      glyphPos;
        Renderer::FontCalcGlyphPos(&glyphPos, &cwi, glyphIndex, 14.0f / originalWidth);

        glyph->xOffset =  std::floor((glyphIndex == 0) ? 0 : glyphPos.xOffset);
        glyph->xAdvance = std::round((glyphIndex == 0) ? glyphPos.xAdvance : (glyphPos.xAdvance - glyphPos.xOffset));
        glyph->glyph = newGlyph;

        // Add Glyph to defaultSysFont
        defaultSysFont[glyphIndex] = reinterpret_cast<u32>(glyph);

        return (glyph);
    }
}
