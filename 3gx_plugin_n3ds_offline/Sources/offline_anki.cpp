#include "offline_anki.hpp"
#include "offline_fs.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <zlib.h>

#ifdef __3DS__
#include <3ds.h>
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ssl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace OfflineAnki {

static std::string JsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c >= 0x20) out.push_back(c);
                break;
        }
    }
    return out;
}

static std::string HtmlEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\n': out += "<br>"; break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

static std::string FormatJsonLine(const AnkiCard &c) {
    char tidBuf[24];
    std::snprintf(tidBuf, sizeof(tidBuf), "%016llX", (unsigned long long)c.titleId);
    std::string line = "{\"titleId\":\"";
    line += tidBuf;
    line += "\",\"headword\":\""; line += JsonEscape(c.headword);
    line += "\",\"reading\":\"";  line += JsonEscape(c.reading);
    line += "\",\"pos\":\"";      line += JsonEscape(c.pos);
    line += "\",\"gloss\":\"";    line += JsonEscape(c.gloss);
    line += "\",\"context\":\"";  line += JsonEscape(c.context);
    line += "\"}\n";
    return line;
}

static bool ExtractJsonStringField(const std::string &line, const char *key, std::string &out) {
    out.clear();
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = line.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < line.size()) {
        char c = line[p++];
        if (c == '"') return true;
        if (c == '\\' && p < line.size()) {
            char esc = line[p++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                default:   out.push_back(esc); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool QueueEntry(const char *queuePath, uint64_t titleId,
                const std::string &headword, const std::string &reading,
                const std::string &pos, const std::string &gloss,
                const std::string &context, std::string &err)
{
    OfflineFile f;
    if (!f.OpenAppend(queuePath)) {
        err = "Failed to open Anki queue";
        return false;
    }
    AnkiCard card;
    card.titleId = titleId;
    card.headword = headword;
    card.reading = reading;
    card.pos = pos;
    card.gloss = gloss;
    card.context = context;
    std::string line = FormatJsonLine(card);
    bool ok = f.WriteExact(line.data(), line.size());
    f.Close();
    if (!ok) {
        err = "Failed writing Anki queue";
        return false;
    }
    return true;
}

int CountQueued(const char *queuePath) {
    OfflineFile f;
    if (!f.OpenRead(queuePath)) return 0;
    uint64_t sz = f.GetSize();
    if (sz == 0 || sz > 4 * 1024 * 1024) return 0;
    std::string data((size_t)sz, '\0');
    if (!f.ReadExact(&data[0], (size_t)sz)) return 0;
    int cnt = 0;
    for (char ch : data) {
        if (ch == '\n') ++cnt;
    }
    return cnt;
}

bool LoadQueued(const char *queuePath, std::vector<AnkiCard> &out) {
    out.clear();
    OfflineFile f;
    if (!f.OpenRead(queuePath)) return true; // No queue file = 0 items
    uint64_t sz = f.GetSize();
    if (sz == 0 || sz > 4 * 1024 * 1024) return true;
    std::string data((size_t)sz, '\0');
    if (!f.ReadExact(&data[0], (size_t)sz)) return false;

    size_t start = 0;
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        std::string line = (nl == std::string::npos)
            ? data.substr(start)
            : data.substr(start, nl - start);
        if (line.size() > 2) {
            AnkiCard c;
            std::string tidStr;
            if (ExtractJsonStringField(line, "titleId", tidStr)) {
                c.titleId = (uint64_t)std::strtoull(tidStr.c_str(), nullptr, 16);
            }
            ExtractJsonStringField(line, "headword", c.headword);
            ExtractJsonStringField(line, "reading", c.reading);
            ExtractJsonStringField(line, "pos", c.pos);
            ExtractJsonStringField(line, "gloss", c.gloss);
            ExtractJsonStringField(line, "context", c.context);
            if (!c.headword.empty()) {
                out.push_back(std::move(c));
            }
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return true;
}

bool RewriteQueue(const char *queuePath, const std::vector<AnkiCard> &remaining, std::string &err) {
    if (remaining.empty()) {
        std::remove(queuePath);
        return true;
    }
    std::string all;
    for (const auto &c : remaining) {
        all += FormatJsonLine(c);
    }
    FILE *fp = std::fopen(queuePath, "wb");
    if (!fp) {
        err = "Failed to update Anki queue";
        return false;
    }
    size_t w = std::fwrite(all.data(), 1, all.size(), fp);
    std::fclose(fp);
    if (w != all.size()) {
        err = "Short write updating Anki queue";
        return false;
    }
    return true;
}

std::string ResolveDeckName(uint64_t titleId, const std::string &fallbackName) {
    if (titleId == 0) return fallbackName.empty() ? "3DS OCR" : fallbackName;
    char tidBuf[24];
    std::snprintf(tidBuf, sizeof(tidBuf), "%016llX", (unsigned long long)titleId);
    std::string tidHex(tidBuf);

    // 1. Optional SD override file: /luma/ocr_titles.txt (lines: 00040000001B5100=Pokémon Ultra Moon)
    if (FILE *fp = std::fopen("/luma/ocr_titles.txt", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), fp)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string k = s.substr(0, eq);
            for (char &c : k) c = (char)std::toupper((unsigned char)c);
            if (k == tidHex && eq + 1 < s.size()) {
                std::fclose(fp);
                return s.substr(eq + 1);
            }
        }
        std::fclose(fp);
    }

    // 2. Built-in 3DS titles matching ocr_server.py BUILTIN_3DS_TITLES
    struct TitleEntry { const char *tid; const char *name; };
    static const TitleEntry kBuiltin3dsTitles[] = {
        {"0004000000164800", "Pokémon Sun"},
        {"0004000000175E00", "Pokémon Moon"},
        {"00040000001B5000", "Pokémon Ultra Sun"},
        {"00040000001B5100", "Pokémon Ultra Moon"},
        {"0004000000055D00", "Pokémon X"},
        {"0004000000055E00", "Pokémon Y"},
        {"000400000011C400", "Pokémon Omega Ruby"},
        {"000400000011C500", "Pokémon Alpha Sapphire"},
        {"0004000000033400", "The Legend of Zelda: Ocarina of Time 3D"},
        {"0004000000033500", "The Legend of Zelda: Ocarina of Time 3D"},
        {"0004000000125500", "The Legend of Zelda: Majora's Mask 3D"},
        {"00040000000EC200", "The Legend of Zelda: A Link Between Worlds"},
        {"000400000009F100", "Fire Emblem Awakening"},
        {"0004000000179400", "Fire Emblem Fates"},
        {"00040000001B4000", "Fire Emblem Echoes: Shadows of Valentia"},
        {"00040000000B0F00", "Bravely Default"},
        {"00040000000FC500", "Bravely Default: For the Sequel"},
        {"0004000000086200", "Animal Crossing: New Leaf"},
        {"0004000000086300", "Animal Crossing: New Leaf"},
        {"0004000000084B00", "Dragon Quest VII"},
        {"0004000000136E00", "Dragon Quest VIII"},
        {"0004000000199A00", "Dragon Quest XI"},
        {"00040000000C9200", "Yo-kai Watch"},
        {"000400000008E500", "Shin Megami Tensei IV"},
        {"0004000000098800", "Fantasy Life"},
        {"0004000000126900", "Monster Hunter 4 Ultimate"},
        {"0004000000155400", "Monster Hunter Cross"},
        {"00040000001B7100", "The Great Ace Attorney 2"},
        {"0004000000132700", "The Great Ace Attorney"},
    };
    for (const auto &te : kBuiltin3dsTitles) {
        if (tidHex == te.tid) return te.name;
    }
    return fallbackName.empty() ? "3DS OCR" : fallbackName;
}

std::string Base64Encode(const uint8_t *data, size_t len) {
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = ((uint32_t)data[i]) << 16;
        if (i + 1 < len) v |= ((uint32_t)data[i + 1]) << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];
        out.push_back(kB64[(v >> 18) & 0x3F]);
        out.push_back(kB64[(v >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kB64[(v >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kB64[v & 0x3F] : '=');
    }
    return out;
}

static void AppendBe32(std::vector<uint8_t> &dst, uint32_t v) {
    dst.push_back((uint8_t)(v >> 24));
    dst.push_back((uint8_t)(v >> 16));
    dst.push_back((uint8_t)(v >> 8));
    dst.push_back((uint8_t)v);
}

static void AppendPngChunk(std::vector<uint8_t> &dst, const char *type4, const uint8_t *data, uint32_t len) {
    AppendBe32(dst, len);
    size_t typePos = dst.size();
    dst.insert(dst.end(), (const uint8_t *)type4, (const uint8_t *)type4 + 4);
    if (len > 0 && data) {
        dst.insert(dst.end(), data, data + len);
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, dst.data() + typePos, 4 + len);
    AppendBe32(dst, (uint32_t)crc);
}

bool EncodeDualScreenPng(const uint8_t *topRgb, const uint8_t *botRgb,
                         bool halfScale, std::vector<uint8_t> &outPng)
{
    outPng.clear();
    if (!topRgb || !botRgb) return false;

    const int outW = halfScale ? 200 : 400;
    const int outH = halfScale ? 240 : 480;
    const int rowStride = 1 + outW * 3;
    std::vector<uint8_t> raw((size_t)outH * (size_t)rowStride, 0);

    auto getFullPixel = [&](int x, int y, uint8_t &r, uint8_t &g, uint8_t &b) {
        if (y < 240) {
            size_t idx = ((size_t)y * 400 + (size_t)x) * 3;
            r = topRgb[idx]; g = topRgb[idx + 1]; b = topRgb[idx + 2];
        } else {
            int bx = x - 40;
            if (bx >= 0 && bx < 320) {
                size_t idx = ((size_t)(y - 240) * 320 + (size_t)bx) * 3;
                r = botRgb[idx]; g = botRgb[idx + 1]; b = botRgb[idx + 2];
            } else {
                r = g = b = 0;
            }
        }
    };

    for (int y = 0; y < outH; ++y) {
        uint8_t *row = raw.data() + (size_t)y * (size_t)rowStride;
        row[0] = 0; // PNG filter type 0 (None)
        if (!halfScale) {
            for (int x = 0; x < outW; ++x) {
                uint8_t r, g, b;
                getFullPixel(x, y, r, g, b);
                row[1 + x * 3 + 0] = r;
                row[1 + x * 3 + 1] = g;
                row[1 + x * 3 + 2] = b;
            }
        } else {
            for (int x = 0; x < outW; ++x) {
                uint32_t sr = 0, sg = 0, sb = 0;
                for (int dy = 0; dy < 2; ++dy) {
                    for (int dx = 0; dx < 2; ++dx) {
                        uint8_t r, g, b;
                        getFullPixel(x * 2 + dx, y * 2 + dy, r, g, b);
                        sr += r; sg += g; sb += b;
                    }
                }
                row[1 + x * 3 + 0] = (uint8_t)(sr >> 2);
                row[1 + x * 3 + 1] = (uint8_t)(sg >> 2);
                row[1 + x * 3 + 2] = (uint8_t)(sb >> 2);
            }
        }
    }

    uLongf compCap = compressBound((uLong)raw.size());
    std::vector<uint8_t> zbuf((size_t)compCap);
    if (compress2(zbuf.data(), &compCap, raw.data(), (uLong)raw.size(), Z_BEST_SPEED) != Z_OK) {
        return false;
    }

    static const uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    outPng.reserve(64 + (size_t)compCap);
    outPng.insert(outPng.end(), kPngSig, kPngSig + 8);

    uint8_t ihdr[13] = {
        (uint8_t)(outW >> 24), (uint8_t)(outW >> 16), (uint8_t)(outW >> 8), (uint8_t)outW,
        (uint8_t)(outH >> 24), (uint8_t)(outH >> 16), (uint8_t)(outH >> 8), (uint8_t)outH,
        8, // bit depth
        2, // color type 2 = RGB
        0, 0, 0
    };
    AppendPngChunk(outPng, "IHDR", ihdr, 13);
    AppendPngChunk(outPng, "IDAT", zbuf.data(), (uint32_t)compCap);
    AppendPngChunk(outPng, "IEND", nullptr, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Compact Baseline JPEG Encoder (YCbCr 4:4:4, Annex-K Quant & Huffman)
// ---------------------------------------------------------------------------

namespace {

static const uint8_t kJpegZigZag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

static const uint8_t kStdLumQ[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68,109,103, 77, 24, 35, 55, 64, 81,104,113, 92,
    49, 64, 78, 87,103,121,120,101, 72, 92, 95, 98,112,100,103, 99
};

static const uint8_t kStdChromQ[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};

static const uint8_t kDcLumBits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t kDcLumVals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
static const uint8_t kDcChromBits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const uint8_t kDcChromVals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};

static const uint8_t kAcLumBits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,125};
static const uint8_t kAcLumVals[162] = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,
    0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,
    0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,
    0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,
    0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,
    0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,
    0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
    0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

static const uint8_t kAcChromBits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,119};
static const uint8_t kAcChromVals[162] = {
    0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,
    0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,
    0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
    0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,
    0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,
    0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
    0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,
    0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,
    0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
    0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,
    0xf9,0xfa
};

struct JpegHuff {
    uint16_t code[256];
    uint8_t size[256];
};

static void BuildHuffTable(const uint8_t *bits, const uint8_t *vals, JpegHuff &ht) {
    std::memset(&ht, 0, sizeof(ht));
    uint16_t code = 0;
    int vi = 0;
    for (int len = 1; len <= 16; ++len) {
        int cnt = bits[len - 1];
        for (int i = 0; i < cnt; ++i) {
            uint8_t sym = vals[vi++];
            ht.code[sym] = code;
            ht.size[sym] = (uint8_t)len;
            ++code;
        }
        code <<= 1;
    }
}

struct JpegBitWriter {
    std::vector<uint8_t> &out;
    uint32_t bitBuf{0};
    int bitCnt{0};
    explicit JpegBitWriter(std::vector<uint8_t> &o) : out(o) {}
    void PutBits(uint16_t bits, int n) {
        bitBuf = (bitBuf << n) | (bits & ((1u << n) - 1u));
        bitCnt += n;
        while (bitCnt >= 8) {
            bitCnt -= 8;
            uint8_t b = (uint8_t)((bitBuf >> bitCnt) & 0xFFu);
            out.push_back(b);
            if (b == 0xFF) out.push_back(0x00);
        }
    }
    void Flush() {
        if (bitCnt > 0) PutBits((uint16_t)((1u << (8 - bitCnt)) - 1u), 8 - bitCnt);
    }
};

static void JpegFdct8x8(float *d) {
    // AAN / separable 1D-8 DCT
    static const float c1 = 0.98078528f, c2 = 0.92387953f, c3 = 0.83146961f;
    static const float c4 = 0.70710678f, c5 = 0.55557023f, c6 = 0.38268343f, c7 = 0.19509032f;
    auto dct1d = [&](int stride, int step) {
        for (int i = 0; i < 8; ++i) {
            float *p = d + i * step;
            float x0 = p[0*stride], x1 = p[1*stride], x2 = p[2*stride], x3 = p[3*stride];
            float x4 = p[4*stride], x5 = p[5*stride], x6 = p[6*stride], x7 = p[7*stride];
            float s0 = x0 + x7, d0 = x0 - x7;
            float s1 = x1 + x6, d1 = x1 - x6;
            float s2 = x2 + x5, d2 = x2 - x5;
            float s3 = x3 + x4, d3 = x3 - x4;
            float t0 = s0 + s3, t3 = s0 - s3;
            float t1 = s1 + s2, t2 = s1 - s2;
            p[0*stride] = 0.35355339f * (t0 + t1);
            p[4*stride] = 0.35355339f * (t0 - t1);
            p[2*stride] = 0.5f * (c2 * t3 + c6 * t2);
            p[6*stride] = 0.5f * (c6 * t3 - c2 * t2);
            p[1*stride] = 0.5f * (c1 * d0 + c3 * d1 + c5 * d2 + c7 * d3);
            p[3*stride] = 0.5f * (c3 * d0 - c7 * d1 - c1 * d2 - c5 * d3);
            p[5*stride] = 0.5f * (c5 * d0 - c1 * d1 + c7 * d2 + c3 * d3);
            p[7*stride] = 0.5f * (c7 * d0 - c5 * d1 + c3 * d2 - c1 * d3);
        }
    };
    dct1d(1, 8);
    dct1d(8, 1);
}

static int EncodeBlock(JpegBitWriter &bw, float *blk, const float *invQ, int prevDc,
                       const JpegHuff &dcH, const JpegHuff &acH)
{
    JpegFdct8x8(blk);
    int q[64];
    for (int i = 0; i < 64; ++i) {
        float v = blk[i] * invQ[i];
        q[kJpegZigZag[i]] = (v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f);
    }
    auto bitLen = [](int v) -> int {
        int a = (v < 0) ? -v : v;
        int n = 0;
        while (a > 0) { a >>= 1; ++n; }
        return n;
    };
    auto encodeVal = [&](int v, int n) {
        uint16_t bits = (v >= 0) ? (uint16_t)v : (uint16_t)((1 << n) - 1 + v);
        bw.PutBits(bits, n);
    };

    int dcDiff = q[0] - prevDc;
    int s = bitLen(dcDiff);
    bw.PutBits(dcH.code[s], dcH.size[s]);
    if (s > 0) encodeVal(dcDiff, s);

    int lastNz = 0;
    for (int i = 63; i >= 1; --i) {
        if (q[i] != 0) { lastNz = i; break; }
    }
    for (int i = 1; i <= lastNz; ++i) {
        int zeroRun = 0;
        while (q[i] == 0 && zeroRun < 16) { ++zeroRun; ++i; }
        if (zeroRun == 16) {
            bw.PutBits(acH.code[0xF0], acH.size[0xF0]);
            --i;
            continue;
        }
        int sz = bitLen(q[i]);
        uint8_t sym = (uint8_t)((zeroRun << 4) | sz);
        bw.PutBits(acH.code[sym], acH.size[sym]);
        encodeVal(q[i], sz);
    }
    if (lastNz < 63) bw.PutBits(acH.code[0x00], acH.size[0x00]);
    return q[0];
}

} // namespace

bool EncodeDualScreenJpeg(const uint8_t *topRgb, const uint8_t *botRgb,
                          bool halfScale, std::vector<uint8_t> &outJpg, int quality)
{
    outJpg.clear();
    if (!topRgb || !botRgb) return false;
    if (quality < 10) quality = 10;
    if (quality > 95) quality = 95;
    int scale = (quality < 50) ? (5000 / quality) : (200 - quality * 2);

    uint8_t qLum[64], qChrom[64];
    float invQLum[64], invQChrom[64];
    for (int i = 0; i < 64; ++i) {
        int ql = (kStdLumQ[i] * scale + 50) / 100;
        int qc = (kStdChromQ[i] * scale + 50) / 100;
        if (ql < 1) ql = 1; else if (ql > 255) ql = 255;
        if (qc < 1) qc = 1; else if (qc > 255) qc = 255;
        qLum[kJpegZigZag[i]] = (uint8_t)ql;
        qChrom[kJpegZigZag[i]] = (uint8_t)qc;
        invQLum[i] = 1.0f / (float)ql;
        invQChrom[i] = 1.0f / (float)qc;
    }

    static JpegHuff dcLum, dcChrom, acLum, acChrom;
    static bool huffInit = false;
    if (!huffInit) {
        BuildHuffTable(kDcLumBits, kDcLumVals, dcLum);
        BuildHuffTable(kDcChromBits, kDcChromVals, dcChrom);
        BuildHuffTable(kAcLumBits, kAcLumVals, acLum);
        BuildHuffTable(kAcChromBits, kAcChromVals, acChrom);
        huffInit = true;
    }

    const int outW = halfScale ? 200 : 400;
    const int outH = halfScale ? 240 : 480;
    outJpg.reserve(halfScale ? 16384 : 40960);

    auto w16 = [&](uint16_t v) { outJpg.push_back((uint8_t)(v >> 8)); outJpg.push_back((uint8_t)v); };
    // SOI + APP0
    outJpg.push_back(0xFF); outJpg.push_back(0xD8);
    outJpg.push_back(0xFF); outJpg.push_back(0xE0); w16(16);
    outJpg.insert(outJpg.end(), {'J','F','I','F',0, 1,1, 0, 0,1, 0,1, 0,0});
    // DQT
    auto writeDqt = [&](uint8_t id, const uint8_t *qt) {
        outJpg.push_back(0xFF); outJpg.push_back(0xDB); w16(67);
        outJpg.push_back(id);
        outJpg.insert(outJpg.end(), qt, qt + 64);
    };
    writeDqt(0, qLum);
    writeDqt(1, qChrom);
    // SOF0 (4:2:0)
    outJpg.push_back(0xFF); outJpg.push_back(0xC0); w16(17);
    outJpg.push_back(8); w16((uint16_t)outH); w16((uint16_t)outW); outJpg.push_back(3);
    outJpg.push_back(1); outJpg.push_back(0x22); outJpg.push_back(0);
    outJpg.push_back(2); outJpg.push_back(0x11); outJpg.push_back(1);
    outJpg.push_back(3); outJpg.push_back(0x11); outJpg.push_back(1);
    // DHT
    auto writeDht = [&](uint8_t clsId, const uint8_t *bits, const uint8_t *vals, int nVals) {
        outJpg.push_back(0xFF); outJpg.push_back(0xC4); w16((uint16_t)(19 + nVals));
        outJpg.push_back(clsId);
        outJpg.insert(outJpg.end(), bits, bits + 16);
        outJpg.insert(outJpg.end(), vals, vals + nVals);
    };
    writeDht(0x00, kDcLumBits, kDcLumVals, 12);
    writeDht(0x10, kAcLumBits, kAcLumVals, 162);
    writeDht(0x01, kDcChromBits, kDcChromVals, 12);
    writeDht(0x11, kAcChromBits, kAcChromVals, 162);
    // SOS
    outJpg.push_back(0xFF); outJpg.push_back(0xDA); w16(12);
    outJpg.push_back(3);
    outJpg.push_back(1); outJpg.push_back(0x00);
    outJpg.push_back(2); outJpg.push_back(0x11);
    outJpg.push_back(3); outJpg.push_back(0x11);
    outJpg.push_back(0); outJpg.push_back(63); outJpg.push_back(0);

    auto getFullPixel = [&](int x, int y, uint8_t &r, uint8_t &g, uint8_t &b) {
        if (y < 240) {
            size_t idx = ((size_t)y * 400 + (size_t)x) * 3;
            r = topRgb[idx]; g = topRgb[idx + 1]; b = topRgb[idx + 2];
        } else {
            int bx = x - 40;
            if (bx >= 0 && bx < 320) {
                size_t idx = ((size_t)(y - 240) * 320 + (size_t)bx) * 3;
                r = botRgb[idx]; g = botRgb[idx + 1]; b = botRgb[idx + 2];
            } else {
                r = g = b = 0;
            }
        }
    };

    JpegBitWriter bw(outJpg);
    int prevY = 0, prevCb = 0, prevCr = 0;
    float blkY[64], blkCb[64], blkCr[64];

    auto samplePixel = [&](int sx, int sy, uint8_t &r, uint8_t &g, uint8_t &b) {
        if (!halfScale) {
            getFullPixel(sx, sy, r, g, b);
        } else {
            uint8_t r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
            getFullPixel(sx * 2, sy * 2, r0, g0, b0);
            getFullPixel(sx * 2 + 1, sy * 2, r1, g1, b1);
            getFullPixel(sx * 2, sy * 2 + 1, r2, g2, b2);
            getFullPixel(sx * 2 + 1, sy * 2 + 1, r3, g3, b3);
            r = (uint8_t)(((uint32_t)r0 + r1 + r2 + r3) >> 2);
            g = (uint8_t)(((uint32_t)g0 + g1 + g2 + g3) >> 2);
            b = (uint8_t)(((uint32_t)b0 + b1 + b2 + b3) >> 2);
        }
    };

    for (int by = 0; by < outH; by += 16) {
        for (int bx = 0; bx < outW; bx += 16) {
            // 4 Y blocks in 16x16 MCU: (0,0), (8,0), (0,8), (8,8)
            for (int qy = 0; qy < 16; qy += 8) {
                for (int qx = 0; qx < 16; qx += 8) {
                    for (int dy = 0; dy < 8; ++dy) {
                        int sy = std::min(outH - 1, by + qy + dy);
                        for (int dx = 0; dx < 8; ++dx) {
                            int sx = std::min(outW - 1, bx + qx + dx);
                            uint8_t r, g, b;
                            samplePixel(sx, sy, r, g, b);
                            blkY[dy * 8 + dx] = 0.299f * (float)r + 0.587f * (float)g + 0.114f * (float)b - 128.0f;
                        }
                    }
                    prevY = EncodeBlock(bw, blkY, invQLum, prevY, dcLum, acLum);
                }
            }
            // 1 subsampled Cb + 1 subsampled Cr block (2x2 average per chroma sample)
            for (int dy = 0; dy < 8; ++dy) {
                int sy0 = std::min(outH - 1, by + dy * 2);
                int sy1 = std::min(outH - 1, by + dy * 2 + 1);
                for (int dx = 0; dx < 8; ++dx) {
                    int sx0 = std::min(outW - 1, bx + dx * 2);
                    int sx1 = std::min(outW - 1, bx + dx * 2 + 1);
                    uint8_t r0, g0, b0, r1, g1, b1, r2, g2, b2, r3, g3, b3;
                    samplePixel(sx0, sy0, r0, g0, b0);
                    samplePixel(sx1, sy0, r1, g1, b1);
                    samplePixel(sx0, sy1, r2, g2, b2);
                    samplePixel(sx1, sy1, r3, g3, b3);
                    float rf = 0.25f * (float)((uint32_t)r0 + r1 + r2 + r3);
                    float gf = 0.25f * (float)((uint32_t)g0 + g1 + g2 + g3);
                    float bf = 0.25f * (float)((uint32_t)b0 + b1 + b2 + b3);
                    int idx = dy * 8 + dx;
                    blkCb[idx] = -0.168736f * rf - 0.331264f * gf + 0.5f * bf;
                    blkCr[idx] =  0.5f * rf - 0.418688f * gf - 0.081312f * bf;
                }
            }
            prevCb = EncodeBlock(bw, blkCb, invQChrom, prevCb, dcChrom, acChrom);
            prevCr = EncodeBlock(bw, blkCr, invQChrom, prevCr, dcChrom, acChrom);
        }
    }
    bw.Flush();
    outJpg.push_back(0xFF); outJpg.push_back(0xD9);
    return true;
}

void BuildCardHtml(const AnkiCard &card, std::string &frontHtml, std::string &backHtml, std::string &tags) {
    frontHtml = "<div style=\"font-size:28px;font-weight:bold;\">" + HtmlEscape(card.headword) + "</div>";
    if (!card.reading.empty() && card.reading != card.headword) {
        frontHtml += "<div style=\"font-size:18px;color:#888;\">【" + HtmlEscape(card.reading) + "】</div>";
    }
    if (!card.context.empty() && card.context != card.headword) {
        frontHtml += "<div style=\"margin-top:8px;font-size:16px;color:#bbb;\">" + HtmlEscape(card.context) + "</div>";
    }

    backHtml.clear();
    if (!card.pos.empty()) {
        backHtml += "<div style=\"font-size:13px;color:#7dd2af;margin-bottom:6px;\">" + HtmlEscape(card.pos) + "</div>";
    }
    backHtml += "<div style=\"font-size:16px;\">" + HtmlEscape(card.gloss) + "</div>";

    char tidBuf[24];
    std::snprintf(tidBuf, sizeof(tidBuf), "%016llX", (unsigned long long)card.titleId);
    tags = std::string("3ds_ocr offline_ocr tid_") + tidBuf;
}

// ---------------------------------------------------------------------------
// Minimal Protobuf Codec (wire types: 0=varint, 2=length-delimited)
// ---------------------------------------------------------------------------

static void PbWriteVarint(std::string &out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back((char)((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    out.push_back((char)(v & 0x7Fu));
}

static void PbWriteTag(std::string &out, uint32_t fieldNo, uint8_t wireType) {
    PbWriteVarint(out, ((uint64_t)fieldNo << 3) | (uint64_t)wireType);
}

static void PbWriteBytes(std::string &out, uint32_t fieldNo, const std::string &s) {
    if (s.empty()) return;
    PbWriteTag(out, fieldNo, 2);
    PbWriteVarint(out, s.size());
    out.append(s);
}

static void PbWriteInt64(std::string &out, uint32_t fieldNo, int64_t v) {
    PbWriteTag(out, fieldNo, 0);
    PbWriteVarint(out, (uint64_t)v);
}

static bool PbReadVarint(const uint8_t *p, size_t len, size_t &pos, uint64_t &val) {
    val = 0;
    int shift = 0;
    while (pos < len) {
        uint8_t b = p[pos++];
        val |= (uint64_t)(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

static bool PbSkipField(const uint8_t *p, size_t len, size_t &pos, uint8_t wire) {
    uint64_t tmp = 0;
    switch (wire) {
        case 0: return PbReadVarint(p, len, pos, tmp);
        case 1: if (pos + 8 > len) return false; pos += 8; return true;
        case 2:
            if (!PbReadVarint(p, len, pos, tmp) || pos + tmp > len) return false;
            pos += (size_t)tmp;
            return true;
        case 5: if (pos + 4 > len) return false; pos += 4; return true;
        default: return false;
    }
}

std::string PbEncodeLoginReq(const std::string &user, const std::string &pass) {
    std::string out;
    PbWriteBytes(out, 1, user);
    PbWriteBytes(out, 2, pass);
    return out;
}

bool PbDecodeLoginResp(const std::string &buf, int &status, std::string &token) {
    status = 0;
    token.clear();
    const uint8_t *p = (const uint8_t *)buf.data();
    size_t len = buf.size(), pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!PbReadVarint(p, len, pos, tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint8_t wt = (uint8_t)(tag & 7);
        if (fn == 1 && wt == 0) {
            uint64_t v = 0;
            if (!PbReadVarint(p, len, pos, v)) return false;
            status = (int)v;
        } else if (fn == 2 && wt == 2) {
            uint64_t sl = 0;
            if (!PbReadVarint(p, len, pos, sl) || pos + sl > len) return false;
            token.assign((const char *)(p + pos), (size_t)sl);
            pos += (size_t)sl;
        } else {
            if (!PbSkipField(p, len, pos, wt)) return false;
        }
    }
    return true;
}

static bool PbDecodeIdName(const uint8_t *p, size_t len, IdName &out) {
    out = IdName();
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!PbReadVarint(p, len, pos, tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint8_t wt = (uint8_t)(tag & 7);
        if (fn == 1 && wt == 0) {
            uint64_t v = 0;
            if (!PbReadVarint(p, len, pos, v)) return false;
            out.id = (int64_t)v;
        } else if (fn == 2 && wt == 2) {
            uint64_t sl = 0;
            if (!PbReadVarint(p, len, pos, sl) || pos + sl > len) return false;
            out.name.assign((const char *)(p + pos), (size_t)sl);
            pos += (size_t)sl;
        } else {
            if (!PbSkipField(p, len, pos, wt)) return false;
        }
    }
    return true;
}

bool PbDecodeAddInfoResp(const std::string &buf,
                         std::vector<IdName> &notetypes,
                         std::vector<IdName> &decks,
                         int64_t &curDeckId,
                         int64_t &curNtId)
{
    notetypes.clear();
    decks.clear();
    curDeckId = 0;
    curNtId = 0;
    const uint8_t *p = (const uint8_t *)buf.data();
    size_t len = buf.size(), pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!PbReadVarint(p, len, pos, tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint8_t wt = (uint8_t)(tag & 7);
        if ((fn == 1 || fn == 2) && wt == 2) {
            uint64_t sl = 0;
            if (!PbReadVarint(p, len, pos, sl) || pos + sl > len) return false;
            IdName item;
            if (!PbDecodeIdName(p + pos, (size_t)sl, item)) return false;
            if (fn == 1) notetypes.push_back(std::move(item));
            else decks.push_back(std::move(item));
            pos += (size_t)sl;
        } else if ((fn == 3 || fn == 4) && wt == 0) {
            uint64_t v = 0;
            if (!PbReadVarint(p, len, pos, v)) return false;
            if (fn == 3) curDeckId = (int64_t)v;
            else curNtId = (int64_t)v;
        } else {
            if (!PbSkipField(p, len, pos, wt)) return false;
        }
    }
    return true;
}

std::string PbEncodeCreateDeckReq(const std::string &name) {
    std::string out;
    PbWriteBytes(out, 1, name);
    return out;
}

std::string PbEncodeAddNoteReq(const std::string &front,
                               const std::string &back,
                               const std::string &tags,
                               int64_t notetypeId,
                               int64_t deckId)
{
    std::string out;
    PbWriteBytes(out, 1, front);
    PbWriteBytes(out, 1, back);
    PbWriteBytes(out, 2, tags);
    std::string addMode;
    PbWriteInt64(addMode, 1, notetypeId);
    PbWriteInt64(addMode, 2, deckId);
    PbWriteBytes(out, 3, addMode);
    return out;
}

std::string PbEncodeSearchReq(const std::string &query) {
    std::string out;
    PbWriteBytes(out, 1, query);
    return out;
}

static bool PbNoteHasText(const uint8_t *p, size_t len, const std::string &needle) {
    size_t pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!PbReadVarint(p, len, pos, tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint8_t wt = (uint8_t)(tag & 7);
        if (fn == 2 && wt == 2) {
            uint64_t sl = 0;
            if (!PbReadVarint(p, len, pos, sl) || pos + sl > len) return false;
            std::string jf((const char *)(p + pos), (size_t)sl);
            if (jf.find(needle) != std::string::npos) return true;
            pos += (size_t)sl;
        } else {
            if (!PbSkipField(p, len, pos, wt)) return false;
        }
    }
    return false;
}

bool PbDecodeSearchRespHasText(const std::string &buf, const std::string &needle) {
    if (needle.empty()) return false;
    const uint8_t *p = (const uint8_t *)buf.data();
    size_t len = buf.size(), pos = 0;
    while (pos < len) {
        uint64_t tag = 0;
        if (!PbReadVarint(p, len, pos, tag)) return false;
        uint32_t fn = (uint32_t)(tag >> 3);
        uint8_t wt = (uint8_t)(tag & 7);
        if (fn == 1 && wt == 2) {
            uint64_t sl = 0;
            if (!PbReadVarint(p, len, pos, sl) || pos + sl > len) return false;
            if (PbNoteHasText(p + pos, (size_t)sl, needle)) return true;
            pos += (size_t)sl;
        } else {
            if (!PbSkipField(p, len, pos, wt)) return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// AnkiConnect JSON Builders
// ---------------------------------------------------------------------------

std::string BuildAnkiConnectCreateDeckJson(const std::string &deckName) {
    return "{\"action\":\"createDeck\",\"version\":6,\"params\":{\"deck\":\"" +
           JsonEscape(deckName) + "\"}}";
}

std::string BuildAnkiConnectStoreMediaJson(const std::string &filename, const std::string &b64Data) {
    return "{\"action\":\"storeMediaFile\",\"version\":6,\"params\":{\"filename\":\"" +
           JsonEscape(filename) + "\",\"data\":\"" + b64Data + "\"}}";
}

static std::string EscapeAnkiQueryTerm(const std::string &s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static int HexDigitVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool ParseHex4(const char *p, uint32_t &out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        int d = HexDigitVal(p[i]);
        if (d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    out = v;
    return true;
}

static void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp <= 0x7Fu) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FFu) {
        out.push_back((char)(0xC0u | (cp >> 6)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back((char)(0xE0u | (cp >> 12)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back((char)(0xF0u | (cp >> 18)));
        out.push_back((char)(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back((char)(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back((char)(0x80u | (cp & 0x3Fu)));
    }
}

std::string BuildAnkiConnectFindNotesJson(const std::string &deckName, const std::string &headword) {
    std::string q = "deck:\"" + EscapeAnkiQueryTerm(deckName) + "\" \"" + EscapeAnkiQueryTerm(headword) + "\"";
    return "{\"action\":\"findNotes\",\"version\":6,\"params\":{\"query\":\"" + JsonEscape(q) + "\"}}";
}

std::string BuildAnkiConnectNotesInfoJson(const std::vector<int64_t> &noteIds) {
    std::string out = "{\"action\":\"notesInfo\",\"version\":6,\"params\":{\"notes\":[";
    for (size_t i = 0; i < noteIds.size(); ++i) {
        if (i > 0) out.push_back(',');
        out += std::to_string((long long)noteIds[i]);
    }
    out += "]}}";
    return out;
}

bool AnkiConnectNotesInfoHasFrontPrefix(const std::string &notesInfoJson, const std::string &hwPrefix) {
    if (hwPrefix.empty()) return false;
    std::string decoded;
    size_t i = 0;
    const size_t n = notesInfoJson.size();
    while (i < n) {
        if (notesInfoJson[i] != '"') {
            ++i;
            continue;
        }
        ++i; // skip opening quote
        decoded.clear();
        while (i < n) {
            char c = notesInfoJson[i++];
            if (c == '"') break;
            if (c == '\\' && i < n) {
                char esc = notesInfoJson[i++];
                switch (esc) {
                    case '"':  decoded.push_back('"'); break;
                    case '\\': decoded.push_back('\\'); break;
                    case '/':  decoded.push_back('/'); break;
                    case 'b':  decoded.push_back('\b'); break;
                    case 'f':  decoded.push_back('\f'); break;
                    case 'n':  decoded.push_back('\n'); break;
                    case 'r':  decoded.push_back('\r'); break;
                    case 't':  decoded.push_back('\t'); break;
                    case 'u': {
                        uint32_t cp = 0;
                        if (i + 4 <= n && ParseHex4(notesInfoJson.data() + i, cp)) {
                            i += 4;
                            if (cp >= 0xD800u && cp <= 0xDBFFu &&
                                i + 6 <= n && notesInfoJson[i] == '\\' && notesInfoJson[i + 1] == 'u') {
                                uint32_t low = 0;
                                if (ParseHex4(notesInfoJson.data() + i + 2, low) &&
                                    low >= 0xDC00u && low <= 0xDFFFu) {
                                    cp = 0x10000u + (((cp - 0xD800u) << 10) | (low - 0xDC00u));
                                    i += 6;
                                }
                            }
                            AppendUtf8(decoded, cp);
                        } else {
                            decoded.push_back('\\');
                            decoded.push_back('u');
                        }
                        break;
                    }
                    default:
                        decoded.push_back(esc);
                        break;
                }
            } else {
                decoded.push_back(c);
            }
        }
        if (decoded.find(hwPrefix) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string BuildAnkiConnectAddNoteJson(const std::string &deckName,
                                        const std::string &frontHtml,
                                        const std::string &backHtml,
                                        const std::string &tags)
{
    std::string out = "{\"action\":\"addNote\",\"version\":6,\"params\":{\"note\":{";
    out += "\"deckName\":\"" + JsonEscape(deckName) + "\",";
    out += "\"modelName\":\"Basic\",";
    out += "\"fields\":{\"Front\":\"" + JsonEscape(frontHtml) + "\",\"Back\":\"" + JsonEscape(backHtml) + "\"},";
    out += "\"options\":{\"allowDuplicate\":false,\"duplicateScope\":\"deck\"},";
    out += "\"tags\":[";
    bool firstTag = true;
    size_t pos = 0;
    while (pos < tags.size()) {
        while (pos < tags.size() && tags[pos] == ' ') ++pos;
        if (pos >= tags.size()) break;
        size_t sp = tags.find(' ', pos);
        std::string t = (sp == std::string::npos) ? tags.substr(pos) : tags.substr(pos, sp - pos);
        if (!firstTag) out.push_back(',');
        out += "\"" + JsonEscape(t) + "\"";
        firstTag = false;
        if (sp == std::string::npos) break;
        pos = sp + 1;
    }
    out += "]}}}";
    return out;
}

std::string BuildAnkiConnectSyncJson(void) {
    return "{\"action\":\"sync\",\"version\":6}";
}

// ---------------------------------------------------------------------------
// 3DS Network Transport (mbedTLS over soc:u for AnkiWeb + raw TCP for AnkiConnect)
// ---------------------------------------------------------------------------

#ifdef __3DS__

static std::string s_ankiWebCookie;
static std::string s_ankiWebMainCookie;
static int64_t s_cachedNtId = 0;
static std::string s_cachedDeckName;
static int64_t s_cachedDeckId = 0;
static std::string s_acCachedDeck;
static uint32_t s_ankiWebIp = 0;
static uint32_t s_ankiUserIp = 0;

static LightLock s_ankiLock;
static bool s_ankiLockInit = false;

struct AnkiLockGuard {
    AnkiLockGuard() {
        if (!s_ankiLockInit) {
            LightLock_Init(&s_ankiLock);
            s_ankiLockInit = true;
        }
        LightLock_Lock(&s_ankiLock);
    }
    ~AnkiLockGuard() {
        LightLock_Unlock(&s_ankiLock);
    }
};

struct HttpsSession {
    int sock{-1};
    bool active{false};
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctrDrbg;
    mbedtls_entropy_context entropy;

    void Close() {
        if (active) {
            mbedtls_ssl_free(&ssl);
            mbedtls_ssl_config_free(&conf);
            mbedtls_ctr_drbg_free(&ctrDrbg);
            mbedtls_entropy_free(&entropy);
            active = false;
        }
        if (sock >= 0) {
            closesocket(sock);
            sock = -1;
        }
    }
};

static HttpsSession s_ankiUserSession;
static mbedtls_ssl_session s_savedUserTlsSess;
static bool s_hasSavedUserTlsSess = false;
static HttpsSession s_ankiWebSession;
static mbedtls_ssl_session s_savedMainTlsSess;
static bool s_hasSavedMainTlsSess = false;

static void ResetCachedSessionLocked(void) {
    s_ankiUserSession.Close();
    if (s_hasSavedUserTlsSess) {
        mbedtls_ssl_session_free(&s_savedUserTlsSess);
        s_hasSavedUserTlsSess = false;
    }
    s_ankiWebSession.Close();
    if (s_hasSavedMainTlsSess) {
        mbedtls_ssl_session_free(&s_savedMainTlsSess);
        s_hasSavedMainTlsSess = false;
    }
    s_ankiWebCookie.clear();
    s_ankiWebMainCookie.clear();
    s_cachedNtId = 0;
    s_cachedDeckName.clear();
    s_cachedDeckId = 0;
    s_acCachedDeck.clear();
}

void CloseIdleConnection(void) {
    AnkiLockGuard guard;
    s_ankiUserSession.Close();
    s_ankiWebSession.Close();
    if (s_hasSavedUserTlsSess) {
        mbedtls_ssl_session_free(&s_savedUserTlsSess);
        s_hasSavedUserTlsSess = false;
    }
    if (s_hasSavedMainTlsSess) {
        mbedtls_ssl_session_free(&s_savedMainTlsSess);
        s_hasSavedMainTlsSess = false;
    }
}

void ResetCachedSession(void) {
    AnkiLockGuard guard;
    ResetCachedSessionLocked();
}

static std::string UrlEncode(const std::string &s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

static int PollSliced(int sock, short events, int timeoutMs, short &reventsOut) {
    reventsOut = 0;
    int remaining = timeoutMs;
    while (remaining > 0) {
        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = events;
        pfd.revents = 0;
        int slice = (remaining > 50) ? 50 : remaining;
        int r = poll(&pfd, 1, slice);
        if (r != 0) {
            reventsOut = pfd.revents;
            return r;
        }
        remaining -= slice;
    }
    return 0;
}

static int TcpConnectTimeout(const std::string &host, uint16_t port, int timeoutMs, std::string &err) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        uint32_t cachedIp = (host == "ankiweb.net") ? s_ankiWebIp : (host == "ankiuser.net" ? s_ankiUserIp : 0);
        if (cachedIp != 0) {
            addr.sin_addr.s_addr = cachedIp;
        } else {
            struct hostent *he = gethostbyname(host.c_str());
            if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
                err = "DNS failed: " + host;
                return -1;
            }
            std::memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);
            if (host == "ankiweb.net") s_ankiWebIp = addr.sin_addr.s_addr;
            else if (host == "ankiuser.net") s_ankiUserIp = addr.sin_addr.s_addr;
        }
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        err = "socket() failed";
        return -1;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0) {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EAGAIN) {
            closesocket(sock);
            err = "Connect failed: " + host;
            return -1;
        }
        short revents = 0;
        int sel = PollSliced(sock, POLLOUT, timeoutMs, revents);
        if (sel == 0) {
            closesocket(sock);
            err = "Connect timeout: " + host;
            return -1;
        }
        if (sel < 0) {
            closesocket(sock);
            err = "Connect poll err: " + host;
            return -1;
        }
        if (revents & (POLLERR | POLLHUP)) {
            int soErr = 0;
            socklen_t slen = sizeof(soErr);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &soErr, &slen);
            closesocket(sock);
            err = "Connect refused (" + std::to_string(soErr) + "): " + host;
            return -1;
        }
        if (revents & POLLOUT) {
            int soErr = 0;
            socklen_t slen = sizeof(soErr);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soErr, &slen) == 0) {
                // On Nintendo 3DS soc:u, SO_ERROR retains raw -26 (SOC EINPROGRESS) upon connect completion
                if (soErr != 0 && soErr != -26 && soErr != EINPROGRESS) {
                    closesocket(sock);
                    err = "Connect refused (" + std::to_string(soErr) + "): " + host;
                    return -1;
                }
            }
        } else {
            closesocket(sock);
            err = "Connect timeout: " + host;
            return -1;
        }
    }
    if (flags >= 0) fcntl(sock, F_SETFL, flags);
    return sock;
}

static bool DecodeChunkedBody(const std::string &raw, std::string &out) {
    out.clear();
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t crlf = raw.find("\r\n", pos);
        if (crlf == std::string::npos) return false;
        std::string hexLen = raw.substr(pos, crlf - pos);
        size_t semi = hexLen.find(';');
        if (semi != std::string::npos) hexLen.resize(semi);
        unsigned long chunkSz = std::strtoul(hexLen.c_str(), nullptr, 16);
        pos = crlf + 2;
        if (chunkSz == 0) return true;
        if (pos + chunkSz > raw.size()) return false;
        out.append(raw, pos, (size_t)chunkSz);
        pos += (size_t)chunkSz;
        if (pos + 2 <= raw.size() && raw[pos] == '\r' && raw[pos + 1] == '\n') pos += 2;
    }
    return true;
}

static bool ParseHttpResponse(const std::string &raw, int &status, std::string &body, std::string &cookieOut) {
    size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return false;
    std::string hdr = raw.substr(0, hdrEnd);
    std::string rawBody = raw.substr(hdrEnd + 4);

    size_t sp1 = hdr.find(' ');
    if (sp1 == std::string::npos) return false;
    status = std::atoi(hdr.c_str() + sp1 + 1);

    std::string hdrLow = hdr;
    for (char &c : hdrLow) c = (char)std::tolower((unsigned char)c);

    // Extract ankiweb=... cookie if present
    size_t scPos = 0;
    while ((scPos = hdrLow.find("set-cookie:", scPos)) != std::string::npos) {
        size_t lineEnd = hdr.find("\r\n", scPos);
        std::string line = (lineEnd == std::string::npos) ? hdr.substr(scPos) : hdr.substr(scPos, lineEnd - scPos);
        size_t ak = line.find("ankiweb=");
        if (ak != std::string::npos) {
            size_t valStart = ak + 8;
            size_t semi = line.find(';', valStart);
            std::string val = (semi == std::string::npos) ? line.substr(valStart) : line.substr(valStart, semi - valStart);
            if (!val.empty() && val != "deleted") {
                cookieOut = "ankiweb=" + val;
            }
        }
        scPos += 11;
    }

    if (hdrLow.find("transfer-encoding: chunked") != std::string::npos) {
        if (!DecodeChunkedBody(rawBody, body)) body = rawBody;
    } else {
        body = rawBody;
    }
    return true;
}

static bool IsHttpResponseComplete(const std::string &raw) {
    size_t hdrEnd = raw.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return false;
    std::string hdrLow = raw.substr(0, hdrEnd);
    for (char &c : hdrLow) c = (char)std::tolower((unsigned char)c);

    size_t clPos = hdrLow.find("content-length:");
    if (clPos != std::string::npos) {
        size_t valStart = clPos + 15;
        while (valStart < hdrLow.size() && (hdrLow[valStart] == ' ' || hdrLow[valStart] == '\t')) ++valStart;
        unsigned long cl = std::strtoul(hdrLow.c_str() + valStart, nullptr, 10);
        return raw.size() >= hdrEnd + 4 + (size_t)cl;
    }
    if (hdrLow.find("transfer-encoding: chunked") != std::string::npos) {
        size_t bodyStart = hdrEnd + 4;
        if (raw.compare(bodyStart, 5, "0\r\n\r\n") == 0) return true;
        if (raw.find("\r\n0\r\n\r\n", hdrEnd) != std::string::npos) return true;
        return false;
    }
    size_t sp1 = hdrLow.find(' ');
    if (sp1 != std::string::npos) {
        int st = std::atoi(hdrLow.c_str() + sp1 + 1);
        if (st == 204 || st == 301 || st == 302 || st == 303 || st == 304) return true;
    }
    return false;
}

static bool HttpPlainPost(const std::string &host, uint16_t port,
                          const std::string &jsonBody, int &status, std::string &respBody,
                          std::string &err, int recvTimeoutMs = 5000)
{
    int sock = TcpConnectTimeout(host, port, 5000, err);
    if (sock < 0) return false;

    std::string req = "POST / HTTP/1.1\r\n"
                      "Host: " + host + ":" + std::to_string(port) + "\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: " + std::to_string(jsonBody.size()) + "\r\n"
                      "Connection: close\r\n\r\n" + jsonBody;

    size_t sent = 0;
    while (sent < req.size()) {
        size_t chunk = std::min<size_t>(req.size() - sent, 0x4000);
        int n = send(sock, req.data() + sent, chunk, 0);
        if (n <= 0) {
            closesocket(sock);
            err = "HTTP send failed";
            return false;
        }
        sent += (size_t)n;
    }

    std::string raw;
    char buf[2048];
    for (;;) {
        short revents = 0;
        if (PollSliced(sock, POLLIN, recvTimeoutMs, revents) <= 0) break;
        if ((revents & (POLLERR | POLLHUP | POLLNVAL)) && !(revents & POLLIN)) break;
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, (size_t)n);
        if (IsHttpResponseComplete(raw)) break;
        if (raw.size() > 1024 * 1024) break;
    }
    closesocket(sock);

    std::string dummyCookie;
    if (!ParseHttpResponse(raw, status, respBody, dummyCookie)) {
        err = "Invalid HTTP response";
        return false;
    }
    return true;
}

static int BioSend(void *ctx, const unsigned char *buf, size_t len) {
    int sock = *(int *)ctx;
    short revents = 0;
    int sel = PollSliced(sock, POLLOUT, 8000, revents);
    if (sel <= 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    size_t chunk = std::min<size_t>(len, 0x4000);
    int ret = send(sock, (const char *)buf, chunk, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

static int BioRecv(void *ctx, unsigned char *buf, size_t len) {
    int sock = *(int *)ctx;
    short revents = 0;
    int sel = PollSliced(sock, POLLIN, 6000, revents);
    if (sel == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (sel < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    int ret = recv(sock, (char *)buf, len, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

static int Entropy3ds(void *data, unsigned char *output, size_t len, size_t *olen) {
    (void)data;
    static uint32_t ctr = 1;
    uint8_t seed[32];
    uint64_t t1 = svcGetSystemTick();
    uint64_t t2 = osGetTime();
    uint32_t ip = (uint32_t)gethostid();
    uint8_t raw[24];
    std::memcpy(raw, &t1, 8);
    std::memcpy(raw + 8, &t2, 8);
    std::memcpy(raw + 16, &ip, 4);
    std::memcpy(raw + 20, &ctr, 4);
    ++ctr;
    mbedtls_sha256(raw, sizeof(raw), seed, 0);
    size_t take = std::min(len, sizeof(seed));
    std::memcpy(output, seed, take);
    *olen = take;
    return 0;
}

static bool OpenHttpsSession(HttpsSession &sess, const std::string &host, uint16_t port, std::string &err) {
    sess.Close();
    int sock = TcpConnectTimeout(host, port, 7000, err);
    if (sock < 0) return false;

    mbedtls_ssl_init(&sess.ssl);
    mbedtls_ssl_config_init(&sess.conf);
    mbedtls_ctr_drbg_init(&sess.ctrDrbg);
    mbedtls_entropy_init(&sess.entropy);
    sess.sock = sock;
    sess.active = true;

    static const mbedtls_ecp_group_id kFastCurves[] = {
        MBEDTLS_ECP_DP_CURVE25519,
        MBEDTLS_ECP_DP_SECP256R1,
        MBEDTLS_ECP_DP_NONE
    };

    bool ok = false;
    do {
        mbedtls_entropy_add_source(&sess.entropy, Entropy3ds, nullptr, 32, MBEDTLS_ENTROPY_SOURCE_STRONG);
        if (mbedtls_ctr_drbg_seed(&sess.ctrDrbg, mbedtls_entropy_func, &sess.entropy,
                                  (const unsigned char *)"3ds_ocr", 7) != 0) {
            err = "TLS RNG seed failed";
            break;
        }
        if (mbedtls_ssl_config_defaults(&sess.conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT) != 0) {
            err = "TLS config failed";
            break;
        }
        mbedtls_ssl_conf_curves(&sess.conf, kFastCurves);
        mbedtls_ssl_conf_authmode(&sess.conf, MBEDTLS_SSL_VERIFY_NONE);
        mbedtls_ssl_conf_rng(&sess.conf, mbedtls_ctr_drbg_random, &sess.ctrDrbg);

        if (mbedtls_ssl_setup(&sess.ssl, &sess.conf) != 0) {
            err = "TLS setup failed";
            break;
        }
        mbedtls_ssl_set_hostname(&sess.ssl, host.c_str());
        mbedtls_ssl_set_bio(&sess.ssl, &sess.sock, BioSend, BioRecv, nullptr);
        if (host == "ankiuser.net" && s_hasSavedUserTlsSess) {
            mbedtls_ssl_set_session(&sess.ssl, &s_savedUserTlsSess);
        } else if (host == "ankiweb.net" && s_hasSavedMainTlsSess) {
            mbedtls_ssl_set_session(&sess.ssl, &s_savedMainTlsSess);
        }

        int hs = 0;
        while ((hs = mbedtls_ssl_handshake(&sess.ssl)) != 0) {
            if (hs != MBEDTLS_ERR_SSL_WANT_READ && hs != MBEDTLS_ERR_SSL_WANT_WRITE) {
                char hbuf[32];
                std::snprintf(hbuf, sizeof(hbuf), "TLS handshake -0x%X", (unsigned)(-hs));
                err = hbuf;
                break;
            }
        }
        if (hs == 0) {
            ok = true;
            if (host == "ankiuser.net") {
                if (!s_hasSavedUserTlsSess) {
                    mbedtls_ssl_session_init(&s_savedUserTlsSess);
                    s_hasSavedUserTlsSess = true;
                }
                mbedtls_ssl_get_session(&sess.ssl, &s_savedUserTlsSess);
            } else if (host == "ankiweb.net") {
                if (!s_hasSavedMainTlsSess) {
                    mbedtls_ssl_session_init(&s_savedMainTlsSess);
                    s_hasSavedMainTlsSess = true;
                }
                mbedtls_ssl_get_session(&sess.ssl, &s_savedMainTlsSess);
            }
        }
    } while (false);

    if (!ok) sess.Close();
    return ok;
}

static bool HttpsRequestKeepAlive(HttpsSession &sess, const std::string &host, uint16_t port,
                                  const char *method, const std::string &path,
                                  const char *contentType, const std::string &cookie,
                                  const std::string &body,
                                  int &status, std::string &respBody,
                                  std::string &setCookieOut, std::string &err)
{
    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "User-Agent: Mozilla/5.0\r\n"
                      "Origin: https://" + host + "\r\n"
                      "Connection: keep-alive\r\n";
    if (!cookie.empty()) req += "Cookie: " + cookie + "\r\n";
    if (contentType && *contentType) req += std::string("Content-Type: ") + contentType + "\r\n";
    if (std::strcmp(method, "POST") == 0) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    if (!body.empty()) req += body;

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!sess.active) {
            if (!OpenHttpsSession(sess, host, port, err)) return false;
        }

        size_t sent = 0;
        bool ioErr = false;
        while (sent < req.size()) {
            int w = mbedtls_ssl_write(&sess.ssl, (const unsigned char *)req.data() + sent, req.size() - sent);
            if (w <= 0) {
                if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
                ioErr = true;
                break;
            }
            sent += (size_t)w;
        }
        if (ioErr) {
            sess.Close();
            if (attempt == 0) continue;
            err = "TLS write failed";
            return false;
        }

        std::string raw;
        unsigned char rbuf[2048];
        for (;;) {
            int r = mbedtls_ssl_read(&sess.ssl, rbuf, sizeof(rbuf));
            if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            if (r <= 0) break;
            raw.append((const char *)rbuf, (size_t)r);
            if (IsHttpResponseComplete(raw)) break;
            if (raw.size() > 256 * 1024) break;
        }

        if (raw.empty()) {
            sess.Close();
            if (attempt == 0) continue;
            err = "TLS empty response";
            return false;
        }

        if (!ParseHttpResponse(raw, status, respBody, setCookieOut)) {
            sess.Close();
            err = "Bad HTTPS response";
            return false;
        }
        std::string hdrLow = raw.substr(0, raw.find("\r\n\r\n"));
        for (char &c : hdrLow) c = (char)std::tolower((unsigned char)c);
        if (hdrLow.find("connection: close") != std::string::npos) {
            sess.Close();
        }
        return true;
    }
    return false;
}

static bool HttpsRequest(const std::string &host, uint16_t port,
                         const char *method, const std::string &path,
                         const char *contentType, const std::string &cookie,
                         const std::string &body,
                         int &status, std::string &respBody,
                         std::string &setCookieOut, std::string &err)
{
    if (host == "ankiuser.net" && port == 443) {
        return HttpsRequestKeepAlive(s_ankiUserSession, host, port, method, path,
                                     contentType, cookie, body, status, respBody, setCookieOut, err);
    }
    if (host == "ankiweb.net" && port == 443) {
        return HttpsRequestKeepAlive(s_ankiWebSession, host, port, method, path,
                                     contentType, cookie, body, status, respBody, setCookieOut, err);
    }
    HttpsSession tmp;
    if (!OpenHttpsSession(tmp, host, port, err)) return false;

    std::string req = std::string(method) + " " + path + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "User-Agent: Mozilla/5.0\r\n"
                      "Origin: https://" + host + "\r\n"
                      "Connection: close\r\n";
    if (!cookie.empty()) req += "Cookie: " + cookie + "\r\n";
    if (contentType && *contentType) req += std::string("Content-Type: ") + contentType + "\r\n";
    if (std::strcmp(method, "POST") == 0) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n";
    if (!body.empty()) req += body;

    bool ok = false;
    size_t sent = 0;
    while (sent < req.size()) {
        int w = mbedtls_ssl_write(&tmp.ssl, (const unsigned char *)req.data() + sent, req.size() - sent);
        if (w <= 0) {
            if (w == MBEDTLS_ERR_SSL_WANT_READ || w == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
            err = "TLS write failed";
            tmp.Close();
            return false;
        }
        sent += (size_t)w;
    }
    std::string raw;
    unsigned char rbuf[2048];
    for (;;) {
        int r = mbedtls_ssl_read(&tmp.ssl, rbuf, sizeof(rbuf));
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r <= 0) break;
        raw.append((const char *)rbuf, (size_t)r);
        if (IsHttpResponseComplete(raw)) break;
        if (raw.size() > 256 * 1024) break;
    }
    tmp.Close();
    if (!ParseHttpResponse(raw, status, respBody, setCookieOut)) {
        err = "Bad HTTPS response";
        return false;
    }
    ok = true;
    return ok;
}

static bool AnkiWebLogin(const std::string &user, const std::string &pass, std::string &err) {
    if (user.empty() || pass.empty()) {
        err = "Set AnkiWeb login first";
        return false;
    }
    int status = 0;
    std::string resp, cookie;
    std::string loginBin = PbEncodeLoginReq(user, pass);
    if (!HttpsRequest("ankiweb.net", 443, "POST", "/svc/account/login",
                      "application/octet-stream", "", loginBin,
                      status, resp, cookie, err)) {
        return false;
    }
    if (status != 200) {
        err = "AnkiWeb login HTTP " + std::to_string(status);
        return false;
    }
    int loginStatus = 0;
    std::string token;
    if (!PbDecodeLoginResp(resp, loginStatus, token)) {
        err = "Bad login proto";
        return false;
    }
    if (loginStatus == 2) { err = "AnkiWeb: unknown email"; return false; }
    if (loginStatus == 3) { err = "AnkiWeb: wrong password"; return false; }
    if (loginStatus != 1 || token.empty()) {
        err = "AnkiWeb login rejected";
        return false;
    }

    int s2 = 0;
    std::string r2, c2;
    std::string authPath = "/account/ankiuser-login?t=" + UrlEncode(token);
    if (!HttpsRequest("ankiuser.net", 443, "GET", authPath,
                      nullptr, "", "", s2, r2, c2, err)) {
        return false;
    }
    if (c2.empty()) {
        // Some servers set the cookie on ankiweb.net too; fallback if first response set it
        if (!cookie.empty()) c2 = cookie;
    }
    if (c2.empty()) {
        err = "AnkiWeb: no session cookie";
        return false;
    }
    s_ankiWebMainCookie = cookie;
    s_ankiWebCookie = c2;
    return true;
}

static bool EnsureAnkiWebIds(const std::string &deckName, std::string &err, bool &needRelogin) {
    needRelogin = false;
    if (s_cachedNtId != 0 && s_cachedDeckId != 0 && s_cachedDeckName == deckName) {
        return true;
    }
    int status = 0;
    std::string resp, dummyCookie;
    if (!HttpsRequest("ankiuser.net", 443, "POST", "/svc/editor/get-info-for-adding",
                      "application/octet-stream", s_ankiWebCookie, "",
                      status, resp, dummyCookie, err)) {
        return false;
    }
    if (status == 401 || status == 403) {
        needRelogin = true;
        err = "Session expired";
        return false;
    }
    if (status != 200) {
        err = "AnkiWeb info HTTP " + std::to_string(status);
        return false;
    }
    std::vector<IdName> nts, decks;
    int64_t curDeck = 0, curNt = 0;
    if (!PbDecodeAddInfoResp(resp, nts, decks, curDeck, curNt)) {
        err = "Bad add-info proto";
        return false;
    }

    int64_t chosenNt = curNt;
    for (const auto &nt : nts) {
        if (nt.name == "Basic") { chosenNt = nt.id; break; }
    }
    if (chosenNt == 0 && !nts.empty()) chosenNt = nts[0].id;
    if (chosenNt == 0) {
        err = "No Basic note type";
        return false;
    }

    int64_t chosenDeck = 0;
    for (const auto &dk : decks) {
        if (dk.name == deckName) { chosenDeck = dk.id; break; }
    }
    if (chosenDeck == 0) {
        // Create deck on AnkiWeb
        int cs = 0;
        std::string cr, cc;
        if (!HttpsRequest("ankiuser.net", 443, "POST", "/svc/decks/create-deck",
                          "application/octet-stream", s_ankiWebCookie,
                          PbEncodeCreateDeckReq(deckName), cs, cr, cc, err)) {
            return false;
        }
        if (cs != 200) {
            err = "Create deck HTTP " + std::to_string(cs);
            return false;
        }
        // Re-fetch to get new deck ID
        if (!HttpsRequest("ankiuser.net", 443, "POST", "/svc/editor/get-info-for-adding",
                          "application/octet-stream", s_ankiWebCookie, "",
                          status, resp, dummyCookie, err) || status != 200) {
            err = "Re-fetch deck failed";
            return false;
        }
        decks.clear();
        PbDecodeAddInfoResp(resp, nts, decks, curDeck, curNt);
        for (const auto &dk : decks) {
            if (dk.name == deckName) { chosenDeck = dk.id; break; }
        }
        if (chosenDeck == 0) chosenDeck = curDeck;
    }
    if (chosenDeck == 0 && !decks.empty()) chosenDeck = decks[0].id;

    s_cachedNtId = chosenNt;
    s_cachedDeckName = deckName;
    s_cachedDeckId = chosenDeck;
    return true;
}

static bool ParseIntArrayFromResult(const std::string &jsonResp, std::vector<int64_t> &out) {
    out.clear();
    size_t rp = jsonResp.find("\"result\"");
    if (rp == std::string::npos) return false;
    size_t colon = jsonResp.find(':', rp + 8);
    if (colon == std::string::npos) return false;
    size_t lb = colon + 1;
    while (lb < jsonResp.size() && std::isspace((unsigned char)jsonResp[lb])) ++lb;
    if (lb >= jsonResp.size() || jsonResp[lb] != '[') return false;
    size_t rb = jsonResp.find(']', lb);
    if (rb == std::string::npos) return false;
    size_t p = lb + 1;
    while (p < rb) {
        while (p < rb && !std::isdigit((unsigned char)jsonResp[p])) ++p;
        if (p >= rb) break;
        int64_t val = std::strtoll(jsonResp.c_str() + p, nullptr, 10);
        if (val > 0) out.push_back(val);
        while (p < rb && std::isdigit((unsigned char)jsonResp[p])) ++p;
    }
    return true;
}

static const char *kAnkiAddedCachePath = "/luma/ocr_offline/anki_added.txt";

static bool LocalCacheHasCard(const std::string &deck, const std::string &hw) {
    OfflineFile f;
    if (!f.OpenRead(kAnkiAddedCachePath)) return false;
    uint64_t sz = f.GetSize();
    if (sz == 0 || sz > 2 * 1024 * 1024) return false;
    std::string data((size_t)sz, '\0');
    if (!f.ReadExact(&data[0], (size_t)sz)) return false;
    std::string needle = deck + "\t" + hw + "\n";
    return data.find(needle) != std::string::npos ||
           (data.size() + 1 >= needle.size() && data.compare(0, needle.size() - 1, needle, 0, needle.size() - 1) == 0 && data.size() == needle.size() - 1);
}

static void LocalCacheRecordCard(const std::string &deck, const std::string &hw) {
    if (LocalCacheHasCard(deck, hw)) return;
    OfflineFile f;
    if (f.OpenAppend(kAnkiAddedCachePath)) {
        std::string line = deck + "\t" + hw + "\n";
        f.WriteExact(line.data(), line.size());
        f.Close();
    }
}

bool PrewarmAnkiWeb(const UploadConfig &cfg, const std::atomic<bool> *cancelFlag) {
    if (cfg.ankiUser.empty() || cfg.ankiPass.empty()) return false;
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;
    AnkiLockGuard guard;
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;

    std::string err;
    std::string deck = cfg.deckName.empty() ? "3DS OCR" : cfg.deckName;
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;
        if (s_ankiWebCookie.empty()) {
            if (!AnkiWebLogin(cfg.ankiUser, cfg.ankiPass, err)) return false;
        }
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;
        bool needRelogin = false;
        if (!EnsureAnkiWebIds(deck, err, needRelogin)) {
            if (needRelogin && attempt == 0) { ResetCachedSessionLocked(); continue; }
            return false;
        }
        if (!s_ankiUserSession.active) {
            OpenHttpsSession(s_ankiUserSession, "ankiuser.net", 443, err);
        }
        return s_ankiUserSession.active;
    }
    return false;
}

static bool UploadViaAnkiWeb(const AnkiCard &card, const UploadConfig &cfg,
                             std::string &statusOut, std::string &err)
{
    std::string frontHtml, backHtml, tags;
    BuildCardHtml(card, frontHtml, backHtml, tags);
    std::string deck = cfg.deckName.empty() ? "3DS OCR" : cfg.deckName;
    std::string hwPrefix = "<div style=\"font-size:28px;font-weight:bold;\">" + HtmlEscape(card.headword) + "</div>";

    if (LocalCacheHasCard(deck, card.headword)) {
        statusOut = "Already in Anki";
        return true;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (s_ankiWebCookie.empty()) {
            if (!AnkiWebLogin(cfg.ankiUser, cfg.ankiPass, err)) return false;
        }
        bool needRelogin = false;
        if (!EnsureAnkiWebIds(deck, err, needRelogin)) {
            if (needRelogin && attempt == 0) { ResetCachedSessionLocked(); continue; }
            return false;
        }

        // Duplicate check via ankiweb.net /svc/search/search (using main ankiweb.net cookie)
        if (!s_ankiWebMainCookie.empty()) {
            int ss = 0;
            std::string sr, sc, sErr;
            std::string searchQuery = "deck:\"" + deck + "\" \"" + card.headword + "\"";
            if (HttpsRequest("ankiweb.net", 443, "POST", "/svc/search/search",
                             "application/octet-stream", s_ankiWebMainCookie,
                             PbEncodeSearchReq(searchQuery), ss, sr, sc, sErr)) {
                if ((ss == 401 || ss == 403) && attempt == 0) {
                    ResetCachedSessionLocked();
                    continue;
                }
                if (ss == 200 && PbDecodeSearchRespHasText(sr, hwPrefix)) {
                    LocalCacheRecordCard(deck, card.headword);
                    statusOut = "Already in Anki";
                    return true;
                }
            }
        }

        std::vector<uint8_t> lazySmall;
        const std::vector<uint8_t> *pngPtr = &card.screenshotPngSmall;
        if (pngPtr->empty() && card.topRgb && card.botRgb) {
            if (EncodeDualScreenJpeg(card.topRgb, card.botRgb, false, lazySmall, 78)) {
                pngPtr = &lazySmall;
            }
        }
        if (!pngPtr->empty()) {
            std::string b64 = Base64Encode(pngPtr->data(), pngPtr->size());
            frontHtml += "<div style=\"margin-top:10px;\"><img src=\"data:image/jpeg;base64," +
                         b64 + "\" style=\"max-width:100%;height:auto;border-radius:6px;\"></div>";
        }

        int status = 0;
        std::string resp, dummyCookie;
        std::string reqBin = PbEncodeAddNoteReq(frontHtml, backHtml, tags, s_cachedNtId, s_cachedDeckId);
        if (!HttpsRequest("ankiuser.net", 443, "POST", "/svc/editor/add-or-update",
                          "application/octet-stream", s_ankiWebCookie, reqBin,
                          status, resp, dummyCookie, err)) {
            return false;
        }
        if ((status == 401 || status == 403) && attempt == 0) {
            ResetCachedSessionLocked();
            continue;
        }
        if (status != 200) {
            err = "AnkiWeb add HTTP " + std::to_string(status);
            return false;
        }
        LocalCacheRecordCard(deck, card.headword);
        statusOut = "Added via AnkiWeb";
        return true;
    }
    return false;
}

static bool UploadViaAnkiConnect(const AnkiCard &card, const UploadConfig &cfg,
                                 std::string &statusOut, std::string &err)
{
    if (cfg.connectHost.empty()) {
        err = "No AnkiConnect host set";
        return false;
    }
    uint16_t port = (cfg.connectPort == 0) ? 8765 : cfg.connectPort;
    std::string deck = cfg.deckName.empty() ? "3DS OCR" : cfg.deckName;

    std::string frontHtml, backHtml, tags;
    BuildCardHtml(card, frontHtml, backHtml, tags);
    std::string hwPrefix = "<div style=\"font-size:28px;font-weight:bold;\">" + HtmlEscape(card.headword) + "</div>";

    int st = 0;
    std::string resp;
    if (s_acCachedDeck != deck) {
        if (!HttpPlainPost(cfg.connectHost, port, BuildAnkiConnectCreateDeckJson(deck), st, resp, err)) {
            return false;
        }
        if (st != 200) {
            err = "AnkiConnect HTTP " + std::to_string(st);
            return false;
        }
        s_acCachedDeck = deck;
    }

    // Duplicate check via findNotes + batched notesInfo (ignoring screenshot filename and decoding \uXXXX)
    std::string fnResp, fnErr;
    int fnSt = 0;
    if (HttpPlainPost(cfg.connectHost, port, BuildAnkiConnectFindNotesJson(deck, card.headword), fnSt, fnResp, fnErr) && fnSt == 200) {
        std::vector<int64_t> nids;
        if (ParseIntArrayFromResult(fnResp, nids) && !nids.empty()) {
            const size_t kBatchSize = 10;
            for (size_t off = 0; off < nids.size(); off += kBatchSize) {
                size_t end = std::min(nids.size(), off + kBatchSize);
                std::vector<int64_t> batch(nids.begin() + off, nids.begin() + end);
                std::string niResp, niErr;
                int niSt = 0;
                if (HttpPlainPost(cfg.connectHost, port, BuildAnkiConnectNotesInfoJson(batch), niSt, niResp, niErr) && niSt == 200) {
                    if (AnkiConnectNotesInfoHasFrontPrefix(niResp, hwPrefix)) {
                        LocalCacheRecordCard(deck, card.headword);
                        statusOut = "Already in Anki";
                        return true;
                    }
                }
            }
        }
    }

    std::vector<uint8_t> lazyFull;
    const std::vector<uint8_t> *pngPtr = &card.screenshotPng;
    if (pngPtr->empty() && card.topRgb && card.botRgb) {
        if (EncodeDualScreenJpeg(card.topRgb, card.botRgb, false, lazyFull, 78)) {
            pngPtr = &lazyFull;
        }
    }
    if (!pngPtr->empty()) {
        std::string fname = card.screenshotName.empty() ? "3ds_ocr_screen.jpg" : card.screenshotName;
        std::string b64 = Base64Encode(pngPtr->data(), pngPtr->size());
        int ms = 0;
        std::string mr, mErr;
        if (HttpPlainPost(cfg.connectHost, port, BuildAnkiConnectStoreMediaJson(fname, b64), ms, mr, mErr) && ms == 200) {
            frontHtml += "<div style=\"margin-top:10px;\"><img src=\"" + HtmlEscape(fname) +
                         "\" style=\"max-width:100%;height:auto;border-radius:6px;\"></div>";
        }
    }

    if (!HttpPlainPost(cfg.connectHost, port,
                       BuildAnkiConnectAddNoteJson(deck, frontHtml, backHtml, tags),
                       st, resp, err)) {
        return false;
    }
    if (st != 200) {
        err = "AnkiConnect add HTTP " + std::to_string(st);
        return false;
    }
    if (resp.find("\"error\":null") == std::string::npos && resp.find("\"error\": null") == std::string::npos) {
        if (resp.find("duplicate") != std::string::npos) {
            LocalCacheRecordCard(deck, card.headword);
            statusOut = "Already in Anki";
            return true;
        }
        err = "AnkiConnect error";
        return false;
    }

    // Fire-and-forget trigger desktop sync (50ms recv cap so we don't block on cloud sync)
    std::string syncResp, syncErr;
    int syncSt = 0;
    HttpPlainPost(cfg.connectHost, port, BuildAnkiConnectSyncJson(), syncSt, syncResp, syncErr, 50);

    LocalCacheRecordCard(deck, card.headword);
    statusOut = "Added via AnkiConnect";
    return true;
}

bool UploadCard(const AnkiCard &card, const UploadConfig &cfg,
                std::string &statusOut, std::string &err)
{
    AnkiLockGuard guard;
    statusOut.clear();
    err.clear();
    switch (cfg.mode) {
        case MODE_ANKIWEB:
            return UploadViaAnkiWeb(card, cfg, statusOut, err);
        case MODE_ANKICONNECT:
            return UploadViaAnkiConnect(card, cfg, statusOut, err);
        case MODE_WEB_THEN_CONNECT: {
            if (UploadViaAnkiWeb(card, cfg, statusOut, err)) return true;
            std::string err2;
            if (UploadViaAnkiConnect(card, cfg, statusOut, err2)) return true;
            err = err + " | " + err2;
            return false;
        }
        case MODE_CONNECT_THEN_WEB: {
            if (UploadViaAnkiConnect(card, cfg, statusOut, err)) return true;
            std::string err2;
            if (UploadViaAnkiWeb(card, cfg, statusOut, err2)) return true;
            err = err + " | " + err2;
            return false;
        }
        default:
            err = "Invalid Anki mode";
            return false;
    }
}

#else

void CloseIdleConnection(void) {}
void ResetCachedSession(void) {}
bool PrewarmAnkiWeb(const UploadConfig &cfg, const std::atomic<bool> *cancelFlag) {
    (void)cfg; (void)cancelFlag;
    return false;
}

bool UploadCard(const AnkiCard &card, const UploadConfig &cfg,
                std::string &statusOut, std::string &err)
{
    (void)card; (void)cfg; (void)statusOut;
    err = "Network unavailable in host test";
    return false;
}

#endif

} // namespace OfflineAnki
