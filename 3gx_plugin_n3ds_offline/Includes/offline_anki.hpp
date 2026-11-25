#ifndef OFFLINE_ANKI_HPP
#define OFFLINE_ANKI_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace OfflineAnki {

enum UploadMode : uint8_t {
    MODE_ANKIWEB = 0,          // 1. AnkiWeb Only (HTTPS)
    MODE_ANKICONNECT = 1,      // 2. AnkiConnect Only (LAN HTTP)
    MODE_WEB_THEN_CONNECT = 2, // 3. AnkiWeb -> AnkiConnect fallback
    MODE_CONNECT_THEN_WEB = 3, // 4. AnkiConnect -> AnkiWeb fallback
};

struct AnkiCard {
    uint64_t titleId{0};
    std::string headword;
    std::string reading;
    std::string pos;
    std::string gloss;
    std::string context;
    std::vector<uint8_t> screenshotPng;      // Full-res 400x480 PNG (for AnkiConnect)
    std::vector<uint8_t> screenshotPngSmall; // Half-res 200x240 PNG (for AnkiWeb inline data URI)
    std::string screenshotName;
    const uint8_t *topRgb{nullptr};          // Optional raw RGB24 pointers for lazy encoding
    const uint8_t *botRgb{nullptr};
};

struct UploadConfig {
    UploadMode mode{MODE_WEB_THEN_CONNECT};
    std::string ankiUser;
    std::string ankiPass;
    std::string connectHost;
    uint16_t connectPort{8765};
    std::string deckName{"3DS OCR"};
    uint32_t netMs{0};
};

struct IdName {
    int64_t id{0};
    std::string name;
};

// Append a dictionary entry to the offline Anki queue JSONL file on SD card.
bool QueueEntry(const char *queuePath, uint64_t titleId,
                const std::string &headword, const std::string &reading,
                const std::string &pos, const std::string &gloss,
                const std::string &context, std::string &err);

// Count queued entries waiting in the JSONL file.
int CountQueued(const char *queuePath);

// Load all queued entries from the JSONL file.
bool LoadQueued(const char *queuePath, std::vector<AnkiCard> &out);

// Rewrite the JSONL file with remaining (unsynced) entries; removes file if empty.
bool RewriteQueue(const char *queuePath, const std::vector<AnkiCard> &remaining, std::string &err);

// Resolve 3DS TitleID to game deck name (checking /luma/ocr_titles.txt + built-in table).
std::string ResolveDeckName(uint64_t titleId, const std::string &fallbackName = "3DS OCR");

// Encode top (400x240 RGB24) + bottom (320x240 RGB24, centered) as 400x480 (or 200x240) PNG.
bool EncodeDualScreenPng(const uint8_t *topRgb, const uint8_t *botRgb,
                         bool halfScale, std::vector<uint8_t> &outPng);

// Encode top + bottom as compact baseline JPEG (~10x smaller and much faster than PNG).
bool EncodeDualScreenJpeg(const uint8_t *topRgb, const uint8_t *botRgb,
                          bool halfScale, std::vector<uint8_t> &outJpg, int quality = 75);

// Base64 encode raw bytes.
std::string Base64Encode(const uint8_t *data, size_t len);

// Build Anki card HTML fields + tags matching ocr_server.py styling.
void BuildCardHtml(const AnkiCard &card, std::string &frontHtml, std::string &backHtml, std::string &tags);

// Minimal protobuf codecs for AnkiWeb /svc/ binary RPC (exposed for unit testing).
std::string PbEncodeLoginReq(const std::string &user, const std::string &pass);
bool PbDecodeLoginResp(const std::string &buf, int &status, std::string &token);
bool PbDecodeAddInfoResp(const std::string &buf,
                         std::vector<IdName> &notetypes,
                         std::vector<IdName> &decks,
                         int64_t &curDeckId,
                         int64_t &curNtId);
std::string PbEncodeCreateDeckReq(const std::string &name);
std::string PbEncodeAddNoteReq(const std::string &front,
                               const std::string &back,
                               const std::string &tags,
                               int64_t notetypeId,
                               int64_t deckId);
std::string PbEncodeSearchReq(const std::string &query);
bool PbDecodeSearchRespHasText(const std::string &buf, const std::string &needle);

// AnkiConnect JSON payload builders (exposed for unit testing).
std::string BuildAnkiConnectCreateDeckJson(const std::string &deckName);
std::string BuildAnkiConnectStoreMediaJson(const std::string &filename, const std::string &b64Data);
std::string BuildAnkiConnectFindNotesJson(const std::string &deckName, const std::string &headword);
std::string BuildAnkiConnectNotesInfoJson(const std::vector<int64_t> &noteIds);
std::string BuildAnkiConnectAddNoteJson(const std::string &deckName,
                                        const std::string &frontHtml,
                                        const std::string &backHtml,
                                        const std::string &tags);
std::string BuildAnkiConnectSyncJson(void);

// Upload a single card directly using the configured mode/fallback.
// On 3DS, requires network (soc:u) already initialized; returns false on failure with short err.
bool UploadCard(const AnkiCard &card, const UploadConfig &cfg,
                std::string &statusOut, std::string &err);

// Clear any cached AnkiWeb session cookie/IDs (e.g. when credentials change).
void ResetCachedSession(void);

// Close idle TLS socket while keeping cookie, deck/notetype IDs, and TLS resumption ticket.
void CloseIdleConnection(void);

// Pre-warm AnkiWeb session (login + TLS handshake + deck/notetype IDs) in background.
bool PrewarmAnkiWeb(const UploadConfig &cfg, const std::atomic<bool> *cancelFlag = nullptr);

} // namespace OfflineAnki

#endif // OFFLINE_ANKI_HPP
