#ifndef OFFLINE_OCR_HPP
#define OFFLINE_OCR_HPP

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace OfflineOcr {

struct CharSpan {
    uint32_t cp{0};
    float xStartRatio{0.0f};
    float xEndRatio{0.0f};
};

struct OcrLine {
    std::string text;
    std::vector<CharSpan> chars;
    int cropX1{0}, cropY1{0}, cropX2{0}, cropY2{0};
};

struct TextBox {
    int x1{0}, y1{0}, x2{0}, y2{0};
};

struct LoadedModel {
    std::vector<uint8_t> modelBuf;
    size_t opsOffset{0};
    uint32_t numOps{0};
    std::vector<std::string> vocab;
    std::vector<float> actOutScales;
    bool loaded{false};
};

struct DetOp {
    uint8_t type{0};
    uint16_t in0{0}, in1{0}, in2{0}, in3{0}, out{0};
    uint16_t inC{0}, outC{0}, kH{0}, kW{0}, sH{0}, sW{0}, pH{0}, pW{0}, groups{1}, act{0};
    uint16_t scale{1};
    float postScale{1.0f}, postBias{0.0f};
    size_t dataOffset{0};
    size_t dataSize{0};
};

struct LoadedDetModel {
    std::string filePath;
    std::vector<uint8_t> modelBuf;
    uint32_t numSlots{0};
    uint32_t inputSlot{0};
    uint32_t outputSlot{0};
    std::vector<DetOp> ops;
    bool loaded{false};
};

struct RecScratch {
    std::vector<float> bufA;
    std::vector<float> bufB;
    std::vector<float> slot1;
    std::vector<float> slot2;
    std::vector<float> nchw;
    std::vector<int16_t> inHwc;
    std::vector<float> scalesVec;
    std::vector<float> biasVec;
};

struct DetScratch {
    struct ArenaBlock {
        size_t off;
        size_t sz;
        bool used;
        ArenaBlock(size_t o = 0, size_t s = 0, bool u = false) : off(o), sz(s), used(u) {}
    };
    uint8_t *arenaBase{nullptr};
    size_t arenaSize{0};
    bool arenaIsSvc{false};
    std::vector<ArenaBlock> blocks;
    std::vector<float> scalesVec;
    std::vector<float> biasVec;
    std::vector<uint8_t> visited;
    std::vector<int> qx;
    std::vector<int> qy;

    ~DetScratch() { Reset(); }
    void InitArena();
    void *AllocBytes(size_t bytes, bool &fromArena);
    void FreeBytes(void *ptr, bool fromArena);
    void Reset();
};

struct Conv1x1OpStat {
    uint16_t cin{0}, cout{0};
    uint8_t act{0}, isS16{0};
    uint32_t calls{0};
    uint64_t sumOutHW{0};
    uint64_t totalMacs{0};
    uint64_t quantTicks{0};
    uint64_t gemmTicks{0};
    uint64_t waitTicks{0};
};

struct OcrProfile {
    uint64_t preprocTicks{0};
    uint64_t conv1x1QuantTicks{0};
    uint64_t conv1x1GemmTicks{0};
    uint64_t convDwTicks{0};
    uint64_t convIm2ColTicks{0};
    uint64_t svtrLinTicks{0};
    uint64_t svtrAttnTicks{0};
    uint64_t ctcTicks{0};
    uint64_t otherTicks{0};
    uint64_t quantMaxAbsTicks{0};
    uint64_t quantTransposeTicks{0};
    uint64_t quantWaitTicks{0};
    uint64_t gemmUnpackTicks{0};
    uint64_t gemmWaitTicks{0};
    uint64_t gemmSingleThreadTicks{0};
    uint32_t gemmSingleThreadCalls{0};
    uint32_t gemmMultiThreadCalls{0};
    uint64_t opTicks[64]{};
    Conv1x1OpStat conv1x1Ops[64]{};
    uint32_t boxCount{0};
    uint32_t totalOutW{0};
    uint32_t dispatchCount{0};
    uint64_t helperBusyTicks[4]{};
    void Reset() { *this = OcrProfile(); }
};

struct DetProfile {
    uint64_t preprocTicks{0};
    uint64_t ioTicks{0};
    uint64_t conv1x1QuantTicks{0};
    uint64_t conv1x1GemmTicks{0};
    uint64_t convDwTicks{0};
    uint64_t convIm2ColTicks{0};
    uint64_t convT2x2Ticks{0};
    uint64_t addTicks{0};
    uint64_t mulChTicks{0};
    uint64_t gapTicks{0};
    uint64_t resizeTicks{0};
    uint64_t concatTicks{0};
    uint64_t postprocTicks{0};
    uint64_t opTicks[160]{};
    uint64_t helperBusyTicks[4]{};
    uint32_t dispatchCount{0};
    uint32_t numOps{0};
    void Reset() { *this = DetProfile(); }
};

// Number of active CPU cores used for parallel inference (probed safely on 3DS).
int GetActiveCoreCount();
void GetAndResetHelperProfile(uint64_t outBusyTicks[4], uint32_t &outDispatches);

// Check that the model file exists and has a valid PPOCRV4I header.
bool CheckModelFile(const char *modelPath, std::string *outInfo = nullptr);

// Load model weights & vocab once into memory for reuse across lines.
bool LoadModel(const char *modelPath, LoadedModel &outModel, std::string &err);

// Load DBNet text-detection model (OCRDET01) into memory.
bool LoadDetModel(const char *detModelPath, LoadedDetModel &outModel, std::string &err);

// Fast classical text-line detector for 400x240 / 320x240 3DS screen captures.
void DetectScreenTextLines(const uint8_t *rgb, int imgW, int imgH, std::vector<TextBox> &outBoxes);

// Neural DBNet text-line detector for 400x240 / 320x240 3DS screen captures.
bool DetectScreenTextLinesDbNet(const uint8_t *rgb, int imgW, int imgH,
                                const LoadedDetModel &detModel,
                                std::vector<TextBox> &outBoxes, std::string &err,
                                bool allowDualCore = false, const std::atomic<bool> *abortFlag = nullptr,
                                int helperCore = 2, DetProfile *detProf = nullptr,
                                DetScratch *detScratch = nullptr);

// Run OCR on a preprocessed NCHW float32 tensor [1, 3, 48, W] (W multiple of 8, <= 320).
bool RunPreprocessed(const float *nchw, int width, const LoadedModel &model,
                     OcrLine &outLine, std::string &err,
                     bool allowDualCore = true, const std::atomic<bool> *abortFlag = nullptr,
                     int helperCore = 2, RecScratch *scratch = nullptr,
                     OcrProfile *prof = nullptr, float *calMaxAbsOut = nullptr);

bool RunPreprocessed(const float *nchw, int width, const char *modelPath,
                     OcrLine &outLine, std::string &err);

// Run OCR on a detected TextBox, returning OcrLine with spans normalized to cropX1..cropX2.
bool RunBox(const uint8_t *rgb, int imgW, int imgH, const TextBox &box,
            const LoadedModel &model, OcrLine &outLine, std::string &err,
            bool allowDualCore = false, const std::atomic<bool> *abortFlag = nullptr,
            int helperCore = 2, RecScratch *scratch = nullptr,
            OcrProfile *prof = nullptr);

// Run OCR across a list of detected TextBoxes in parallel across active cores.
bool RunBoxesBatch(const uint8_t *rgb, int imgW, int imgH,
                   const std::vector<TextBox> &boxes,
                   const LoadedModel &model,
                   std::vector<OcrLine> &outLines,
                   std::vector<uint8_t> &doneMask,
                   std::string &err,
                   std::atomic<int> *progressCur = nullptr,
                   const std::atomic<bool> *abortFlag = nullptr,
                   int helperCore = 0,
                   OcrProfile *prof = nullptr);

// Single-word mode: crop a horizontal text band around (clickX, clickY),
// run OCR, and return the recognized line + estimated character index under clickX.
bool RunWord(const uint8_t *rgb, int imgW, int imgH, int clickX, int clickY,
             const LoadedModel &model, OcrLine &outLine, int &outCharIdx, std::string &err,
             bool allowDualCore = true, const std::atomic<bool> *abortFlag = nullptr,
             int helperCore = 2);

bool RunWord(const uint8_t *rgb, int imgW, int imgH, int clickX, int clickY,
             const char *modelPath, OcrLine &outLine, int &outCharIdx, std::string &err);

// Region mode: split [x1, y1, x2, y2] into horizontal text lines and run OCR on each.
bool RunRegion(const uint8_t *rgb, int imgW, int imgH,
               int x1, int y1, int x2, int y2,
               const LoadedModel &model, std::vector<OcrLine> &outLines, std::string &err,
               bool allowDualCore = true, const std::atomic<bool> *abortFlag = nullptr,
               int helperCore = 2);

bool RunRegion(const uint8_t *rgb, int imgW, int imgH,
               int x1, int y1, int x2, int y2,
               const char *modelPath, std::vector<OcrLine> &outLines, std::string &err);

} // namespace OfflineOcr

#endif // OFFLINE_OCR_HPP
