#include "ocr_client.hpp"
#include "sha256.h"
#include "quirc.h"
#include "csvc.h"


#include <3ds/services/ndm.h>
#include <3ds/services/ac.h>
#include <sys/socket.h>
#include <sys/iosupport.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <malloc.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <string>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <atomic>
#include <zlib.h>
#include <CTRPluginFrameworkImpl/System/SystemImpl.hpp>

namespace CTRPluginFramework
{
    static std::atomic<bool> s_sleepOrSuspendActive{false};
    static std::atomic<int>  s_pausedOverlayDepth{0};

    struct ScopedPausedOverlay {
        bool active{false};
        explicit ScopedPausedOverlay(bool track = true) : active(track) {
            if (active) s_pausedOverlayDepth.fetch_add(1, std::memory_order_acq_rel);
        }
        ~ScopedPausedOverlay() {
            if (active) s_pausedOverlayDepth.fetch_sub(1, std::memory_order_acq_rel);
        }
    };

    static bool OCR_ShouldAbortForSleep(void)
    {
        return s_sleepOrSuspendActive.load(std::memory_order_acquire) || SystemImpl::IsSleeping();
    }

    bool OCR_IsSleeping(void)
    {
        return OCR_ShouldAbortForSleep();
    }

    void OCR_OnProcessEvent(Process::Event event)
    {
        if (event == Process::Event::SLEEP_ENTER ||
            event == Process::Event::HOME_ENTER ||
            event == Process::Event::SWAP_ENTER ||
            event == Process::Event::EXIT)
        {
            s_sleepOrSuspendActive.store(true, std::memory_order_release);
            if (event == Process::Event::SLEEP_ENTER || event == Process::Event::HOME_ENTER) {
                // Set CTRPF's sleep flag early so any open CTRPF Keyboard/MessageBox also aborts immediately
                SystemImpl::AptStatus |= BIT(6);
            }
            // Wait for any active paused overlay to clean up and call Process::Play() BEFORE PLGLDR__Reply()
            for (int i = 0; i < 300 && s_pausedOverlayDepth.load(std::memory_order_acquire) > 0; ++i) {
                svcSleepThread(5000000ULL); // 5ms, max 1.5s
            }
        }
        else if (event == Process::Event::SLEEP_EXIT ||
                 event == Process::Event::HOME_EXIT ||
                 event == Process::Event::SWAP_EXIT)
        {
            s_sleepOrSuspendActive.store(false, std::memory_order_release);
        }
    }

    // Global static socket context using Luma3DS system memory allocation
    static bool s_socInitialized = false;
    static bool s_socBorrowed = false;
    static u32  s_socBufferAddr = 0;
    static u32  s_socAliasAddr = 0;
    static u32  s_socBufferSize = 0;
    static bool s_socBufferIsHeap = false;
    static const u32 SOC_BUFFER_SIZE = 0x48000; // 288 KB (minimum required by 3DS soc:u service)

    extern "C" {
        extern Handle SOCU_handle;
        int _net_convert_error(s32 sock_retval);
        ssize_t soc_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
        ssize_t soc_sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
    }

    static int BorrowedSocOpen(struct _reent *r, void *fileStruct, const char *path, int flags, int mode)
    {
        (void)fileStruct; (void)path; (void)flags; (void)mode;
        r->_errno = ENOSYS;
        return -1;
    }

    static int BorrowedSocClose(struct _reent *r, void *fd)
    {
        u32 *cmdbuf = getThreadCommandBuffer();
        cmdbuf[0] = 0x000B0042;
        cmdbuf[1] = static_cast<u32>(*static_cast<int *>(fd));
        cmdbuf[2] = 0x20;
        Result ret = svcSendSyncRequest(SOCU_handle);
        if (R_FAILED(ret)) {
            r->_errno = ENODEV;
            return ret;
        }
        s32 res = static_cast<s32>(cmdbuf[1]);
        if (res == 0)
            res = _net_convert_error(static_cast<s32>(cmdbuf[2]));
        if (res < 0) {
            r->_errno = -res;
            return -1;
        }
        return 0;
    }

    static ssize_t BorrowedSocWrite(struct _reent *r, void *fd, const char *ptr, size_t len)
    {
        (void)r;
        return soc_sendto(*static_cast<int *>(fd), ptr, len, 0, nullptr, 0);
    }

    static ssize_t BorrowedSocRead(struct _reent *r, void *fd, char *ptr, size_t len)
    {
        (void)r;
        return soc_recvfrom(*static_cast<int *>(fd), ptr, len, 0, nullptr, nullptr);
    }

    static const devoptab_t s_borrowedSocDevoptab = {
        "soc",
        sizeof(int),
        BorrowedSocOpen,
        BorrowedSocClose,
        BorrowedSocWrite,
        BorrowedSocRead,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        0,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };

    struct HostSocSearchDiag {
        u32 hits00010044 = 0;
        u32 candAddr = 0;
        u32 candVal = 0;
        int roHits = 0;
        Handle handle = 0;
    };

    static bool OCR_IsReadableAddress(u32 addr)
    {
        if ((addr & 3) != 0 || addr < 0x00100000 || addr >= 0x08000000)
            return false;
        MemInfo mi{};
        PageInfo pi{};
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)))
            return false;
        return mi.state != MEMSTATE_FREE && (mi.perm & MEMPERM_READ) != 0;
    }

    static u32 OCR_FindFreeVa(u32 size)
    {
        const u32 ranges[2][2] = {
            { 0x07500000, 0x07F80000 },
            { 0x06000000, 0x06F80000 }
        };
        for (int r = 0; r < 2; ++r) {
            u32 addr = ranges[r][0];
            while (addr < ranges[r][1]) {
                MemInfo mi{};
                PageInfo pi{};
                if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) {
                    addr += 0x10000;
                    continue;
                }
                u64 regionEnd = (u64)mi.base_addr + (u64)mi.size;
                if (mi.state == MEMSTATE_FREE && regionEnd >= (u64)addr + (u64)size) {
                    return addr;
                }
                u64 next = (regionEnd + 0xFFFULL) & ~0xFFFULL;
                if (next <= (u64)addr) next = (u64)addr + 0x1000ULL;
                if (next >= (u64)ranges[r][1]) break;
                addr = (u32)next;
            }
        }
        return 0;
    }

    static Handle OCR_FindHostSocuHandle(HostSocSearchDiag *outDiag = nullptr)
    {
        HostSocSearchDiag diag;
        const u32 textStart = 0x00100000;
        const u32 textSize = Process::GetTextSize();
        const u32 textEnd = textStart + textSize;

        // Pass 1: Scan for SOCU_Initialize command header 0x00010044 in literal pools
        for (u32 litAddr = textStart; litAddr + sizeof(u32) <= textEnd; litAddr += sizeof(u32)) {
            if (*reinterpret_cast<const u32 *>(litAddr) != 0x00010044)
                continue;

            diag.hits00010044++;
            const u32 offHi = std::min<u32>(0xFF0, litAddr - textStart);
            for (u32 off = 4; off <= offHi; off += 4) {
                const u32 ldrCmdAddr = litAddr - off;
                const u32 ldrCmdInsn = *reinterpret_cast<const u32 *>(ldrCmdAddr);
                // Match ldr r*, [pc, #off] or [pc, #-off]
                if ((ldrCmdInsn & 0x0F7F0000) != 0x051F0000)
                    continue;
                const bool uBit = (ldrCmdInsn & 0x00800000) != 0;
                const u32 target = uBit ? (ldrCmdAddr + 8 + (ldrCmdInsn & 0xFFF))
                                        : (ldrCmdAddr + 8 - (ldrCmdInsn & 0xFFF));
                if (target != litAddr)
                    continue;

                const u32 searchLo = (ldrCmdAddr >= textStart + 64) ? (ldrCmdAddr - 64) : textStart;
                const u32 searchHi = std::min(textEnd, ldrCmdAddr + 64);
                for (u32 insnAddr = searchLo; insnAddr < searchHi; insnAddr += 4) {
                    const u32 insn = *reinterpret_cast<const u32 *>(insnAddr);
                    if ((insn & 0x0F7F0000) != 0x051F0000)
                        continue;

                    const u32 reg = (insn >> 12) & 0xF;
                    const bool ptrUp = (insn & 0x00800000) != 0;
                    const u32 ptrLitAddr = ptrUp ? (insnAddr + 8 + (insn & 0xFFF))
                                                 : (insnAddr + 8 - (insn & 0xFFF));
                    if (ptrLitAddr < textStart || ptrLitAddr + sizeof(u32) > textEnd)
                        continue;

                    const u32 baseAddr = *reinterpret_cast<const u32 *>(ptrLitAddr);
                    if (baseAddr < textEnd || baseAddr >= 0x08000000)
                        continue;

                    for (u32 k = insnAddr + 4; k < std::min(textEnd, insnAddr + 64); k += 4) {
                        const u32 kInsn = *reinterpret_cast<const u32 *>(k);
                        // Match `ldr r0, [r<reg>, #imm]` (including #0)
                        if ((kInsn & 0x0F7FF000) == (0x05100000 | (reg << 16))) {
                            const bool kUp = (kInsn & 0x00800000) != 0;
                            const u32 disp = kInsn & 0xFFF;
                            const u32 handleVarAddr = kUp ? (baseAddr + disp) : (baseAddr - disp);
                            if (OCR_IsReadableAddress(handleVarAddr)) {
                                const Handle h = *reinterpret_cast<const Handle *>(handleVarAddr);
                                diag.candAddr = handleVarAddr;
                                diag.candVal = h;
                                if (h != 0) {
                                    diag.handle = h;
                                    if (outDiag) *outDiag = diag;
                                    return h;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Pass 2: Scan .rodata for "soc:U" / "soc:u" used in srvGetServiceHandle(&SOCU_handle, "soc:U")
        const u32 roStart = textEnd;
        const u32 roEnd = roStart + Process::GetRoDataSize();
        for (u32 sAddr = roStart; sAddr + 6 <= roEnd; ++sAddr) {
            const char *s = reinterpret_cast<const char *>(sAddr);
            if (!(s[0] == 's' && s[1] == 'o' && s[2] == 'c' && s[3] == ':' && (s[4] == 'U' || s[4] == 'u') && s[5] == '\0'))
                continue;

            for (u32 litAddr = textStart; litAddr + sizeof(u32) <= textEnd; litAddr += sizeof(u32)) {
                if (*reinterpret_cast<const u32 *>(litAddr) != sAddr)
                    continue;
                diag.roHits++;

                const u32 offHi = std::min<u32>(0xFF0, litAddr - textStart);
                for (u32 off = 4; off <= offHi; off += 4) {
                    const u32 ldrStrAddr = litAddr - off;
                    const u32 ldrStrInsn = *reinterpret_cast<const u32 *>(ldrStrAddr);
                    if ((ldrStrInsn & 0x0F7FF000) != 0x051F1000) // ldr r1, [pc, #off]
                        continue;
                    const bool uBit = (ldrStrInsn & 0x00800000) != 0;
                    const u32 target = uBit ? (ldrStrAddr + 8 + (ldrStrInsn & 0xFFF))
                                            : (ldrStrAddr + 8 - (ldrStrInsn & 0xFFF));
                    if (target != litAddr)
                        continue;

                    const u32 searchLo = (ldrStrAddr >= textStart + 32) ? (ldrStrAddr - 32) : textStart;
                    const u32 searchHi = std::min(textEnd, ldrStrAddr + 32);
                    for (u32 insnAddr = searchLo; insnAddr < searchHi; insnAddr += 4) {
                        const u32 insn = *reinterpret_cast<const u32 *>(insnAddr);
                        if ((insn & 0x0F7FF000) != 0x051F0000) // ldr r0, [pc, #off]
                            continue;
                        const bool ptrUp = (insn & 0x00800000) != 0;
                        const u32 ptrLitAddr = ptrUp ? (insnAddr + 8 + (insn & 0xFFF))
                                                     : (insnAddr + 8 - (insn & 0xFFF));
                        if (ptrLitAddr < textStart || ptrLitAddr + sizeof(u32) > textEnd)
                            continue;
                        const u32 handleVarAddr = *reinterpret_cast<const u32 *>(ptrLitAddr);
                        if (OCR_IsReadableAddress(handleVarAddr)) {
                            const Handle h = *reinterpret_cast<const Handle *>(handleVarAddr);
                            diag.candAddr = handleVarAddr;
                            diag.candVal = h;
                            if (h != 0) {
                                diag.handle = h;
                                if (outDiag) *outDiag = diag;
                                return h;
                            }
                        }
                    }
                }
            }
        }

        if (outDiag) *outDiag = diag;
        return 0;
    }

    // Configurable server endpoints (loaded from /luma/ocr_config.txt or updated via QR code)
    static std::string s_serverLanHost = "";
    static uint16_t    s_serverLanPort = 8888;
    static std::string s_serverWanHost = "";
    static uint16_t    s_serverWanPort = 8888;
    static std::string s_lastConnectedEndpoint = "";

    struct SessionBuffer {
        u8 *ptr = nullptr;
        u32 size = 0;
        bool isSvc = false;

        bool Alloc(u32 vaddr, u32 reqSize, u32 fallbackVaddr = 0, bool allowBase = false) {
            if (ptr) return true;
            u32 aligned = (reqSize + 0xFFF) & ~0xFFF;
            u32 out = 0;

            // Strategy 1: Dedicated memory in APP region at fixed vaddr (bypasses newlib heap limit)
            if (vaddr != 0) {
                Result r = svcControlMemoryUnsafe(&out, vaddr, aligned,
                    (MemOp)(MEMOP_REGION_APP | MEMOP_ALLOC), (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
                if (R_SUCCEEDED(r) && out != 0) {
                    ptr = (u8 *)out;
                    size = aligned;
                    isSvc = true;
                    return true;
                }
            }

            // Strategy 2: Fallback APP region address (e.g. 0x072E0000 or 0x07320000)
            if (fallbackVaddr != 0 && fallbackVaddr != vaddr) {
                out = 0;
                Result r = svcControlMemoryUnsafe(&out, fallbackVaddr, aligned,
                    (MemOp)(MEMOP_REGION_APP | MEMOP_ALLOC), (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
                if (R_SUCCEEDED(r) && out != 0) {
                    ptr = (u8 *)out;
                    size = aligned;
                    isSvc = true;
                    return true;
                }
            }

            // Strategy 3: SYSTEM region at 0x074C0000 (New 3DS extended memory)
            out = 0;
            Result r = svcControlMemoryUnsafe(&out, 0x074C0000, aligned,
                (MemOp)(MEMOP_REGION_SYSTEM | MEMOP_ALLOC), (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
            if (R_SUCCEEDED(r) && out != 0) {
                ptr = (u8 *)out;
                size = aligned;
                isSvc = true;
                return true;
            }

            // Strategy 4: fallback to newlib malloc
            void *m = malloc(reqSize);
            if (m) {
                ptr = (u8 *)m;
                size = reqSize;
                isSvc = false;
                return true;
            }

            // Strategy 5: optional MEMOP_REGION_BASE fallback
            if (allowBase) {
                u32 baseVa = OCR_FindFreeVa(aligned);
                if (baseVa != 0) {
                    out = baseVa;
                    Result br = svcControlMemoryUnsafe(&out, baseVa, aligned,
                        (MemOp)(MEMOP_REGION_BASE | MEMOP_ALLOC), (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
                    if (R_SUCCEEDED(br) && out != 0) {
                        ptr = (u8 *)out;
                        size = aligned;
                        isSvc = true;
                        return true;
                    }
                }
            }

            return false;
        }

        void Free() {
            if (!ptr) return;
            if (isSvc) {
                svcControlMemoryUnsafe(nullptr, (u32)ptr, size, MEMOP_FREE, (MemPerm)0);
            } else {
                free(ptr);
            }
            ptr = nullptr;
            size = 0;
            isSvc = false;
        }
    };

    static SessionBuffer s_topBuffer;
    static SessionBuffer s_botBuffer;
    static SessionBuffer s_popupBuffer;

    static SessionBuffer s_kanjiTallBuffer;
    static int s_kanjiScrollY;
    static int s_kanjiMaxScrollY;           // actual max scroll = max(0, content_height - viewport)

    static const u32 TOP_FB_565_SIZE = TOP_WIDTH * TOP_HEIGHT * 2;
    static const u32 BOT_FB_565_SIZE = BOT_WIDTH * BOT_HEIGHT * 2;
    static const u32 KANJI_TALL_565_SIZE = POPUP_WIDTH * KANJI_TALL_HEIGHT * 2;

    #define s_top_565   ((u16 *)s_topBuffer.ptr)
    #define s_bot_565   ((u16 *)s_botBuffer.ptr)
    #define s_popup_rgb (s_popupBuffer.ptr)

    #define s_kanji_tall_565 ((u16 *)s_kanjiTallBuffer.ptr)

    static inline u16 PackRGB565(const Color &c)
    {
        return (u16)(((u16)(c.r & 0xF8) << 8) | ((u16)(c.g & 0xFC) << 3) | ((u16)(c.b & 0xF8) >> 3));
    }

    static inline void UnpackRGB565(u16 p, u8 &r, u8 &g, u8 &b)
    {
        r = (u8)((p >> 8) & 0xF8); r |= (u8)(r >> 5);
        g = (u8)((p >> 3) & 0xFC); g |= (u8)(g >> 6);
        b = (u8)((p << 3) & 0xF8); b |= (u8)(b >> 5);
    }

    static void OCR_FreePopupBuffer(void)
    {
        s_popupBuffer.Free();
    }

    static void OCR_FreeKanjiBuffer(void)
    {
        s_kanjiTallBuffer.Free();
        s_kanjiScrollY = 0;
        s_kanjiMaxScrollY = 0;
    }

    static void OCR_FreeSessionBuffers(void)
    {
        s_topBuffer.Free();
        s_botBuffer.Free();
        s_popupBuffer.Free();
        s_kanjiTallBuffer.Free();
    }

    struct AutoFreeSessionBuffer {
        SessionBuffer &sb;
        ~AutoFreeSessionBuffer() { sb.Free(); }
    };

    static __attribute__((noinline)) bool HeapCanFit(size_t bytes)
    {
        if (bytes == 0) return true;
        void * volatile p = malloc(bytes);
        if (!p) return false;
        *(volatile uint8_t *)p = 0;
        free((void *)p);
        return true;
    }

    // Parse host and port from "ip:port" or "ip" string
    static void SplitHostPort(const std::string &str, std::string &out_host, uint16_t &out_port, uint16_t default_port)
    {
        size_t colon = str.find(':');
        if (colon != std::string::npos) {
            out_host = str.substr(0, colon);
            int p = atoi(str.c_str() + colon + 1);
            out_port = (p > 0 && p <= 65535) ? (uint16_t)p : default_port;
        } else {
            out_host = str;
            out_port = default_port;
        }
    }

    static void OCR_LoadConfig(void)
    {
        File file;
        char buf[256];
        memset(buf, 0, sizeof(buf));

        if (File::Open(file, OCR_CONFIG_SD_PATH, File::READ) == File::SUCCESS) {
            u64 size = file.GetSize();
            if (size > 0 && size < sizeof(buf)) {
                file.Read(buf, (u32)size);
            }
            file.Close();
        } else {
            FILE *fp = fopen(OCR_CONFIG_SD_PATH, "r");
            if (fp) {
                fread(buf, 1, sizeof(buf) - 1, fp);
                fclose(fp);
            }
        }

        if (buf[0] != '\0') {
            std::string content(buf);
            size_t start = 0;
            while (start < content.length()) {
                size_t end = content.find_first_of("\r\n", start);
                if (end == std::string::npos) end = content.length();
                std::string line = content.substr(start, end - start);
                start = end + 1;

                if (line.compare(0, 4, "lan=") == 0) {
                    SplitHostPort(line.substr(4), s_serverLanHost, s_serverLanPort, 8888);
                } else if (line.compare(0, 4, "wan=") == 0) {
                    SplitHostPort(line.substr(4), s_serverWanHost, s_serverWanPort, 8888);
                }
            }
        }
    }

    static bool OCR_SaveConfig(const std::string &lan, const std::string &wan)
    {
        std::string cfg = "lan=" + lan + "\nwan=" + wan + "\n";
        File file;
        if (File::Open(file, OCR_CONFIG_SD_PATH, File::WRITE | File::CREATE | File::TRUNCATE) == File::SUCCESS) {
            file.Write(cfg.c_str(), cfg.length());
            file.Close();
            return true;
        }
        FILE *fp = fopen(OCR_CONFIG_SD_PATH, "w");
        if (fp) {
            fputs(cfg.c_str(), fp);
            fclose(fp);
            return true;
        }
        return false;
    }

    static bool OCR_SavePskToSd(const std::string &psk)
    {
        File file;
        if (File::Open(file, OCR_PSK_SD_PATH, File::WRITE | File::CREATE | File::TRUNCATE) == File::SUCCESS) {
            file.Write(psk.c_str(), psk.length());
            file.Write("\n", 1);
            file.Close();
            return true;
        }
        FILE *fp = fopen(OCR_PSK_SD_PATH, "w");
        if (fp) {
            fputs(psk.c_str(), fp);
            fputc('\n', fp);
            fclose(fp);
            return true;
        }
        return false;
    }

    static std::string s_ankiUser;
    static std::string s_ankiPass;
    static int s_activeSessionSock = -1;
    static sha256_stream_t *s_activeSessionTx = nullptr;

    // Forward declaration (send_encrypted_frame is defined later in this file)
    static int send_encrypted_frame(int sock, sha256_stream_t *tx, const void *buf, size_t len);

    static std::string TrimWhitespace(const std::string &s)
    {
        size_t first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = s.find_last_not_of(" \t\r\n");
        return s.substr(first, last - first + 1);
    }

    static void WipeStringBuffer(std::string &s)
    {
        if (!s.empty()) {
            crypto_wipe(&s[0], s.size());
            s.clear();
        }
    }

    static void OCR_LoadAnkiCredentials(void)
    {
        WipeStringBuffer(s_ankiPass);
        s_ankiUser.clear();

        char buf[640];
        memset(buf, 0, sizeof(buf));
        File file;
        if (File::Open(file, OCR_ANKI_SD_PATH, File::READ) == File::SUCCESS) {
            u64 size = file.GetSize();
            if (size > 0 && size < sizeof(buf)) {
                file.Read(buf, (u32)size);
            }
            file.Close();
        } else {
            FILE *fp = fopen(OCR_ANKI_SD_PATH, "r");
            if (fp) {
                fread(buf, 1, sizeof(buf) - 1, fp);
                fclose(fp);
            }
        }

        if (buf[0] != '\0') {
            std::string content(buf);
            size_t start = 0;
            while (start < content.length()) {
                size_t end = content.find_first_of("\r\n", start);
                if (end == std::string::npos) end = content.length();
                std::string line = content.substr(start, end - start);
                start = end + 1;

                if (line.compare(0, 5, "user=") == 0) {
                    s_ankiUser = TrimWhitespace(line.substr(5));
                    if (s_ankiUser.size() > OCR_ANKI_MAX_LEN) {
                        s_ankiUser.resize(OCR_ANKI_MAX_LEN);
                    }
                } else if (line.compare(0, 5, "pass=") == 0) {
                    std::string p = line.substr(5);
                    while (!p.empty() && (p.back() == '\r' || p.back() == '\n')) {
                        p.pop_back();
                    }
                    if (p.size() > OCR_ANKI_MAX_LEN) {
                        p.resize(OCR_ANKI_MAX_LEN);
                    }
                    s_ankiPass = p;
                    WipeStringBuffer(p);
                }
            }
            WipeStringBuffer(content);
        }
        crypto_wipe(buf, sizeof(buf));
        if (s_ankiUser.empty() || s_ankiPass.empty()) {
            WipeStringBuffer(s_ankiPass);
            s_ankiUser.clear();
        }
    }

    static bool OCR_SendAnkiCredentials(int sock, sha256_stream_t *tx)
    {
        if (sock < 0 || tx == nullptr) return false;
        if (!s_ankiUser.empty() && !s_ankiPass.empty()) {
            size_t uLen = std::min<size_t>(s_ankiUser.size(), OCR_ANKI_MAX_LEN);
            size_t pLen = std::min<size_t>(s_ankiPass.size(), OCR_ANKI_MAX_LEN);
            uint8_t hdr[5] = {
                CMD_SET_ANKI_CREDS,
                (uint8_t)((uLen >> 8) & 0xFF),
                (uint8_t)(uLen & 0xFF),
                (uint8_t)((pLen >> 8) & 0xFF),
                (uint8_t)(pLen & 0xFF)
            };
            if (send_encrypted_frame(sock, tx, hdr, sizeof(hdr)) < 0) {
                return false;
            }
            uint8_t payload[OCR_ANKI_MAX_LEN * 2];
            memcpy(payload, s_ankiUser.data(), uLen);
            memcpy(payload + uLen, s_ankiPass.data(), pLen);
            int res = send_encrypted_frame(sock, tx, payload, uLen + pLen);
            crypto_wipe(payload, sizeof(payload));
            return res >= 0;
        } else {
            uint8_t hdr[5] = { CMD_SET_ANKI_CREDS, 0, 0, 0, 0 };
            return send_encrypted_frame(sock, tx, hdr, sizeof(hdr)) >= 0;
        }
    }

    static bool OCR_SaveAnkiCredentials(const std::string &user, const std::string &pass)
    {
        WipeStringBuffer(s_ankiPass);
        s_ankiUser = user;
        s_ankiPass = pass;

        bool ok = false;
        if (s_ankiUser.empty() || s_ankiPass.empty()) {
            WipeStringBuffer(s_ankiPass);
            s_ankiUser.clear();
            if (File::Remove(OCR_ANKI_SD_PATH) == File::SUCCESS) {
                ok = true;
            } else {
                FILE *fp = fopen(OCR_ANKI_SD_PATH, "w");
                if (fp) {
                    fclose(fp);
                    ok = true;
                }
            }
        } else {
            std::string cfg = "user=" + s_ankiUser + "\npass=" + s_ankiPass + "\n";
            File file;
            if (File::Open(file, OCR_ANKI_SD_PATH, File::WRITE | File::CREATE | File::TRUNCATE) == File::SUCCESS) {
                file.Write(cfg.c_str(), cfg.length());
                file.Close();
                ok = true;
            } else {
                FILE *fp = fopen(OCR_ANKI_SD_PATH, "w");
                if (fp) {
                    fputs(cfg.c_str(), fp);
                    fclose(fp);
                    ok = true;
                }
            }
            WipeStringBuffer(cfg);
        }

        if (s_activeSessionSock >= 0 && s_activeSessionTx != nullptr) {
            OCR_SendAnkiCredentials(s_activeSessionSock, s_activeSessionTx);
        }
        return ok;
    }

    std::string OCR_GetAnkiCredentialsLabel(void)
    {
        if (s_ankiUser.empty() || s_ankiPass.empty()) {
            return "Server Default";
        }
        if (s_ankiUser.size() > 24) {
            return s_ankiUser.substr(0, 21) + "...";
        }
        return s_ankiUser;
    }

    bool OCR_OpenAnkiCredentialsDialog(void)
    {
        std::string statusLine = (!s_ankiUser.empty() && !s_ankiPass.empty())
            ? ("Current: " + s_ankiUser)
            : "Current: Server Default";
        std::vector<std::string> opts = {
            "Set AnkiWeb Login",
            "Clear (Use Server Default)",
            "Cancel"
        };
        Keyboard menuKbd("AnkiWeb Account Settings\n" + statusLine, opts);
        menuKbd.CanAbort(true);
        int choice = menuKbd.Open();
        if (choice == 0) {
            std::string newUser = s_ankiUser;
            Keyboard userKbd("Enter AnkiWeb Email / Username:");
            userKbd.CanAbort(true);
            userKbd.SetMaxLength(OCR_ANKI_MAX_LEN);
            if (userKbd.Open(newUser, s_ankiUser) < 0) {
                return false;
            }
            newUser = TrimWhitespace(newUser);
            if (newUser.empty()) {
                OSD::Notify("AnkiWeb username cannot be empty!", Color::Red);
                return false;
            }

            std::string newPass;
            Keyboard passKbd("Enter AnkiWeb Password for:\n" + newUser);
            passKbd.CanAbort(true);
            passKbd.SetMaxLength(OCR_ANKI_MAX_LEN);
            if (passKbd.Open(newPass, "") < 0) {
                WipeStringBuffer(newPass);
                return false;
            }
            if (newPass.empty()) {
                WipeStringBuffer(newPass);
                OSD::Notify("AnkiWeb password cannot be empty!", Color::Red);
                return false;
            }

            bool live = (s_activeSessionSock >= 0 && s_activeSessionTx != nullptr);
            OCR_SaveAnkiCredentials(newUser, newPass);
            WipeStringBuffer(newPass);
            OSD::Notify(live ? "AnkiWeb login updated for session!" : "AnkiWeb login saved!", Color::Green);
            return true;
        } else if (choice == 1) {
            bool live = (s_activeSessionSock >= 0 && s_activeSessionTx != nullptr);
            OCR_SaveAnkiCredentials("", "");
            OSD::Notify(live ? "Session reset to server default Anki!" : "AnkiWeb login cleared (server default)", Color::Yellow);
            return true;
        }
        return false;
    }

    OcrKeyBindings g_ocrKeys = {
        Key::R | Key::Start | Key::Select, // activation
        Key::Start | Key::Select | Key::L, // slimMenu
        Key::A,                            // lookup
        Key::Select,                       // modeToggle
        Key::Y,                            // switchScreen
        Key::X,                            // addAnki
        Key::L,                            // prevEntry
        Key::R,                            // nextEntry
        Key::B,                            // exitSession
        Key::ZL,                           // prevOcr
        Key::ZR                            // nextOcr
    };

    static u32 ParseKeyHex(const std::string &str, u32 defaultVal)
    {
        if (str.empty()) return defaultVal;
        char *end = nullptr;
        unsigned long val = strtoul(str.c_str(), &end, 0);
        if (end == str.c_str()) return defaultVal;
        return (u32)val;
    }

    void OCR_InitKeyBindings(void)
    {
        char buf[512];
        memset(buf, 0, sizeof(buf));
        File file;
        bool ok = false;
        if (File::Open(file, OCR_KEYS_SD_PATH, File::READ) == File::SUCCESS) {
            u64 size = file.GetSize();
            if (size > 0 && size < sizeof(buf)) {
                if (file.Read(buf, (u32)size) == File::SUCCESS) {
                    ok = true;
                }
            }
            file.Close();
        } else {
            FILE *fp = fopen(OCR_KEYS_SD_PATH, "r");
            if (fp) {
                fread(buf, 1, sizeof(buf) - 1, fp);
                fclose(fp);
                ok = true;
            }
        }

        if (ok) {
            std::string content(buf);
            size_t start = 0;
            while (start < content.length()) {
                size_t end = content.find_first_of("\r\n", start);
                if (end == std::string::npos) end = content.length();
                std::string line = content.substr(start, end - start);
                start = end + 1;

                auto parseVal = [&](const std::string &prefix, u32 &target) {
                    if (line.compare(0, prefix.length(), prefix) == 0) {
                        target = ParseKeyHex(line.substr(prefix.length()), target);
                    }
                };
                parseVal("activation=", g_ocrKeys.activation);
                parseVal("slimMenu=", g_ocrKeys.slimMenu);
                parseVal("lookup=", g_ocrKeys.lookup);
                parseVal("modeToggle=", g_ocrKeys.modeToggle);
                parseVal("switchScreen=", g_ocrKeys.switchScreen);
                parseVal("addAnki=", g_ocrKeys.addAnki);
                parseVal("prevEntry=", g_ocrKeys.prevEntry);
                parseVal("nextEntry=", g_ocrKeys.nextEntry);
                parseVal("exitSession=", g_ocrKeys.exitSession);
                parseVal("prevOcr=", g_ocrKeys.prevOcr);
                parseVal("nextOcr=", g_ocrKeys.nextOcr);
            }
        }
    }

    void OCR_SaveKeyBindings(void)
    {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "activation=0x%08lX\n"
            "slimMenu=0x%08lX\n"
            "lookup=0x%08lX\n"
            "modeToggle=0x%08lX\n"
            "switchScreen=0x%08lX\n"
            "addAnki=0x%08lX\n"
            "prevEntry=0x%08lX\n"
            "nextEntry=0x%08lX\n"
            "exitSession=0x%08lX\n"
            "prevOcr=0x%08lX\n"
            "nextOcr=0x%08lX\n",
            (unsigned long)g_ocrKeys.activation,
            (unsigned long)g_ocrKeys.slimMenu,
            (unsigned long)g_ocrKeys.lookup,
            (unsigned long)g_ocrKeys.modeToggle,
            (unsigned long)g_ocrKeys.switchScreen,
            (unsigned long)g_ocrKeys.addAnki,
            (unsigned long)g_ocrKeys.prevEntry,
            (unsigned long)g_ocrKeys.nextEntry,
            (unsigned long)g_ocrKeys.exitSession,
            (unsigned long)g_ocrKeys.prevOcr,
            (unsigned long)g_ocrKeys.nextOcr);

        File file;
        if (File::Open(file, OCR_KEYS_SD_PATH, File::WRITE | File::CREATE | File::TRUNCATE) == File::SUCCESS) {
            file.Write(buf, strlen(buf));
            file.Close();
            return;
        }
        FILE *fp = fopen(OCR_KEYS_SD_PATH, "w");
        if (fp) {
            fputs(buf, fp);
            fclose(fp);
        }
    }

    void OCR_ResetKeyBindings(void)
    {
        g_ocrKeys.activation   = Key::R | Key::Start | Key::Select;
        g_ocrKeys.slimMenu     = Key::Start | Key::Select | Key::L;
        g_ocrKeys.lookup       = Key::A;
        g_ocrKeys.modeToggle   = Key::Select;
        g_ocrKeys.switchScreen = Key::Y;
        g_ocrKeys.addAnki      = Key::X;
        g_ocrKeys.prevEntry    = Key::L;
        g_ocrKeys.nextEntry    = Key::R;
        g_ocrKeys.exitSession  = Key::B;
        g_ocrKeys.prevOcr      = Key::ZL;
        g_ocrKeys.nextOcr      = Key::ZR;
        OCR_SaveKeyBindings();
    }

    std::string OCR_KeyToString(u32 keys)
    {
        if (keys == 0) return "None";
        std::string res;
        auto appendKey = [&](u32 k, const char *name) {
            if (keys & k) {
                if (!res.empty()) res += " + ";
                res += name;
            }
        };
        appendKey(Key::A, "A");
        appendKey(Key::B, "B");
        appendKey(Key::X, "X");
        appendKey(Key::Y, "Y");
        appendKey(Key::L, "L");
        appendKey(Key::R, "R");
        appendKey(Key::ZL, "ZL");
        appendKey(Key::ZR, "ZR");
        appendKey(Key::Start, "Start");
        appendKey(Key::Select, "Select");
        appendKey(Key::DPadUp, "D-Up");
        appendKey(Key::DPadDown, "D-Down");
        appendKey(Key::DPadLeft, "D-Left");
        appendKey(Key::DPadRight, "D-Right");
        return res.empty() ? "None" : res;
    }

    uint8_t OCR_KeyToCharCode(u32 k)
    {
        if (k & Key::Select) return 'S';
        if (k & Key::Start) return 'T';
        if (k & Key::A) return 'A';
        if (k & Key::B) return 'B';
        if (k & Key::X) return 'X';
        if (k & Key::Y) return 'Y';
        if (k & Key::L) return 'L';
        if (k & Key::R) return 'R';
        if (k & Key::ZL) return '1';
        if (k & Key::ZR) return '2';
        return 'A';
    }

    std::string OCR_GetInstructionsText(void)
    {
        std::string txt = "How to Use OCR\n\n";
        txt += "In Game:\n";
        txt += "Press [" + OCR_KeyToString(g_ocrKeys.activation) + "] anytime during gameplay to capture text.\n\n";
        txt += "Controls in OCR Mode:\n";
        txt += "- Circle Pad / C-Stick: Move cursor\n";
        txt += "- Touch Screen: Jump cursor to touched position\n";
        txt += "- [" + OCR_KeyToString(g_ocrKeys.lookup) + "]: Single word lookup / Region point\n";
        txt += "- [" + OCR_KeyToString(g_ocrKeys.modeToggle) + "]: Toggle Mode (Single Word / Region Box)\n";
        txt += "- [" + OCR_KeyToString(g_ocrKeys.switchScreen) + "]: Switch Screen (Top <-> Bottom)\n";
        txt += "- [" + OCR_KeyToString(g_ocrKeys.addAnki) + "]: Add current entry to Anki\n";
        txt += "- [" + OCR_KeyToString(g_ocrKeys.prevEntry) + " / " + OCR_KeyToString(g_ocrKeys.nextEntry) + "]: Cycle dictionary entries\n";
        txt += "- [" + OCR_KeyToString(g_ocrKeys.prevOcr) + " / " + OCR_KeyToString(g_ocrKeys.nextOcr) + "]: Cycle differing OCR outputs\n";
        txt += "- [D-Pad]: Scroll definition & grammar notes\n";
        txt += "- [Start]: Scan Server QR Code (PSK & IPs)\n";
        txt += "- [" + OCR_KeyToString(g_ocrKeys.exitSession) + "]: Dismiss popup / Resume game\n";
        return txt;
    }

    static bool s_ndmuInitialized = false;
    static bool s_acInitialized = false;
    static bool s_networkLocked = false;

    // Acquire and lock NDMU infrastructure state and AC association once the game is running
    static void OCR_LockNetwork(void)
    {
        if (s_networkLocked) return;
        s_networkLocked = true;

        // Resume network scheduler and lock infrastructure state (prevents games like Ultra Moon from killing Wi-Fi)
        if (R_SUCCEEDED(ndmuInit())) {
            NDMU_ResumeScheduler();
            NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
            NDMU_LockState();
            s_ndmuInitialized = true;
        }

        // Keep AC initialized to maintain active Wi-Fi association
        if (R_SUCCEEDED(acInit())) {
            s_acInitialized = true;
        }
    }

    static bool s_pluginOwnIrrst = false;

    static void OCR_InitCStick(void)
    {
        if (System::IsNew3DS()) {
            if (irrstSharedMem == nullptr && !s_pluginOwnIrrst) {
                Result res = irrstInit();
                if (R_SUCCEEDED(res)) {
                    s_pluginOwnIrrst = true;
                }
            }
        }
    }

    static void OCR_ExitCStick(void)
    {
        if (s_pluginOwnIrrst) {
            s_pluginOwnIrrst = false;
            irrstExit();
        }
    }

    static shortVector GetCStickPosition(void)
    {
        circlePosition cPos = {0, 0};
        irrstCstickRead(&cPos);
        if (cPos.dx == 0 && cPos.dy == 0 && irrstSharedMem != nullptr) {
            vu32 *imem = irrstSharedMem;
            u32 idx = imem[4];
            if (idx > 7) idx = 7;
            if (idx == 0) {
                u64 tickOld = reinterpret_cast<vu64*>(imem)[1];
                u64 tickNew = reinterpret_cast<vu64*>(imem)[0];
                if (tickNew == tickOld || static_cast<s64>(tickNew) < 0) {
                    return shortVector(0, 0);
                }
            }
            u32 raw = imem[9 + idx * 4];
            cPos.dx = (s16)(raw & 0xFFFF);
            cPos.dy = (s16)(raw >> 16);
        }
        return shortVector(cPos.dx, cPos.dy);
    }

    void OCR_Initialize(void)
    {
        static bool s_initialized = false;
        if (s_initialized) return;
        s_initialized = true;

        // Defer NDMU/AC locking until the game finishes nnMain boot and begins rendering frames
        OCR_LoadConfig();
        OCR_LoadAnkiCredentials();
        OCR_InitKeyBindings();
    }

    static bool OCR_CloseSocLocked(bool allowBorrowed)
    {
        if (!s_socInitialized)
            return false;

        if (s_socBorrowed) {
            if (!allowBorrowed)
                return false;
            if (FindDevice("soc:") >= 0) {
                RemoveDevice("soc:");
            }
            SOCU_handle = 0;
            s_socBorrowed = false;
            s_socInitialized = false;
            return true;
        }

        socExit();
        if (s_socAliasAddr) {
            svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, s_socAliasAddr, s_socBufferSize);
            s_socAliasAddr = 0;
        }
        if (s_socBufferAddr) {
            if (s_socBufferIsHeap) {
                free((void *)s_socBufferAddr);
            } else {
                svcControlMemoryUnsafe(nullptr, s_socBufferAddr, s_socBufferSize, MEMOP_FREE, (MemPerm)0);
            }
            s_socBufferAddr = 0;
            s_socBufferSize = 0;
            s_socBufferIsHeap = false;
        }
        s_socInitialized = false;
        return true;
    }

    void OCR_Cleanup(void)
    {
        OCR_CloseSocLocked(true);
        if (s_ndmuInitialized) {
            NDMU_UnlockState();
            NDMU_LeaveExclusiveState();
            ndmuExit();
            s_ndmuInitialized = false;
        }
        if (s_acInitialized) {
            acExit();
            s_acInitialized = false;
        }
        s_networkLocked = false;
        WipeStringBuffer(s_ankiPass);
        s_ankiUser.clear();
        OCR_FreeSessionBuffers();
        OCR_ExitCStick();
    }

    static size_t OCR_LoadPskFromSd(char *out_psk, size_t max_len)
    {
        File file;
        if (File::Open(file, OCR_PSK_SD_PATH, File::READ) != File::SUCCESS) {
            FILE *fp = fopen(OCR_PSK_SD_PATH, "r");
            if (!fp) return 0;
            size_t n = fread(out_psk, 1, max_len - 1, fp);
            fclose(fp);
            if (n == 0) return 0;
            out_psk[n] = '\0';
        } else {
            u64 fileSize = file.GetSize();
            if (fileSize == 0 || fileSize >= max_len) {
                file.Close();
                return 0;
            }
            if (file.Read(out_psk, (u32)fileSize) != File::SUCCESS) {
                file.Close();
                return 0;
            }
            out_psk[fileSize] = '\0';
            file.Close();
        }

        // Strip trailing whitespace, CR, LF
        size_t len = strlen(out_psk);
        while (len > 0 && (out_psk[len - 1] == '\r' || out_psk[len - 1] == '\n' ||
                           out_psk[len - 1] == ' '  || out_psk[len - 1] == '\t')) {
            out_psk[--len] = '\0';
        }
        return len;
    }

    static int send_all(int sock, const void *buf, size_t len)
    {
        const uint8_t *p = (const uint8_t *)buf;
        size_t total = 0;
        while (total < len) {
            if (OCR_ShouldAbortForSleep()) return -1;
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 50);
            if (pr == 0) continue;
            if (pr < 0) return -1;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return -1;
            size_t chunk = len - total;
            if (chunk > 0x4000) chunk = 0x4000;
            ssize_t n = send(sock, p + total, chunk, 0);
            if (n <= 0) return -1;
            total += (size_t)n;
        }
        return 0;
    }

    static int recv_all(int sock, void *buf, size_t len)
    {
        uint8_t *p = (uint8_t *)buf;
        size_t total = 0;
        while (total < len) {
            if (OCR_ShouldAbortForSleep()) return -1;
            struct pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
            pfd.revents = 0;
            int pr = poll(&pfd, 1, 50);
            if (pr == 0) continue;
            if (pr < 0) return -1;
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(pfd.revents & POLLIN)) return -1;
            size_t chunk = len - total;
            if (chunk > 0x4000) chunk = 0x4000;
            ssize_t n = recv(sock, p + total, chunk, 0);
            if (n <= 0) return -1;
            total += (size_t)n;
        }
        return 0;
    }

    static int send_encrypted_frame(int sock, sha256_stream_t *tx, const void *plaintext, size_t len)
    {
        const uint8_t *src = (const uint8_t *)plaintext;
        uint8_t chunk[512];
        uint8_t aad[9];
        uint8_t full_mac[32];
        hmac_sha256_ctx_t hctx;

        aad[0] = tx->dir;
        store_be64(&aad[1], tx->seq++);

        hmac_sha256_init(&hctx, tx->mac_key, 32);
        hmac_sha256_update(&hctx, aad, sizeof(aad));

        size_t offset = 0;
        while (offset < len) {
            size_t n = (len - offset > sizeof(chunk)) ? sizeof(chunk) : (len - offset);
            memcpy(chunk, src + offset, n);
            sha256_stream_xor(tx, chunk, n);
            hmac_sha256_update(&hctx, chunk, n);
            if (send_all(sock, chunk, n) < 0) {
                crypto_wipe(chunk, sizeof(chunk));
                crypto_wipe(&hctx, sizeof(hctx));
                return -1;
            }
            offset += n;
        }

        hmac_sha256_final(&hctx, full_mac);
        int res = send_all(sock, full_mac, STREAM_MAC_SIZE);
        crypto_wipe(chunk, sizeof(chunk));
        crypto_wipe(full_mac, sizeof(full_mac));
        return res;
    }

    static int send_encrypted_rgb565_as_rgb24(int sock, sha256_stream_t *tx, const u16 *rgb565, size_t numPixels)
    {
        uint8_t chunk[170 * 3]; // 510 bytes per chunk (170 RGB24 pixels)
        uint8_t aad[9];
        uint8_t full_mac[32];
        hmac_sha256_ctx_t hctx;

        aad[0] = tx->dir;
        store_be64(&aad[1], tx->seq++);

        hmac_sha256_init(&hctx, tx->mac_key, 32);
        hmac_sha256_update(&hctx, aad, sizeof(aad));

        size_t offset = 0;
        while (offset < numPixels) {
            size_t count = (numPixels - offset > 170) ? 170 : (numPixels - offset);
            for (size_t i = 0; i < count; ++i) {
                u8 r, g, b;
                UnpackRGB565(rgb565[offset + i], r, g, b);
                chunk[i * 3 + 0] = r;
                chunk[i * 3 + 1] = g;
                chunk[i * 3 + 2] = b;
            }
            size_t n = count * 3;
            sha256_stream_xor(tx, chunk, n);
            hmac_sha256_update(&hctx, chunk, n);
            if (send_all(sock, chunk, n) < 0) {
                crypto_wipe(chunk, sizeof(chunk));
                crypto_wipe(&hctx, sizeof(hctx));
                return -1;
            }
            offset += count;
        }

        hmac_sha256_final(&hctx, full_mac);
        int res = send_all(sock, full_mac, STREAM_MAC_SIZE);
        crypto_wipe(chunk, sizeof(chunk));
        crypto_wipe(full_mac, sizeof(full_mac));
        return res;
    }

    static int recv_encrypted_frame(int sock, sha256_stream_t *rx, uint8_t *buf, size_t len)
    {
        uint8_t recv_mac[STREAM_MAC_SIZE];
        uint8_t calc_mac[32];
        uint8_t aad[9];
        hmac_sha256_ctx_t hctx;

        if (recv_all(sock, buf, len) < 0) return -1;
        if (recv_all(sock, recv_mac, STREAM_MAC_SIZE) < 0) return -1;

        aad[0] = rx->dir;
        store_be64(&aad[1], rx->seq++);

        hmac_sha256_init(&hctx, rx->mac_key, 32);
        hmac_sha256_update(&hctx, aad, sizeof(aad));
        hmac_sha256_update(&hctx, buf, len);
        hmac_sha256_final(&hctx, calc_mac);

        if (crypto_verify_16(recv_mac, calc_mac) != 0) {
            crypto_wipe(buf, len);
            crypto_wipe(calc_mac, sizeof(calc_mac));
            return -1;
        }

        sha256_stream_xor(rx, buf, len);
        crypto_wipe(calc_mac, sizeof(calc_mac));
        return 0;
    }

    static bool DrainEncryptedPayload(int sock, sha256_stream_t *rx, uint32_t payloadSize)
    {
        uint8_t recv_mac[STREAM_MAC_SIZE];
        uint8_t calc_mac[32];
        uint8_t aad[9];
        uint8_t drainBuf[4096];
        hmac_sha256_ctx_t hctx;

        aad[0] = rx->dir;
        store_be64(&aad[1], rx->seq++);

        hmac_sha256_init(&hctx, rx->mac_key, 32);
        hmac_sha256_update(&hctx, aad, sizeof(aad));

        uint32_t rem = payloadSize;
        while (rem > 0) {
            uint32_t chunk = (rem < (uint32_t)sizeof(drainBuf)) ? rem : (uint32_t)sizeof(drainBuf);
            if (recv_all(sock, drainBuf, chunk) < 0) return false;
            hmac_sha256_update(&hctx, drainBuf, chunk);
            sha256_stream_xor(rx, drainBuf, chunk);
            rem -= chunk;
        }
        if (recv_all(sock, recv_mac, STREAM_MAC_SIZE) < 0) return false;
        hmac_sha256_final(&hctx, calc_mac);

        bool ok = (crypto_verify_16(recv_mac, calc_mac) == 0);
        crypto_wipe(calc_mac, sizeof(calc_mac));
        return ok;
    }

    static bool recv_encrypted_kanji_as_rgb565(int sock, sha256_stream_t *rx, u16 *out565, size_t numPixels, u16 &outContentH)
    {
        uint8_t recv_mac[STREAM_MAC_SIZE];
        uint8_t calc_mac[32];
        uint8_t aad[9];
        uint8_t chunk[170 * 3]; // 510 bytes per chunk (170 RGB24 pixels)
        uint8_t tail[2];
        hmac_sha256_ctx_t hctx;

        aad[0] = rx->dir;
        store_be64(&aad[1], rx->seq++);

        hmac_sha256_init(&hctx, rx->mac_key, 32);
        hmac_sha256_update(&hctx, aad, sizeof(aad));

        size_t offset = 0;
        while (offset < numPixels) {
            size_t count = (numPixels - offset > 170) ? 170 : (numPixels - offset);
            size_t n = count * 3;
            if (recv_all(sock, chunk, n) < 0) {
                crypto_wipe(chunk, sizeof(chunk));
                crypto_wipe(&hctx, sizeof(hctx));
                return false;
            }
            hmac_sha256_update(&hctx, chunk, n);
            sha256_stream_xor(rx, chunk, n);
            for (size_t i = 0; i < count; ++i) {
                out565[offset + i] = PackRGB565(Color(chunk[i * 3 + 0], chunk[i * 3 + 1], chunk[i * 3 + 2]));
            }
            offset += count;
        }

        if (recv_all(sock, tail, 2) < 0 || recv_all(sock, recv_mac, STREAM_MAC_SIZE) < 0) {
            crypto_wipe(chunk, sizeof(chunk));
            crypto_wipe(&hctx, sizeof(hctx));
            return false;
        }
        hmac_sha256_update(&hctx, tail, 2);
        sha256_stream_xor(rx, tail, 2);
        outContentH = ((u16)tail[0] << 8) | (u16)tail[1];

        hmac_sha256_final(&hctx, calc_mac);
        bool ok = (crypto_verify_16(recv_mac, calc_mac) == 0);
        crypto_wipe(chunk, sizeof(chunk));
        crypto_wipe(calc_mac, sizeof(calc_mac));
        return ok;
    }

    static bool OCR_GenerateEphemeralSecret(uint8_t out_secret[32])
    {
        static uint32_t session_counter = 0;
        struct {
            uint8_t   hw_rng[32];
            uint64_t  sys_tick;
            uint64_t  os_time;
            uint32_t  counter;
            uintptr_t stack_addr;
        } entropy;

        memset(&entropy, 0, sizeof(entropy));
        if (R_SUCCEEDED(psInit())) {
            PS_GenerateRandomBytes(entropy.hw_rng, sizeof(entropy.hw_rng));
            psExit();
        } else {
            // Software entropy fallback when ps:ps is restricted to system modules
            for (size_t i = 0; i < sizeof(entropy.hw_rng); ++i) {
                entropy.hw_rng[i] = (uint8_t)(svcGetSystemTick() ^ (rand() & 0xFF));
            }
        }

        entropy.sys_tick = svcGetSystemTick();
        entropy.os_time = osGetTime();
        entropy.counter = ++session_counter;
        entropy.stack_addr = (uintptr_t)&entropy;

        sha256_compute((const uint8_t *)&entropy, sizeof(entropy), out_secret);
        crypto_wipe(&entropy, sizeof(entropy));
        return true;
    }

    static int OCR_ConnectHost(const char *ip_str, uint16_t port, int timeout_ms, int &out_err)
    {
        out_err = 0;
        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(port);
        serverAddr.sin_addr.s_addr = inet_addr(ip_str);
        if (serverAddr.sin_addr.s_addr == INADDR_NONE) {
            out_err = EINVAL;
            return -1;
        }

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            out_err = errno;
            return -1;
        }

        // Set non-blocking mode for fast connection timeout
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }

        int res = connect(sock, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
        if (res < 0) {
            if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN) {
                int remaining_ms = timeout_ms;
                int pollRes = 0;
                struct pollfd pfd;
                pfd.fd = sock;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                while (remaining_ms > 0) {
                    if (OCR_ShouldAbortForSleep()) {
                        out_err = ECANCELED;
                        s_activeSessionSock = -1;
                        s_activeSessionTx = nullptr;
                        closesocket(sock);
                        return -1;
                    }
                    int slice = (remaining_ms > 50) ? 50 : remaining_ms;
                    pfd.revents = 0;
                    pollRes = poll(&pfd, 1, slice);
                    if (pollRes != 0) break;
                    remaining_ms -= slice;
                }
                if (pollRes > 0) {
                    if (pfd.revents & (POLLERR | POLLHUP)) {
                        int sockErr = 0;
                        socklen_t len = sizeof(sockErr);
                        getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockErr, &len);
                        out_err = (sockErr != 0) ? (sockErr < 0 ? -sockErr : sockErr) : ECONNREFUSED;
                        s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        closesocket(sock);
                        return -1;
                    }
                    if (pfd.revents & POLLOUT) {
                        int sockErr = 0;
                        socklen_t len = sizeof(sockErr);
                        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sockErr, &len) == 0) {
                            // On Nintendo 3DS, sockErr retains raw -26 (SOC EINPROGRESS) upon connect completion
                            if (sockErr != 0 && sockErr != -26 && sockErr != EINPROGRESS) {
                                out_err = (sockErr < 0) ? -sockErr : sockErr;
                                s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        closesocket(sock);
                                return -1;
                            }
                        }
                        // Connection established!
                    } else {
                        out_err = ETIMEDOUT;
                        s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        closesocket(sock);
                        return -1;
                    }
                } else if (pollRes == 0) {
                    out_err = ETIMEDOUT;
                    s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        closesocket(sock);
                    return -1;
                } else {
                    out_err = errno;
                    s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        closesocket(sock);
                    return -1;
                }
            } else {
                out_err = errno;
                s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        closesocket(sock);
                return -1;
            }
        }

        // Restore blocking mode for payload transfer
        if (flags >= 0) {
            fcntl(sock, F_SETFL, flags);
        }

        return sock;
    }

    // Ensure soc:U is initialized with an active IP
    static bool OCR_EnsureNetwork(std::string &diag)
    {
        // 1. Ensure config loaded and NDMU infrastructure lock is active
        OCR_Initialize();
        OCR_LockNetwork();

        // 2. If the game already suspended network scheduling, resume it
        if (!s_ndmuInitialized) {
            if (R_SUCCEEDED(ndmuInit())) {
                s_ndmuInitialized = true;
            }
        }
        if (s_ndmuInitialized) {
            NDMU_ResumeScheduler();
            NDMU_EnterExclusiveState(NDM_EXCLUSIVE_STATE_INFRASTRUCTURE);
            NDMU_LockState();
        }

        // Force active Wi-Fi connection if disconnected (bypasses Pokemon / offline game Wi-Fi blocks)
        if (!s_acInitialized) {
            if (R_SUCCEEDED(acInit())) {
                s_acInitialized = true;
            }
        }
        if (s_acInitialized) {
            u32 wifiStatus = 0;
            if (R_FAILED(ACU_GetStatus(&wifiStatus)) || wifiStatus != 3) {
                acuConfig acuConf;
                memset(&acuConf, 0, sizeof(acuConf));
                if (R_SUCCEEDED(ACU_CreateDefaultConfig(&acuConf))) {
                    ACU_SetNetworkArea(&acuConf, 2);
                    ACU_SetAllowApType(&acuConf, (u8)0xFF);
                    ACU_SetRequestEulaVersion(&acuConf);

                    Handle connEvent = 0;
                    if (R_SUCCEEDED(svcCreateEvent(&connEvent, RESET_ONESHOT))) {
                        if (R_SUCCEEDED(ACU_ConnectAsync(&acuConf, connEvent))) {
                            // Wait up to 6 seconds for Wi-Fi association in 50ms slices so sleep aborts immediately
                            for (int i = 0; i < 120; ++i) {
                                if (OCR_ShouldAbortForSleep()) break;
                                if (R_SUCCEEDED(svcWaitSynchronization(connEvent, 50000000ULL))) break;
                            }
                        }
                        svcCloseHandle(connEvent);
                    }
                }
            }
            // Retain open session handle to ac:u so Wi-Fi does not get torn down
        }

        // 3. If soc was initialized but interface dropped, reinitialize
        if (s_socInitialized && !s_socBorrowed && (u32)gethostid() == 0) {
            OCR_CloseSocLocked(false);
        }

        if (!s_socInitialized) {
            u32 outAddr = 0;
            Result memRes = 0;
            Result socRes = 0;
            Result mapRes = 0;
            Result baseRes = 0;
            HostSocSearchDiag socDiag;
            int addDevRes = -99;

            auto tryBorrowHostSoc = [&]() {
                if (s_socInitialized) return;
                Handle hostSoc = OCR_FindHostSocuHandle(&socDiag);
                if (hostSoc != 0) {
                    SOCU_handle = hostSoc;
                    int existingDev = FindDevice("soc:");
                    addDevRes = (existingDev >= 0) ? existingDev : AddDevice(&s_borrowedSocDevoptab);
                    if (addDevRes >= 0) {
                        s_socBorrowed = true;
                        s_socInitialized = true;
                    } else {
                        SOCU_handle = 0;
                    }
                }
            };

            auto trySocControlMem = [&](u32 addr, MemOp region, u32 bufSize) {
                if (addr == 0 || s_socInitialized || socRes == static_cast<Result>(0xC8A0700B))
                    return;
                outAddr = addr;
                memRes = svcControlMemoryUnsafe(&outAddr, addr, bufSize,
                    (MemOp)(region | MEMOP_ALLOC), (MemPerm)(MEMPERM_READ | MEMPERM_WRITE));
                if (R_SUCCEEDED(memRes)) {
                    socRes = socInit((u32 *)addr, bufSize);
                    if (R_SUCCEEDED(socRes)) {
                        s_socBufferAddr = addr;
                        s_socAliasAddr = 0;
                        s_socBufferSize = bufSize;
                        s_socBufferIsHeap = false;
                        s_socBorrowed = false;
                        s_socInitialized = true;
                    } else {
                        svcControlMemoryUnsafe(nullptr, addr, bufSize, MEMOP_FREE, (MemPerm)0);
                        outAddr = 0;
                        if (socRes == static_cast<Result>(0xC8A0700B))
                            tryBorrowHostSoc();
                    }
                }
            };

            // Strategies 1-3: Outside plugin heap with standard 288 KB buffer
            trySocControlMem(0x07200000, MEMOP_REGION_APP, SOC_BUFFER_SIZE);
            trySocControlMem(0x074C0000, MEMOP_REGION_SYSTEM, SOC_BUFFER_SIZE);
            trySocControlMem(0x074C0000, MEMOP_REGION_APP, SOC_BUFFER_SIZE);

            // Smaller APP/SYSTEM buffer fallbacks (using free VA scan) for Mode 3 games
            if (!s_socInitialized && socRes != static_cast<Result>(0xC8A0700B)) {
                trySocControlMem(OCR_FindFreeVa(0x20000), MEMOP_REGION_APP, 0x20000);
                trySocControlMem(OCR_FindFreeVa(0x20000), MEMOP_REGION_SYSTEM, 0x20000);
                trySocControlMem(OCR_FindFreeVa(0x10000), MEMOP_REGION_APP, 0x10000);
                trySocControlMem(OCR_FindFreeVa(0x10000), MEMOP_REGION_SYSTEM, 0x10000);
            }

            // Strategy 4: CTRPF plugin heap fallback via memalign + MAPEXFLAGS_PRIVATE alias
            // (Plugin heap is MEMSTATE_SHARED, which svcCreateMemoryBlock rejects with 0xE0A01BF5;
            // aliasing as MEMSTATE_PRIVATE via svcMapProcessMemoryEx satisfies socInit.)
            if (!s_socInitialized && socRes != static_cast<Result>(0xC8A0700B)) {
                const u32 heapCandidates[3] = { SOC_BUFFER_SIZE, 0x20000, 0x10000 };
                for (int ci = 0; ci < 3 && !s_socInitialized && socRes != static_cast<Result>(0xC8A0700B); ++ci) {
                    u32 sz = heapCandidates[ci];
                    if (ci == 0 && !HeapCanFit(sz + 0x28000)) continue;
                    if (ci == 1 && !HeapCanFit(sz + 0x18000)) continue;
                    void *heapSocBuf = memalign(0x1000, sz);
                    if (heapSocBuf) {
                        u32 aliasVa = OCR_FindFreeVa(sz);
                        if (aliasVa != 0) {
                            mapRes = svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, aliasVa, CUR_PROCESS_HANDLE, (u32)heapSocBuf, sz, MAPEXFLAGS_PRIVATE);
                        }
                        if (aliasVa != 0 && R_SUCCEEDED(mapRes)) {
                            socRes = socInit((u32 *)aliasVa, sz);
                            if (R_SUCCEEDED(socRes)) {
                                s_socBufferAddr = (u32)heapSocBuf;
                                s_socAliasAddr = aliasVa;
                                s_socBufferSize = sz;
                                s_socBufferIsHeap = true;
                                s_socBorrowed = false;
                                s_socInitialized = true;
                            } else {
                                svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, aliasVa, sz);
                                free(heapSocBuf);
                                if (socRes == static_cast<Result>(0xC8A0700B))
                                    tryBorrowHostSoc();
                            }
                        } else {
                            socRes = socInit((u32 *)heapSocBuf, sz);
                            if (R_SUCCEEDED(socRes)) {
                                s_socBufferAddr = (u32)heapSocBuf;
                                s_socAliasAddr = 0;
                                s_socBufferSize = sz;
                                s_socBufferIsHeap = true;
                                s_socBorrowed = false;
                                s_socInitialized = true;
                            } else {
                                free(heapSocBuf);
                                if (socRes == static_cast<Result>(0xC8A0700B))
                                    tryBorrowHostSoc();
                            }
                        }
                    }
                }
            }

            // Strategy 5: MEMOP_REGION_BASE fallback
            if (!s_socInitialized && socRes != static_cast<Result>(0xC8A0700B)) {
                const u32 baseCandidates[3] = { SOC_BUFFER_SIZE, 0x20000, 0x10000 };
                for (int bi = 0; bi < 3 && !s_socInitialized && socRes != static_cast<Result>(0xC8A0700B); ++bi) {
                    u32 sz = baseCandidates[bi];
                    trySocControlMem(OCR_FindFreeVa(sz), MEMOP_REGION_BASE, sz);
                    baseRes = memRes;
                }
            }

            // Strategy 6: Last-resort fallback: borrow host game's soc:u handle if it already owns soc:u
            if (!s_socInitialized) {
                tryBorrowHostSoc();
            }

            if (!s_socInitialized) {
                char buf[160];
                snprintf(buf, sizeof(buf),
                    "socInit failed: 0x%08lX (mem: 0x%08lX)\nmap=0x%08lX base=0x%08lX\nhost=0x%lX h=%lu r=%d dev=%d\n",
                    (unsigned long)socRes, (unsigned long)memRes,
                    (unsigned long)mapRes, (unsigned long)baseRes,
                    (unsigned long)socDiag.handle, (unsigned long)socDiag.hits00010044, socDiag.roHits, addDevRes);
                diag += buf;
                return false;
            }
        }

        // Wait up to 3 seconds for DHCP IP to become available in socket stack
        u32 finalIp = 0;
        for (int retry = 0; retry < 30; ++retry) {
            finalIp = (u32)gethostid();
            if (finalIp != 0 && finalIp != 0xFFFFFFFF && (finalIp & 0xFF) != 0) break;
            svcSleepThread(100000000ULL); // 100ms
        }

        if (finalIp == 0 || finalIp == 0xFFFFFFFF || (finalIp & 0xFF) == 0) {
            diag += "Warning: 3DS WiFi not connected (0.0.0.0)\n";
        }

        return true;
    }

    static int OCR_ConnectServer(std::string *out_diag = nullptr)
    {
        std::string diag = "";
        s_lastConnectedEndpoint = "";

        if (!OCR_EnsureNetwork(diag)) {
            if (out_diag) *out_diag = diag;
            return -1;
        }

        u32 hostIp = (u32)gethostid();
        struct in_addr in;
        in.s_addr = hostIp;
        std::string myIpStr = inet_ntoa(in);

        diag += "3DS IP: " + myIpStr + "\n";
        int lastErr = 0;

        // Check if 3DS and LAN host appear to be on the same /24 subnet
        bool sameSubnet = false;
        if (!s_serverLanHost.empty()) {
            in_addr_t lanAddr = inet_addr(s_serverLanHost.c_str());
            if (lanAddr != INADDR_NONE) {
                if ((hostIp & 0x00FFFFFF) == (lanAddr & 0x00FFFFFF)) {
                    sameSubnet = true;
                }
            }
        }

        // Fast LAN timeout: 1500ms if on same subnet, 800ms if on a foreign subnet (e.g. mobile hotspot)
        int lanTimeout = sameSubnet ? 1500 : 800;

        // 1. Try LAN host first if configured
        if (!s_serverLanHost.empty()) {
            diag += "LAN " + s_serverLanHost + ":" + std::to_string(s_serverLanPort) + " -> ";
            int sock = OCR_ConnectHost(s_serverLanHost.c_str(), s_serverLanPort, lanTimeout, lastErr);
            if (sock >= 0) {
                s_lastConnectedEndpoint = s_serverLanHost + ":" + std::to_string(s_serverLanPort);
                diag += "OK\n";
                if (out_diag) *out_diag = diag;
                return sock;
            }
            diag += "ERR " + std::to_string(lastErr) + " (" + strerror(lastErr) + ")\n";
        }

        // 2. Fallback to external WAN host with 5000ms timeout
        if (!s_serverWanHost.empty() && s_serverWanHost != s_serverLanHost) {
            diag += "WAN " + s_serverWanHost + ":" + std::to_string(s_serverWanPort) + " -> ";
            int sock = OCR_ConnectHost(s_serverWanHost.c_str(), s_serverWanPort, 5000, lastErr);
            if (sock >= 0) {
                s_lastConnectedEndpoint = s_serverWanHost + ":" + std::to_string(s_serverWanPort);
                diag += "OK\n";
                if (out_diag) *out_diag = diag;
                return sock;
            }
            diag += "ERR " + std::to_string(lastErr) + " (" + strerror(lastErr) + ")\n";
        }

        if (s_serverLanHost.empty() && s_serverWanHost.empty()) {
            diag += "No Server IP configured! Scan QR code first.\n";
        }

        if (out_diag) *out_diag = diag;
        return -1;
    }

    static bool OCR_PerformHandshake(int sock, sha256_stream_t *tx, sha256_stream_t *rx)
    {
        char pskBuf[OCR_PSK_MAX_LEN];
        size_t pskLen = OCR_LoadPskFromSd(pskBuf, sizeof(pskBuf));
        if (pskLen == 0) return false;

        uint8_t s_pub[X25519_KEY_SIZE];
        if (recv_all(sock, s_pub, X25519_KEY_SIZE) < 0) {
            crypto_wipe(pskBuf, sizeof(pskBuf));
            return false;
        }

        uint8_t c_priv[X25519_KEY_SIZE];
        uint8_t c_pub[X25519_KEY_SIZE];
        uint8_t shared_secret[X25519_KEY_SIZE];

        if (!OCR_GenerateEphemeralSecret(c_priv)) {
            crypto_wipe(pskBuf, sizeof(pskBuf));
            return false;
        }
        x25519_basepoint(c_pub, c_priv);

        if (x25519_scalarmult(shared_secret, c_priv, s_pub) < 0) {
            crypto_wipe(c_priv, sizeof(c_priv));
            crypto_wipe(shared_secret, sizeof(shared_secret));
            crypto_wipe(pskBuf, sizeof(pskBuf));
            return false;
        }
        crypto_wipe(c_priv, sizeof(c_priv));

        sha256_ctx_t hctx;
        uint8_t clientResponse[X25519_KEY_SIZE + 32];
        memcpy(clientResponse, c_pub, X25519_KEY_SIZE);

        sha256_init(&hctx);
        sha256_update(&hctx, shared_secret, X25519_KEY_SIZE);
        sha256_update(&hctx, s_pub, X25519_KEY_SIZE);
        sha256_update(&hctx, c_pub, X25519_KEY_SIZE);
        sha256_update(&hctx, (const uint8_t *)pskBuf, pskLen);
        sha256_update(&hctx, (const uint8_t *)"CLIENT", 6);
        sha256_final(&hctx, clientResponse + X25519_KEY_SIZE);

        if (send_all(sock, clientResponse, sizeof(clientResponse)) < 0) {
            crypto_wipe(clientResponse, sizeof(clientResponse));
            crypto_wipe(shared_secret, sizeof(shared_secret));
            crypto_wipe(pskBuf, sizeof(pskBuf));
            return false;
        }
        crypto_wipe(clientResponse, sizeof(clientResponse));

        uint8_t recvServerDigest[32];
        uint8_t expectedServerDigest[32];
        if (recv_all(sock, recvServerDigest, 32) < 0) {
            crypto_wipe(shared_secret, sizeof(shared_secret));
            crypto_wipe(pskBuf, sizeof(pskBuf));
            return false;
        }

        sha256_init(&hctx);
        sha256_update(&hctx, shared_secret, X25519_KEY_SIZE);
        sha256_update(&hctx, s_pub, X25519_KEY_SIZE);
        sha256_update(&hctx, c_pub, X25519_KEY_SIZE);
        sha256_update(&hctx, (const uint8_t *)pskBuf, pskLen);
        sha256_update(&hctx, (const uint8_t *)"SERVER", 6);
        sha256_final(&hctx, expectedServerDigest);

        if (crypto_verify_32(recvServerDigest, expectedServerDigest) != 0) {
            crypto_wipe(expectedServerDigest, sizeof(expectedServerDigest));
            crypto_wipe(shared_secret, sizeof(shared_secret));
            crypto_wipe(pskBuf, sizeof(pskBuf));
            return false;
        }
        crypto_wipe(expectedServerDigest, sizeof(expectedServerDigest));

        sha256_stream_init(tx, shared_secret, s_pub, c_pub, pskBuf, STREAM_DIR_C2S);
        sha256_stream_init(rx, shared_secret, s_pub, c_pub, pskBuf, STREAM_DIR_S2C);
        crypto_wipe(shared_secret, sizeof(shared_secret));
        crypto_wipe(pskBuf, sizeof(pskBuf));
        return true;
    }

    bool OCR_TestServerConnection(std::string &out_msg)
    {
        char pskBuf[OCR_PSK_MAX_LEN];
        size_t pskLen = OCR_LoadPskFromSd(pskBuf, sizeof(pskBuf));
        if (pskLen == 0) {
            out_msg = "Error: /luma/ocr_psk.txt is missing on SD card!\nScan server QR code with [Start] or camera.";
            return false;
        }
        crypto_wipe(pskBuf, sizeof(pskBuf));

        std::string diag;
        int sock = OCR_ConnectServer(&diag);
        if (sock < 0) {
            out_msg = "Failed to connect to OCR Server!\n\n" + diag;
            return false;
        }

        u64 handshakeStart = svcGetSystemTick();
        sha256_stream_t tx, rx;
        bool ok = OCR_PerformHandshake(sock, &tx, &rx);
        u64 elapsedMs = (svcGetSystemTick() - handshakeStart) / 268123ULL;

        s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        closesocket(sock);
        crypto_wipe(&tx, sizeof(tx));
        crypto_wipe(&rx, sizeof(rx));

        if (!ok) {
            out_msg = "Connected to " + s_lastConnectedEndpoint + ", but PSK authentication failed!\nCheck /luma/ocr_psk.txt or scan QR code.";
            return false;
        }

        out_msg = "Success! Secure connection established.\nEndpoint: " + s_lastConnectedEndpoint +
                  "\nRound-trip latency: " + std::to_string(elapsedMs) + " ms\n\n" + diag;
        return true;
    }

    // Capture screen pixels safely via Screen::ReadPixel (stored as RGB565)
    static void CaptureScreen(const Screen &screen, u32 width, u32 height, u16 *out_565)
    {
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                Color c;
                screen.ReadPixel(x, y, c);
                out_565[y * width + x] = PackRGB565(c);
            }
        }
    }

    // Restore a rectangular region from saved pristine game framebuffer (RGB565)
    static void RestoreRect(const Screen &screen, int rx, int ry, int rw, int rh, const u16 *screen_565, int screenW, int screenH)
    {
        for (int dy = 0; dy < rh; ++dy) {
            int y = ry + dy;
            if (y < 0 || y >= screenH) continue;
            for (int dx = 0; dx < rw; ++dx) {
                int x = rx + dx;
                if (x < 0 || x >= screenW) continue;
                u8 r, g, b;
                UnpackRGB565(screen_565[y * screenW + x], r, g, b);
                screen.DrawPixel(x, y, Color(r, g, b));
            }
        }
    }

    static Color GetCleanPixelColor(int screenId, int x, int y, bool hasPopup, int popupTargetScreen, bool hasKanji = false, int kanjiTargetScreen = 0)
    {
        int screenW = (screenId == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
        if (x < 0 || x >= screenW || y < 0 || y >= 240) return Color::Black;

        if (hasKanji && kanjiTargetScreen == screenId && s_kanji_tall_565) {
            int xOffset = (screenId == SCREEN_ID_TOP) ? ((TOP_WIDTH - POPUP_WIDTH) / 2) : 0;
            if (x >= xOffset && x < xOffset + POPUP_WIDTH && y >= 0 && y < POPUP_HEIGHT) {
                int px = x - xOffset;
                int py;
                if (y < 32) {
                    py = y;
                } else if (y >= 216) {
                    py = (KANJI_TALL_HEIGHT - 24) + (y - 216);
                } else {
                    py = y + std::max(0, std::min(s_kanjiScrollY, s_kanjiMaxScrollY));
                }
                u8 r, g, b;
                UnpackRGB565(s_kanji_tall_565[py * POPUP_WIDTH + px], r, g, b);
                return Color(r, g, b);
            }
        }

        if (hasPopup && popupTargetScreen == screenId && s_popup_rgb) {
            int xOffset = (screenId == SCREEN_ID_TOP) ? ((TOP_WIDTH - POPUP_WIDTH) / 2) : 0;
            if (x >= xOffset && x < xOffset + POPUP_WIDTH && y >= 0 && y < POPUP_HEIGHT) {
                int px = x - xOffset;
                int py = y;
                size_t idx = (py * POPUP_WIDTH + px) * 3;
                return Color(s_popup_rgb[idx], s_popup_rgb[idx + 1], s_popup_rgb[idx + 2]);
            }
        }

        const u16 *bg = (screenId == SCREEN_ID_TOP) ? s_top_565 : s_bot_565;
        if (!bg) return Color::Black;
        u8 r, g, b;
        UnpackRGB565(bg[y * screenW + x], r, g, b);
        return Color(r, g, b);
    }

    static void RestorePixelUnder(int screenId, int x, int y, bool hasPopup, int popupTargetScreen, bool hasKanji = false, int kanjiTargetScreen = 0)
    {
        int screenW = (screenId == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
        if (x < 0 || x >= screenW || y < 0 || y >= 240) return;
        const Screen &screen = (screenId == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
        screen.DrawPixel(x, y, GetCleanPixelColor(screenId, x, y, hasPopup, popupTargetScreen, hasKanji, kanjiTargetScreen));
    }

    // Cursor Drawing & Restoration
    static void RestoreCursorArea(int screenId, int x, int y, bool hasPopup, int popupTargetScreen, bool hasKanji = false, int kanjiTargetScreen = 0)
    {
        for (int dy = -CURSOR_HALF; dy <= CURSOR_HALF; ++dy) {
            int py = y + dy;
            if (py < 0 || py >= 240) continue;
            for (int dx = -CURSOR_HALF; dx <= CURSOR_HALF; ++dx) {
                int px = x + dx;
                RestorePixelUnder(screenId, px, py, hasPopup, popupTargetScreen, hasKanji, kanjiTargetScreen);
            }
        }
    }

    static void DrawCursor(int screenId, int x, int y, bool translateMode, bool selectingBox)
    {
        const Screen &screen = (screenId == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
        int screenW = (screenId == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
        Color fg = translateMode ? (selectingBox ? Color(255, 220, 0) : Color(0, 229, 255)) : Color(255, 32, 32);
        Color black = Color::Black;

        for (int dy = -CURSOR_HALF; dy <= CURSOR_HALF; ++dy) {
            int py = y + dy;
            if (py < 0 || py >= 240) continue;
            for (int dx = -CURSOR_HALF; dx <= CURSOR_HALF; ++dx) {
                int px = x + dx;
                if (px < 0 || px >= screenW) continue;
                if (dx == 0 || dy == 0) {
                    screen.DrawPixel(px, py, fg);
                } else if (abs(dx) == 1 || abs(dy) == 1) {
                    screen.DrawPixel(px, py, black);
                }
            }
        }
    }

    // Selection Box Drawing & Restoration
    struct SelBoxState {
        int  screenId{0};
        int  x1{0}, y1{0};
        int  x2{0}, y2{0};
        int  drawn_rx1{0}, drawn_ry1{0};
        int  drawn_w{0}, drawn_h{0};
        bool active{false};
        bool is_drawn{false};
    };

    static void RestoreSelBoxArea(const SelBoxState &sb, bool hasPopup, int popupTargetScreen, bool hasKanji = false, int kanjiTargetScreen = 0)
    {
        if (!sb.is_drawn) return;
        int rx1 = sb.drawn_rx1;
        int ry1 = sb.drawn_ry1;
        int rx2 = rx1 + sb.drawn_w - 1;
        int ry2 = ry1 + sb.drawn_h - 1;

        for (int y = ry1; y <= ry2; ++y) {
            for (int x = rx1; x <= rx2; ++x) {
                bool isOuter = (x == rx1 || x == rx2 || y == ry1 || y == ry2);
                bool isInner = (x == rx1 + 1 || x == rx2 - 1 || y == ry1 + 1 || y == ry2 - 1);
                if (isOuter || isInner) {
                    RestorePixelUnder(sb.screenId, x, y, hasPopup, popupTargetScreen, hasKanji, kanjiTargetScreen);
                }
            }
        }
    }

    static void DrawSelBox(SelBoxState &sb)
    {
        if (!sb.active) return;
        const Screen &screen = (sb.screenId == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
        int screenW = (sb.screenId == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;

        int rx1 = std::min(sb.x1, sb.x2);
        int rx2 = std::max(sb.x1, sb.x2);
        int ry1 = std::min(sb.y1, sb.y2);
        int ry2 = std::max(sb.y1, sb.y2);

        rx1 = std::max(0, std::min(rx1, screenW - 1));
        rx2 = std::max(0, std::min(rx2, screenW - 1));
        ry1 = std::max(0, std::min(ry1, 239));
        ry2 = std::max(0, std::min(ry2, 239));

        int w = rx2 - rx1 + 1;
        int h = ry2 - ry1 + 1;
        sb.drawn_rx1 = rx1;
        sb.drawn_ry1 = ry1;
        sb.drawn_w = w;
        sb.drawn_h = h;

        for (int y = ry1; y <= ry2; ++y) {
            for (int x = rx1; x <= rx2; ++x) {
                bool isOuter = (x == rx1 || x == rx2 || y == ry1 || y == ry2);
                bool isInner = (x == rx1 + 1 || x == rx2 - 1 || y == ry1 + 1 || y == ry2 - 1);
                if (isOuter) {
                    Color c = (((x + y) & 4) == 0) ? Color(0, 240, 255) : Color(255, 232, 48);
                    screen.DrawPixel(x, y, c);
                } else if (isInner) {
                    screen.DrawPixel(x, y, Color(16, 22, 36));
                }
            }
        }
        sb.is_drawn = true;
    }

    static void DrawPopupImage(int targetScreenId, const u8 *popupRgb)
    {
        const Screen &screen = (targetScreenId == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
        int xOffset = (targetScreenId == SCREEN_ID_TOP) ? ((TOP_WIDTH - POPUP_WIDTH) / 2) : 0;
        for (int y = 0; y < POPUP_HEIGHT; ++y) {
            for (int x = 0; x < POPUP_WIDTH; ++x) {
                size_t idx = (y * POPUP_WIDTH + x) * 3;
                screen.DrawPixel(x + xOffset, y, Color(popupRgb[idx], popupRgb[idx+1], popupRgb[idx+2]));
            }
        }
    }
    // Draw a 240px-high crop of the tall kanji buffer starting at scrollY
    // Draw a 240px-high kanji window:
    //   - Screen y=0..31:   pinned header (from tall canvas y=0..31)
    //   - Screen y=32..215: scrollable content (from tall canvas y = y + scrollY)
    //   - Screen y=216..239: pinned footer (from tall canvas y=376..399)
    //   - Outer border (2px light-blue) + header/footer divider lines overlaid
    static void DrawKanjiCrop(int targetScreenId, const u16 *tall565, int scrollY)
    {
        static const int HEADER_HEIGHT   = 32;                     // pinned header rows
        static const int FOOTER_SCREEN_Y = 216;                    // screen y where footer starts
        static const int FOOTER_TALL_Y   = KANJI_TALL_HEIGHT - 24; // 376 — server-rendered footer

        scrollY = std::max(0, std::min(scrollY, s_kanjiMaxScrollY));
        const Screen &screen = (targetScreenId == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
        int xOffset = (targetScreenId == SCREEN_ID_TOP) ? ((TOP_WIDTH - POPUP_WIDTH) / 2) : 0;

        // 1. Pinned header (screen y = 0..31, source: tall y = 0..31)
        for (int y = 0; y < HEADER_HEIGHT; ++y) {
            for (int x = 0; x < POPUP_WIDTH; ++x) {
                u8 r, g, b;
                UnpackRGB565(tall565[y * POPUP_WIDTH + x], r, g, b);
                screen.DrawPixel(x + xOffset, y, Color(r, g, b));
            }
        }

        // 2. Scrollable content (screen y = 32..215, source: tall y = y + scrollY)
        for (int y = HEADER_HEIGHT; y < FOOTER_SCREEN_Y; ++y) {
            int tallY = y + scrollY;
            for (int x = 0; x < POPUP_WIDTH; ++x) {
                u8 r, g, b;
                UnpackRGB565(tall565[tallY * POPUP_WIDTH + x], r, g, b);
                screen.DrawPixel(x + xOffset, y, Color(r, g, b));
            }
        }

        // 3. Pinned footer (screen y = 216..239, source: tall y = 376..399)
        for (int y = FOOTER_SCREEN_Y; y < POPUP_HEIGHT; ++y) {
            int tallY = FOOTER_TALL_Y + (y - FOOTER_SCREEN_Y);
            for (int x = 0; x < POPUP_WIDTH; ++x) {
                u8 r, g, b;
                UnpackRGB565(tall565[tallY * POPUP_WIDTH + x], r, g, b);
                screen.DrawPixel(x + xOffset, y, Color(r, g, b));
            }
        }

        // 4. Fixed border overlay (2px light-blue)
        Color borderCol(88, 166, 255);
        // Top edge
        for (int x = 0; x < POPUP_WIDTH; ++x) {
            screen.DrawPixel(x + xOffset, 0, borderCol);
            screen.DrawPixel(x + xOffset, 1, borderCol);
        }
        // Bottom edge
        for (int x = 0; x < POPUP_WIDTH; ++x) {
            screen.DrawPixel(x + xOffset, POPUP_HEIGHT - 2, borderCol);
            screen.DrawPixel(x + xOffset, POPUP_HEIGHT - 1, borderCol);
        }
        // Left and right edges
        for (int y = 0; y < POPUP_HEIGHT; ++y) {
            screen.DrawPixel(0 + xOffset, y, borderCol);
            screen.DrawPixel(1 + xOffset, y, borderCol);
            screen.DrawPixel(POPUP_WIDTH - 2 + xOffset, y, borderCol);
            screen.DrawPixel(POPUP_WIDTH - 1 + xOffset, y, borderCol);
        }
        // Header separator line (screen y=31)
        for (int x = 2; x < POPUP_WIDTH - 2; ++x) {
            screen.DrawPixel(x + xOffset, HEADER_HEIGHT - 1, borderCol);
        }
        // Footer separator line (screen y=216)
        for (int x = 2; x < POPUP_WIDTH - 2; ++x) {
            screen.DrawPixel(x + xOffset, FOOTER_SCREEN_Y, borderCol);
        }
    }

    // =========================================================================
    // Client-Side Popup Bundle Parsing & 4-bpp Glyph Rendering
    // =========================================================================

    static const int GLOSS_VISIBLE_LINES = 7;
    static const int POS_WINDOW_CHARS = 36;

    struct PopupGlyph {
        uint8_t width{0};
        uint8_t height{0};
        int8_t  offsetX{0};
        int8_t  offsetY{0};
        uint8_t advanceX{0};
        std::vector<uint8_t> packed;
    };

    struct PopupEntry {
        std::string matchedSurface;
        std::string contextText;
        std::string headword;
        std::string reading;
        std::string pos;
        bool found{false};
        bool ocrDisagreement{false};
        int16_t contextIdx{-1};
        std::string ocrBadge;
        std::string rankJlpt;
        std::vector<std::string> wrappedLines;
    };

    struct PopupVariant {
        std::string enginesLabel;
        std::string ocrText;
        std::vector<PopupEntry> entries;
    };

    using GlyphMap = std::unordered_map<uint64_t, PopupGlyph>;

    struct PopupBundle {
        bool active{false};
        uint8_t sourceScreenId{0};
        uint16_t x{0};
        uint16_t y{0};
        std::string footerLookup{"A"};
        std::string footerMode{"SEL"};
        std::string footerAnki{"X"};
        std::string statusBanner;
        uint8_t statusLevel{0};
        GlyphMap glyphs;
        std::vector<PopupVariant> variants;
        size_t curVariantIdx{0};
        size_t curEntryIdx{0};
        int glossScrollY{0};
        int posScrollX{0};
    };

    struct ScreenSpotVariant {
        std::string enginesLabel;
        std::string ocrText;
        std::vector<uint16_t> entryIndices;
    };

    struct ScreenSpot {
        int16_t x1{0}, y1{0}, x2{0}, y2{0};
        std::vector<ScreenSpotVariant> variants;
    };

    struct ScreenParagraph {
        int16_t x1{0}, y1{0}, x2{0}, y2{0};
        uint16_t firstSpotIdx{0}, lastSpotIdx{0};
    };

    struct ScreenBundle {
        bool ready{false};
        uint8_t sourceScreenId{0};
        std::string footerLookup{"A"};
        std::string footerMode{"SEL"};
        std::string footerAnki{"X"};
        GlyphMap glyphs;
        std::vector<std::string> contexts;
        std::vector<PopupEntry> entries;
        std::vector<ScreenSpot> spots;
        std::vector<ScreenParagraph> paragraphs;
    };

    static inline uint64_t GlyphKey(uint8_t fontId, uint32_t cp)
    {
        return ((uint64_t)fontId << 32) | (uint64_t)cp;
    }

    static std::vector<uint32_t> Utf8ToCodepoints(const std::string &s)
    {
        std::vector<uint32_t> cps;
        size_t i = 0;
        const size_t n = s.size();
        while (i < n) {
            uint8_t c = (uint8_t)s[i];
            if (c < 0x80) {
                cps.push_back(c);
                ++i;
            } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
                cps.push_back(((uint32_t)(c & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F));
                i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
                cps.push_back(((uint32_t)(c & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F));
                i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
                cps.push_back(((uint32_t)(c & 0x07) << 18) | (((uint8_t)s[i + 1] & 0x3F) << 12) | (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F));
                i += 4;
            } else {
                cps.push_back('?');
                ++i;
            }
        }
        return cps;
    }

    static std::string CodepointsToUtf8(const std::vector<uint32_t> &cps, size_t start, size_t count)
    {
        std::string out;
        size_t end = std::min(cps.size(), start + count);
        for (size_t i = start; i < end; ++i) {
            uint32_t cp = cps[i];
            if (cp < 0x80) {
                out.push_back((char)cp);
            } else if (cp < 0x800) {
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

    static bool ReadU8(const uint8_t *buf, size_t len, size_t &off, uint8_t &out)
    {
        if (off + 1 > len) return false;
        out = buf[off++];
        return true;
    }

    static bool ReadU16(const uint8_t *buf, size_t len, size_t &off, uint16_t &out)
    {
        if (off + 2 > len) return false;
        out = (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
        off += 2;
        return true;
    }

    static bool ReadI16(const uint8_t *buf, size_t len, size_t &off, int16_t &out)
    {
        uint16_t v = 0;
        if (!ReadU16(buf, len, off, v)) return false;
        out = (int16_t)v;
        return true;
    }

    static bool ReadU32(const uint8_t *buf, size_t len, size_t &off, uint32_t &out)
    {
        if (off + 4 > len) return false;
        out = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) | ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
        off += 4;
        return true;
    }

    static bool ReadStrU8(const uint8_t *buf, size_t len, size_t &off, std::string &out)
    {
        uint8_t n = 0;
        if (!ReadU8(buf, len, off, n) || off + n > len) return false;
        out.assign((const char *)(buf + off), n);
        off += n;
        return true;
    }

    static bool ReadStrU16(const uint8_t *buf, size_t len, size_t &off, std::string &out)
    {
        uint16_t n = 0;
        if (!ReadU16(buf, len, off, n) || off + n > len) return false;
        out.assign((const char *)(buf + off), n);
        off += n;
        return true;
    }

    static bool ParsePopupGlyphs(const uint8_t *buf, size_t len, size_t &off, std::unordered_map<uint64_t, PopupGlyph> &glyphs)
    {
        uint16_t count = 0;
        if (!ReadU16(buf, len, off, count)) return false;
        if (count > 4096 || !HeapCanFit((size_t)count * 48 + 49152)) return false;
        for (uint16_t i = 0; i < count; ++i) {
            uint32_t cp = 0;
            uint8_t fid = 0, gw = 0, gh = 0, adv = 0;
            uint8_t oxRaw = 0, oyRaw = 0;
            if (!ReadU32(buf, len, off, cp) || !ReadU8(buf, len, off, fid) ||
                !ReadU8(buf, len, off, gw) || !ReadU8(buf, len, off, gh) ||
                !ReadU8(buf, len, off, oxRaw) || !ReadU8(buf, len, off, oyRaw) ||
                !ReadU8(buf, len, off, adv)) {
                return false;
            }
            size_t packedBytes = ((size_t)gw * (size_t)gh + 1) / 2;
            if (off + packedBytes > len) return false;
            PopupGlyph g;
            g.width = gw;
            g.height = gh;
            g.offsetX = (int8_t)oxRaw;
            g.offsetY = (int8_t)oyRaw;
            g.advanceX = adv;
            if (packedBytes > 0) {
                g.packed.assign(buf + off, buf + off + packedBytes);
            }
            off += packedBytes;
            glyphs[GlyphKey(fid, cp)] = std::move(g);
        }
        return true;
    }

    static uint8_t ClassifyStatusBanner(const std::string &banner)
    {
        if (banner.compare(0, 10, "Anki Error") == 0 || banner.compare(0, 11, "Set AnkiWeb") == 0) return 2;
        if (banner.compare(0, 7, "Adding ") == 0 || banner.compare(0, 12, "Translating ") == 0) return 1;
        return 0;
    }

    static bool ParseEntryFields(const uint8_t *buf, size_t len, size_t &off, PopupEntry &e, bool hasCtxIdx, const std::vector<std::string> *contexts)
    {
        uint8_t fFlag = 0, dFlag = 0;
        uint16_t numLines = 0;
        if (!ReadStrU16(buf, len, off, e.matchedSurface)) return false;
        if (hasCtxIdx) {
            uint16_t cIdx = 0;
            if (!ReadU16(buf, len, off, cIdx)) return false;
            e.contextIdx = (contexts && cIdx < contexts->size()) ? (int16_t)cIdx : -1;
        } else {
            if (!ReadStrU16(buf, len, off, e.contextText)) return false;
        }
        if (!ReadStrU16(buf, len, off, e.headword) ||
            !ReadStrU16(buf, len, off, e.reading) ||
            !ReadStrU16(buf, len, off, e.pos) ||
            !ReadU8(buf, len, off, fFlag) ||
            !ReadU8(buf, len, off, dFlag) ||
            !ReadStrU8(buf, len, off, e.ocrBadge) ||
            !ReadStrU8(buf, len, off, e.rankJlpt) ||
            !ReadU16(buf, len, off, numLines)) {
            return false;
        }
        e.found = (fFlag != 0);
        e.ocrDisagreement = (dFlag != 0);
        e.wrappedLines.reserve(numLines);
        for (uint16_t li = 0; li < numLines; ++li) {
            std::string ln;
            if (!ReadStrU16(buf, len, off, ln)) return false;
            e.wrappedLines.push_back(std::move(ln));
        }
        return true;
    }

    static bool ParsePopupBundle(const uint8_t *buf, size_t len, PopupBundle &out)
    {
        PopupBundle b;
        size_t off = 0;
        if (!ReadU8(buf, len, off, b.sourceScreenId) ||
            !ReadU16(buf, len, off, b.x) ||
            !ReadU16(buf, len, off, b.y) ||
            !ReadStrU8(buf, len, off, b.footerLookup) ||
            !ReadStrU8(buf, len, off, b.footerMode) ||
            !ReadStrU8(buf, len, off, b.footerAnki) ||
            !ReadStrU8(buf, len, off, b.statusBanner)) {
            return false;
        }
        b.statusLevel = ClassifyStatusBanner(b.statusBanner);
        if (!ParsePopupGlyphs(buf, len, off, b.glyphs)) return false;

        uint8_t numVars = 0;
        if (!ReadU8(buf, len, off, numVars)) return false;
        b.variants.reserve(numVars);
        for (uint8_t vi = 0; vi < numVars; ++vi) {
            PopupVariant v;
            uint16_t numEntries = 0;
            if (!ReadStrU8(buf, len, off, v.enginesLabel) ||
                !ReadStrU16(buf, len, off, v.ocrText) ||
                !ReadU16(buf, len, off, numEntries)) {
                return false;
            }
            v.entries.reserve(numEntries);
            for (uint16_t ei = 0; ei < numEntries; ++ei) {
                PopupEntry e;
                if (!ParseEntryFields(buf, len, off, e, false, nullptr)) return false;
                v.entries.push_back(std::move(e));
            }
            if (!v.entries.empty()) {
                b.variants.push_back(std::move(v));
            }
        }
        if (b.variants.empty()) return false;
        b.active = true;
        out = std::move(b);
        return true;
    }

    static bool ParseScreenBundle(SessionBuffer &rawSb, size_t len, ScreenBundle &out)
    {
        if (!rawSb.ptr) return false;
        const uint8_t *buf = rawSb.ptr;
        size_t off = 0;
        uint8_t compFlag = 0;
        uint32_t rawLen = 0;
        if (!ReadU8(buf, len, off, compFlag) || !ReadU32(buf, len, off, rawLen)) return false;
        if (rawLen == 0 || rawLen > 2 * 1024 * 1024) return false;

        SessionBuffer decompSb;
        AutoFreeSessionBuffer decompGuard{decompSb};
        const uint8_t *raw = buf + off;
        size_t rLen = len - off;
        if (compFlag & 1) {
            if (!decompSb.Alloc(0x07420000, rawLen, 0x073E0000)) {
                return false;
            }
            uLongf destLen = (uLongf)rawLen;
            if (uncompress(decompSb.ptr, &destLen, buf + off, (uLong)(len - off)) != Z_OK || destLen == 0) {
                return false;
            }
            rawSb.Free();
            raw = decompSb.ptr;
            rLen = (size_t)destLen;
        } else if (rLen < rawLen) {
            return false;
        }

        if (!HeapCanFit(rLen + 98304)) {
            return false;
        }

        size_t pOff = 0;
        ScreenBundle b;
        if (!ReadU8(raw, rLen, pOff, b.sourceScreenId) ||
            !ReadStrU8(raw, rLen, pOff, b.footerLookup) ||
            !ReadStrU8(raw, rLen, pOff, b.footerMode) ||
            !ReadStrU8(raw, rLen, pOff, b.footerAnki)) {
            return false;
        }
        if (!ParsePopupGlyphs(raw, rLen, pOff, b.glyphs)) return false;

        uint16_t numCtx = 0;
        if (!ReadU16(raw, rLen, pOff, numCtx)) return false;
        b.contexts.resize(numCtx);
        for (uint16_t i = 0; i < numCtx; ++i) {
            if (!ReadStrU16(raw, rLen, pOff, b.contexts[i])) return false;
        }

        uint16_t numEntries = 0;
        if (!ReadU16(raw, rLen, pOff, numEntries)) return false;
        size_t remBytes = (pOff < rLen) ? (rLen - pOff) : 0;
        if (numEntries > 4096 || !HeapCanFit((size_t)numEntries * sizeof(PopupEntry) + remBytes + 65536)) {
            return false;
        }
        b.entries.resize(numEntries);
        for (uint16_t i = 0; i < numEntries; ++i) {
            if (!ParseEntryFields(raw, rLen, pOff, b.entries[i], true, &b.contexts)) return false;
        }

        uint16_t numSpots = 0;
        if (!ReadU16(raw, rLen, pOff, numSpots)) return false;
        b.spots.resize(numSpots);
        for (uint16_t i = 0; i < numSpots; ++i) {
            ScreenSpot &sp = b.spots[i];
            uint8_t numVars = 0;
            if (!ReadI16(raw, rLen, pOff, sp.x1) || !ReadI16(raw, rLen, pOff, sp.y1) ||
                !ReadI16(raw, rLen, pOff, sp.x2) || !ReadI16(raw, rLen, pOff, sp.y2) ||
                !ReadU8(raw, rLen, pOff, numVars)) {
                return false;
            }
            sp.variants.reserve(numVars);
            for (uint8_t vi = 0; vi < numVars; ++vi) {
                ScreenSpotVariant sv;
                uint16_t nE = 0;
                if (!ReadStrU8(raw, rLen, pOff, sv.enginesLabel) ||
                    !ReadStrU16(raw, rLen, pOff, sv.ocrText) ||
                    !ReadU16(raw, rLen, pOff, nE)) {
                    return false;
                }
                sv.entryIndices.reserve(nE);
                for (uint16_t k = 0; k < nE; ++k) {
                    uint16_t eIdx = 0;
                    if (!ReadU16(raw, rLen, pOff, eIdx) || eIdx >= b.entries.size()) return false;
                    sv.entryIndices.push_back(eIdx);
                }
                if (!sv.entryIndices.empty()) sp.variants.push_back(std::move(sv));
            }
        }

        uint16_t numParas = 0;
        if (!ReadU16(raw, rLen, pOff, numParas)) return false;
        b.paragraphs.reserve(numParas);
        for (uint16_t i = 0; i < numParas; ++i) {
            ScreenParagraph sp;
            if (!ReadI16(raw, rLen, pOff, sp.x1) || !ReadI16(raw, rLen, pOff, sp.y1) ||
                !ReadI16(raw, rLen, pOff, sp.x2) || !ReadI16(raw, rLen, pOff, sp.y2) ||
                !ReadU16(raw, rLen, pOff, sp.firstSpotIdx) || !ReadU16(raw, rLen, pOff, sp.lastSpotIdx)) {
                return false;
            }
            if (sp.firstSpotIdx < b.spots.size() && sp.lastSpotIdx < b.spots.size()) {
                b.paragraphs.push_back(sp);
            }
        }

        b.ready = true;
        out = std::move(b);
        return true;
    }

    static bool FindScreenSpot(const ScreenBundle &sb, int x, int y, size_t &outSpotIdx)
    {
        if (!sb.ready || sb.spots.empty()) return false;
        float bestDistSq = 1e9f;
        size_t bestIdx = 0;
        bool foundAny = false;
        for (size_t i = 0; i < sb.spots.size(); ++i) {
            const ScreenSpot &sp = sb.spots[i];
            if (sp.variants.empty()) continue;
            float dx = std::max(0.0f, std::max((float)sp.x1 - (float)x, (float)x - (float)sp.x2));
            float dy = std::max(0.0f, std::max((float)sp.y1 - (float)y, (float)y - (float)sp.y2));
            float sd = (dx * 0.65f) * (dx * 0.65f) + dy * dy;
            if (sd < bestDistSq) {
                bestDistSq = sd;
                bestIdx = i;
                foundAny = true;
            }
        }
        if (foundAny && bestDistSq <= (26.0f * 26.0f)) {
            outSpotIdx = bestIdx;
            return true;
        }
        float bestHDist = 1e9f;
        size_t bestEdgeSpot = 0;
        bool foundEdge = false;
        for (const ScreenParagraph &p : sb.paragraphs) {
            float pcy = ((float)p.y1 + (float)p.y2) * 0.5f;
            float ph = std::max(10.0f, (float)(p.y2 - p.y1));
            float dy = ((float)y > pcy) ? ((float)y - pcy) : (pcy - (float)y);
            if (dy <= std::max(16.0f, ph * 0.85f)) {
                float hDist = std::max(0.0f, std::max((float)p.x1 - (float)x, (float)x - (float)p.x2));
                if (hDist < bestHDist && hDist <= 50.0f) {
                    size_t candIdx = ((float)x <= ((float)p.x1 + (float)p.x2) * 0.5f) ? p.firstSpotIdx : p.lastSpotIdx;
                    if (candIdx < sb.spots.size() && !sb.spots[candIdx].variants.empty()) {
                        bestHDist = hDist;
                        bestEdgeSpot = candIdx;
                        foundEdge = true;
                    }
                }
            }
        }
        if (foundEdge) {
            outSpotIdx = bestEdgeSpot;
            return true;
        }
        return false;
    }

    static bool ParseStatusBannerPacket(const uint8_t *buf, size_t len, GlyphMap &glyphs, std::string &statusBanner, uint8_t &statusLevel)
    {
        size_t off = 0;
        uint8_t lvl = 0;
        std::string txt;
        if (!ReadU8(buf, len, off, lvl) || !ReadStrU16(buf, len, off, txt)) return false;
        if (!ParsePopupGlyphs(buf, len, off, glyphs)) return false;
        statusLevel = lvl;
        statusBanner = std::move(txt);
        return true;
    }

    static int MeasureGlyphTextWidth(const GlyphMap &glyphs, uint8_t fontId, const std::string &text)
    {
        int w = 0;
        for (uint32_t cp : Utf8ToCodepoints(text)) {
            auto it = glyphs.find(GlyphKey(fontId, cp));
            if (it != glyphs.end()) {
                w += it->second.advanceX;
            } else {
                w += (cp > 0x7E) ? 12 : 7;
            }
        }
        return w;
    }

    static std::string TruncateGlyphText(const GlyphMap &glyphs, uint8_t fontId, const std::string &text, int maxPx)
    {
        if (MeasureGlyphTextWidth(glyphs, fontId, text) <= maxPx) return text;
        std::vector<uint32_t> cps = Utf8ToCodepoints(text);
        int ellW = MeasureGlyphTextWidth(glyphs, fontId, "…");
        int w = 0;
        size_t keep = 0;
        for (size_t i = 0; i < cps.size(); ++i) {
            auto it = glyphs.find(GlyphKey(fontId, cps[i]));
            int adv = (it != glyphs.end()) ? it->second.advanceX : ((cps[i] > 0x7E) ? 12 : 7);
            if (w + adv + ellW > maxPx && i > 0) break;
            w += adv;
            keep = i + 1;
        }
        return CodepointsToUtf8(cps, 0, keep) + "…";
    }

    struct PopupTitleLayout {
        std::vector<std::string> lines;
        int posY;
        int divY;
        int glossStartY;
        int visibleGlossLines;
    };

    static void AppendWrappedGlyphSegment(const GlyphMap &glyphs, uint8_t fontId, const std::string &seg,
                                          int maxPx, int maxLines, std::vector<std::string> &out)
    {
        if (seg.empty() || (int)out.size() >= maxLines) return;
        std::vector<uint32_t> cps = Utf8ToCodepoints(seg);
        size_t pos = 0;
        while (pos < cps.size() && (int)out.size() < maxLines) {
            if ((int)out.size() == maxLines - 1) {
                std::string rem = CodepointsToUtf8(cps, pos, cps.size() - pos);
                out.push_back(TruncateGlyphText(glyphs, fontId, rem, maxPx));
                break;
            }
            int w = 0;
            size_t end = pos;
            while (end < cps.size()) {
                auto it = glyphs.find(GlyphKey(fontId, cps[end]));
                int adv = (it != glyphs.end()) ? (int)it->second.advanceX : ((cps[end] > 0x7E) ? ((fontId == 2) ? 18 : 12) : ((fontId == 2) ? 10 : 7));
                if (w + adv > maxPx && end > pos) break;
                w += adv;
                end++;
            }
            if (end == pos) end = pos + 1;
            out.push_back(CodepointsToUtf8(cps, pos, end - pos));
            pos = end;
        }
    }

    static PopupTitleLayout ComputePopupTitleLayout(const GlyphMap &glyphs, const PopupEntry &res,
                                                    int maxPx = 295, int maxLines = 3)
    {
        PopupTitleLayout tl;
        bool hasReading = (!res.reading.empty() && res.reading != res.headword);
        std::string fullTitle = hasReading ? (res.headword + " 【" + res.reading + "】") : res.headword;
        if (MeasureGlyphTextWidth(glyphs, 2, fullTitle) <= maxPx) {
            tl.lines.push_back(fullTitle);
        } else if (hasReading) {
            AppendWrappedGlyphSegment(glyphs, 2, res.headword, maxPx, maxLines, tl.lines);
            AppendWrappedGlyphSegment(glyphs, 2, "【" + res.reading + "】", maxPx, maxLines, tl.lines);
        } else {
            AppendWrappedGlyphSegment(glyphs, 2, res.headword, maxPx, maxLines, tl.lines);
        }
        if (tl.lines.empty()) tl.lines.push_back(res.headword);
        int extraY = std::max(0, (int)tl.lines.size() - 1) * 22;
        tl.posY = 64 + extraY;
        tl.divY = 85 + extraY;
        tl.glossStartY = 90 + extraY;
        tl.visibleGlossLines = std::max(1, std::min(GLOSS_VISIBLE_LINES, (212 - tl.glossStartY + 3) / 17));
        return tl;
    }

    static void DrawGlyphText(const Screen &screen, int xOff, const GlyphMap &glyphs, uint8_t fontId,
                              const std::string &text, int startX, int startY, const Color &fg, const Color &bg, int maxRightX = 315)
    {
        int penX = startX;
        for (uint32_t cp : Utf8ToCodepoints(text)) {
            auto it = glyphs.find(GlyphKey(fontId, cp));
            if (it == glyphs.end()) {
                penX += (cp > 0x7E) ? 12 : 7;
                if (penX >= maxRightX) break;
                continue;
            }
            const PopupGlyph &g = it->second;
            if (g.width > 0 && g.height > 0 && !g.packed.empty()) {
                int gx0 = penX + g.offsetX;
                int gy0 = startY + g.offsetY;
                size_t totalPx = (size_t)g.width * (size_t)g.height;
                for (size_t p = 0; p < totalPx; ++p) {
                    uint8_t byteVal = g.packed[p >> 1];
                    uint8_t nib = ((p & 1) == 0) ? (byteVal >> 4) : (byteVal & 0x0F);
                    if (nib == 0) continue;
                    int px = gx0 + (int)(p % g.width);
                    int py = gy0 + (int)(p / g.width);
                    if (px < 2 || px >= maxRightX || py < 2 || py >= 238) continue;
                    if (nib == 15) {
                        screen.DrawPixel(px + xOff, py, fg);
                    } else {
                        uint16_t a = (uint16_t)nib * 17;
                        uint16_t inv = 255 - a;
                        Color blended(
                            (uint8_t)((bg.r * inv + fg.r * a) >> 8),
                            (uint8_t)((bg.g * inv + fg.g * a) >> 8),
                            (uint8_t)((bg.b * inv + fg.b * a) >> 8)
                        );
                        screen.DrawPixel(px + xOff, py, blended);
                    }
                }
            }
            penX += g.advanceX;
            if (penX >= maxRightX) break;
        }
    }

    struct CachedPopupState {
        bool active{false};
        uint8_t sourceScreenId{0};
        uint16_t clickX{0}, clickY{0};
        size_t spotIdx{0};
        size_t curVariantIdx{0};
        size_t curEntryIdx{0};
        int glossScrollY{0};
        int posScrollX{0};
        std::string statusBanner;
        uint8_t statusLevel{0};
    };

    static void DrawPopupView(int targetScreenId, const GlyphMap &glyphs, uint8_t sourceScreenId, uint16_t clickX, uint16_t clickY,
                              const std::string &footerLookup, const std::string &footerMode, const std::string &footerAnki,
                              const std::string &statusBanner, uint8_t statusLevel,
                              size_t numVars, size_t eIdx, int totalEntries, const PopupEntry &res,
                              int glossScrollY, int posScrollX, const std::vector<std::string> *contexts = nullptr)
    {
        const Screen &screen = (targetScreenId == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
        int xOff = (targetScreenId == SCREEN_ID_TOP) ? ((TOP_WIDTH - POPUP_WIDTH) / 2) : 0;

        Color bgBody(20, 24, 34);
        Color bgHeader(34, 42, 64);
        Color bgFooter(28, 34, 50);
        Color borderCol = !res.found ? Color(235, 130, 90)
                        : (res.ocrDisagreement ? Color(245, 180, 75) : Color(88, 166, 255));

        screen.DrawRect(xOff, 0, POPUP_WIDTH, POPUP_HEIGHT, borderCol, true);
        screen.DrawRect(xOff + 2, 2, POPUP_WIDTH - 4, POPUP_HEIGHT - 4, bgBody, true);
        screen.DrawRect(xOff + 2, 2, POPUP_WIDTH - 4, 29, bgHeader, true);

        // Match badge [i/N]
        std::string badgeText = "[" + std::to_string((int)eIdx + 1) + "/" + std::to_string(totalEntries) + "]";
        int badgeW = MeasureGlyphTextWidth(glyphs, 0, badgeText);
        int badgeX = std::max(200, 312 - badgeW);
        Color badgeCol = (totalEntries > 1) ? Color(255, 220, 110) : Color(150, 175, 210);
        DrawGlyphText(screen, xOff, glyphs, 0, badgeText, badgeX, 7, badgeCol, bgHeader);

        // OCR badge (Gemini / [4/4✓] / [OCR 1/3!])
        int ocrBadgeW = 0;
        if (!res.ocrBadge.empty()) {
            ocrBadgeW = MeasureGlyphTextWidth(glyphs, 0, res.ocrBadge);
            int ocrBadgeX = std::max(140, badgeX - ocrBadgeW - 5);
            Color ocrCol = (res.ocrDisagreement && numVars > 1) ? Color(255, 175, 80) : Color(125, 230, 165);
            DrawGlyphText(screen, xOff, glyphs, 0, res.ocrBadge, ocrBadgeX, 7, ocrCol, bgHeader);
        }

        // Header label
        const std::string &ctxStr = (res.contextIdx >= 0 && contexts && (size_t)res.contextIdx < contexts->size())
                                    ? (*contexts)[(size_t)res.contextIdx]
                                    : res.contextText;
        std::string surfInfo;
        if (res.headword == "Selection Translation") {
            surfInfo = "Selection: " + res.matchedSurface;
        } else if (!res.matchedSurface.empty() && res.matchedSurface != res.headword) {
            surfInfo = "Match: " + res.matchedSurface + " -> " + res.headword;
        } else {
            surfInfo = "Text: " + ctxStr;
        }
        std::string tagStr;
        if (res.headword == "Selection Translation") {
            tagStr = "[Translate] ";
        } else if (!res.rankJlpt.empty()) {
            tagStr = "[" + res.rankJlpt + "] ";
        }
        int maxHdrW = std::max(125, 312 - badgeW - ocrBadgeW - 14);
        std::string hdrText = tagStr + surfInfo;
        hdrText = TruncateGlyphText(glyphs, 0, hdrText, maxHdrW);
        DrawGlyphText(screen, xOff, glyphs, 0, hdrText, 8, 7, Color(185, 215, 255), bgHeader, badgeX - ocrBadgeW - 4);

        // Title (headword + reading) — wrap to lines below instead of truncating
        PopupTitleLayout tl = ComputePopupTitleLayout(glyphs, res);
        for (size_t i = 0; i < tl.lines.size(); ++i) {
            DrawGlyphText(screen, xOff, glyphs, 2, tl.lines[i], 12, 36 + (int)i * 22, Color(255, 230, 115), bgBody, 312);
        }

        // Grammar / POS horizontally scrollable line
        std::vector<uint32_t> posCps = Utf8ToCodepoints(res.pos);
        int maxPosX = std::max(0, (int)posCps.size() - POS_WINDOW_CHARS);
        int posOff = std::max(0, std::min(posScrollX, maxPosX));
        std::string posSlice = CodepointsToUtf8(posCps, (size_t)posOff, POS_WINDOW_CHARS);
        if (posOff > 0) {
            DrawGlyphText(screen, xOff, glyphs, 1, "<", 4, tl.posY, Color(255, 220, 110), bgBody, 16);
        }
        int posStartX = (posOff > 0) ? 14 : 12;
        int posMaxRight = (posOff < maxPosX) ? 300 : 312;
        DrawGlyphText(screen, xOff, glyphs, 1, posSlice, posStartX, tl.posY, Color(125, 210, 175), bgBody, posMaxRight);
        if (posOff < maxPosX) {
            DrawGlyphText(screen, xOff, glyphs, 1, ">", 306, tl.posY, Color(255, 220, 110), bgBody, 317);
        }

        // Divider line
        screen.DrawRect(xOff + 12, tl.divY, 296, 1, Color(55, 68, 95), true);

        // Vertically scrollable gloss lines
        int totalLines = (int)res.wrappedLines.size();
        int maxGlossY = std::max(0, totalLines - tl.visibleGlossLines);
        int gOff = std::max(0, std::min(glossScrollY, maxGlossY));
        int visCount = std::min(tl.visibleGlossLines, totalLines - gOff);
        int yCur = tl.glossStartY;
        for (int i = 0; i < visCount; ++i) {
            const std::string &ln = res.wrappedLines[(size_t)(gOff + i)];
            Color lc(235, 240, 248);
            if (ln.compare(0, 3, "\xE2\x98\x85") == 0) {         // ★
                lc = Color(255, 195, 95);
            } else if (ln.compare(0, 6, "\xE2\x94\x80\xE2\x94\x80") == 0) { // ──
                lc = Color(120, 195, 255);
            } else if (ln.compare(0, 3, "\xE2\x80\xA2") == 0) {  // •
                lc = Color(255, 225, 135);
            }
            DrawGlyphText(screen, xOff, glyphs, 1, ln, 12, yCur, lc, bgBody, 309);
            yCur += 17;
        }

        // Scrollbar
        if (maxGlossY > 0) {
            int trackTop = tl.glossStartY;
            int trackH = std::max(24, 206 - tl.glossStartY);
            screen.DrawRect(xOff + 311, trackTop, 4, trackH, Color(38, 46, 68), true);
            int thumbH = std::max(14, (trackH * tl.visibleGlossLines) / std::max(1, totalLines));
            int thumbY = trackTop + ((trackH - thumbH) * gOff) / maxGlossY;
            screen.DrawRect(xOff + 311, thumbY, 4, thumbH, Color(88, 166, 255), true);
        }

        // Footer bar / status banner
        screen.DrawRect(xOff + 2, 214, POPUP_WIDTH - 4, 24, bgFooter, true);
        if (!statusBanner.empty()) {
            Color bCol = (statusLevel == 2) ? Color(255, 160, 120)
                       : ((statusLevel == 1) ? Color(255, 225, 110) : Color(120, 240, 165));
            std::string bDisp = TruncateGlyphText(glyphs, 0, statusBanner, 304);
            DrawGlyphText(screen, xOff, glyphs, 0, bDisp, 8, 219, bCol, bgFooter, 316);
        } else {
            std::string footer;
            if (res.ocrDisagreement) {
                footer = (res.headword == "Selection Translation")
                       ? ("[" + footerLookup + "]Box [ZL/ZR]OCR [L/R]Vocab [DPad]Scrl [" + footerAnki + "]Anki")
                       : ("[" + footerLookup + "]Word [ZL/ZR]OCR [L/R]Match [DPad]Scrl [" + footerAnki + "]Anki");
            } else {
                footer = (res.headword == "Selection Translation")
                       ? ("[" + footerLookup + "]Box [" + footerMode + "]Dict [L/R]Vocab [DPad]Scrl [" + footerAnki + "]Anki")
                       : ("[" + footerLookup + "]Word [" + footerMode + "]Box [L/R]Match [DPad]Scrl [" + footerAnki + "]Anki");
            }
            DrawGlyphText(screen, xOff, glyphs, 0, footer, 6, 219, Color(145, 160, 185), bgFooter, 316);
        }
    }

    static void DrawActivePopup(int targetScreenId, const PopupBundle &popupBundle,
                                const CachedPopupState &cached, const ScreenBundle screenBundles[2])
    {
        if (cached.active && cached.sourceScreenId < 2) {
            const ScreenBundle &sb = screenBundles[cached.sourceScreenId];
            if (!sb.ready || cached.spotIdx >= sb.spots.size()) return;
            const ScreenSpot &sp = sb.spots[cached.spotIdx];
            if (sp.variants.empty()) return;
            size_t vIdx = std::min(cached.curVariantIdx, sp.variants.size() - 1);
            const ScreenSpotVariant &sv = sp.variants[vIdx];
            if (sv.entryIndices.empty()) return;
            size_t eIdx = std::min(cached.curEntryIdx, sv.entryIndices.size() - 1);
            uint16_t poolIdx = sv.entryIndices[eIdx];
            if (poolIdx >= sb.entries.size()) return;
            DrawPopupView(targetScreenId, sb.glyphs, cached.sourceScreenId, cached.clickX, cached.clickY,
                          sb.footerLookup, sb.footerMode, sb.footerAnki,
                          cached.statusBanner, cached.statusLevel,
                          sp.variants.size(), eIdx, (int)sv.entryIndices.size(), sb.entries[poolIdx],
                          cached.glossScrollY, cached.posScrollX, &sb.contexts);
        } else if (popupBundle.active && !popupBundle.variants.empty()) {
            size_t vIdx = std::min(popupBundle.curVariantIdx, popupBundle.variants.size() - 1);
            const PopupVariant &var = popupBundle.variants[vIdx];
            if (var.entries.empty()) return;
            size_t eIdx = std::min(popupBundle.curEntryIdx, var.entries.size() - 1);
            DrawPopupView(targetScreenId, popupBundle.glyphs, popupBundle.sourceScreenId, popupBundle.x, popupBundle.y,
                          popupBundle.footerLookup, popupBundle.footerMode, popupBundle.footerAnki,
                          popupBundle.statusBanner, popupBundle.statusLevel,
                          popupBundle.variants.size(), eIdx, (int)var.entries.size(), var.entries[eIdx],
                          popupBundle.glossScrollY, popupBundle.posScrollX);
        }
    }

    static inline bool IsKanjiCodepoint(uint32_t cp)
    {
        return (cp >= 0x4E00 && cp <= 0x9FFF) ||
               (cp >= 0x3400 && cp <= 0x4DBF) ||
               (cp >= 0xF900 && cp <= 0xFAFF) ||
               (cp == 0x3005);
    }

    static bool TryGetKanjiInText(const GlyphMap &glyphs, uint8_t fontId, const std::string &text,
                                  int startX, int startY, int fontHeight, int popX, int popY,
                                  uint32_t &outCp, int maxRightX = 315)
    {
        if (popY < startY - 2 || popY >= startY + fontHeight + 2) return false;
        int penX = startX;
        for (uint32_t cp : Utf8ToCodepoints(text)) {
            auto it = glyphs.find(GlyphKey(fontId, cp));
            int adv = (it != glyphs.end()) ? (int)it->second.advanceX : ((cp > 0x7E) ? ((fontId == 2) ? 18 : 12) : ((fontId == 2) ? 10 : 7));
            if (popX >= penX - 1 && popX < penX + adv + 1) {
                if (IsKanjiCodepoint(cp)) {
                    outCp = cp;
                    return true;
                }
                return false;
            }
            penX += adv;
            if (penX >= maxRightX) break;
        }
        return false;
    }

    static bool TryGetKanjiAtPopupCoord(int popX, int popY, const GlyphMap &glyphs,
                                        const PopupEntry &res, int glossScrollY, int posScrollX,
                                        uint32_t &outKanji, const std::vector<std::string> *contexts = nullptr)
    {
        // 1. Title (headword + reading) starting at y=36, fontId=2
        PopupTitleLayout tl = ComputePopupTitleLayout(glyphs, res);
        for (size_t i = 0; i < tl.lines.size(); ++i) {
            if (TryGetKanjiInText(glyphs, 2, tl.lines[i], 12, 34 + (int)i * 22, 22, popX, popY, outKanji, 312)) {
                return true;
            }
        }

        // 2. Header label at y=7, fontId=0
        const std::string &ctxStr = (res.contextIdx >= 0 && contexts && (size_t)res.contextIdx < contexts->size())
                                    ? (*contexts)[(size_t)res.contextIdx]
                                    : res.contextText;
        std::string surfInfo;
        if (res.headword == "Selection Translation") {
            surfInfo = "Selection: " + res.matchedSurface;
        } else if (!res.matchedSurface.empty() && res.matchedSurface != res.headword) {
            surfInfo = "Match: " + res.matchedSurface + " -> " + res.headword;
        } else {
            surfInfo = "Text: " + ctxStr;
        }
        std::string tagStr;
        if (res.headword == "Selection Translation") {
            tagStr = "[Translate] ";
        } else if (!res.rankJlpt.empty()) {
            tagStr = "[" + res.rankJlpt + "] ";
        }
        std::string hdrText = tagStr + surfInfo;
        hdrText = TruncateGlyphText(glyphs, 0, hdrText, 300);
        if (TryGetKanjiInText(glyphs, 0, hdrText, 8, 4, 24, popX, popY, outKanji, 310)) {
            return true;
        }

        // 3. POS line at tl.posY, fontId=1
        std::vector<uint32_t> posCps = Utf8ToCodepoints(res.pos);
        int maxPosX = std::max(0, (int)posCps.size() - POS_WINDOW_CHARS);
        int posOff = std::max(0, std::min(posScrollX, maxPosX));
        std::string posSlice = CodepointsToUtf8(posCps, (size_t)posOff, POS_WINDOW_CHARS);
        int posStartX = (posOff > 0) ? 14 : 12;
        int posMaxRight = (posOff < maxPosX) ? 300 : 312;
        if (TryGetKanjiInText(glyphs, 1, posSlice, posStartX, tl.posY - 4, 24, popX, popY, outKanji, posMaxRight)) {
            return true;
        }

        // 4. Vertically scrollable gloss lines starting at tl.glossStartY, fontId=1
        int totalLines = (int)res.wrappedLines.size();
        int maxGlossY = std::max(0, totalLines - tl.visibleGlossLines);
        int gOff = std::max(0, std::min(glossScrollY, maxGlossY));
        int visCount = std::min(tl.visibleGlossLines, totalLines - gOff);
        int yCur = tl.glossStartY;
        for (int i = 0; i < visCount; ++i) {
            const std::string &ln = res.wrappedLines[(size_t)(gOff + i)];
            if (TryGetKanjiInText(glyphs, 1, ln, 12, yCur - 2, 19, popX, popY, outKanji, 309)) {
                return true;
            }
            yCur += 17;
        }

        return false;
    }

    struct BufferState {
        bool hasCursor{false};
        int  curScreen{0};
        int  curX{0}, curY{0};

        bool hasSelBox{false};
        SelBoxState selbox;
    };

    // =========================================================================
    // QR Code Scanning & Parsing
    // =========================================================================

    static bool IsValidPsk(const std::string &psk)
    {
        if (psk.length() < 20 || psk.length() > OCR_PSK_MAX_LEN) return false;
        for (char c : psk) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '=')) {
                return false;
            }
        }
        return true;
    }

    static bool IsValidHost(const std::string &host)
    {
        if (host.empty() || host.length() > 64) return false;
        for (char c : host) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) {
                return false;
            }
        }
        return true;
    }

    static bool ParseKeyValue(const std::string &str, const std::string &key, std::string &out_val)
    {
        size_t pos = str.find(key + "=");
        if (pos == std::string::npos) return false;
        pos += key.length() + 1;
        size_t end = str.find_first_of(";,\r\n& ", pos);
        if (end == std::string::npos) end = str.length();
        out_val = str.substr(pos, end - pos);
        return true;
    }

    static bool ParseJsonValue(const std::string &json, const std::string &key, std::string &out_val)
    {
        std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return false;
        pos = json.find(":", pos);
        if (pos == std::string::npos) return false;
        pos = json.find("\"", pos);
        if (pos == std::string::npos) return false;
        pos++;
        size_t end = json.find("\"", pos);
        if (end == std::string::npos) return false;
        out_val = json.substr(pos, end - pos);
        return true;
    }

    static void OCR_ParseQrPayload(const std::string &payload, std::string &out_psk, std::string &out_lan, std::string &out_wan)
    {
        out_psk.clear();
        out_lan.clear();
        out_wan.clear();

        // 1. Format: 3DSOCR:lan=<ip>:<port>;wan=<ip>:<port>;psk=<base64>
        if (payload.compare(0, 7, "3DSOCR:") == 0) {
            std::string sub = payload.substr(7);
            ParseKeyValue(sub, "lan", out_lan);
            ParseKeyValue(sub, "wan", out_wan);
            ParseKeyValue(sub, "psk", out_psk);
            return;
        }

        // 2. Format: JSON {"lan":"...", "wan":"...", "psk":"..."}
        if (!payload.empty() && payload[0] == '{') {
            ParseJsonValue(payload, "lan", out_lan);
            ParseJsonValue(payload, "wan", out_wan);
            ParseJsonValue(payload, "psk", out_psk);
            return;
        }

        // 3. Format: ocr://lan_ip:port/wan_ip:port?psk=<base64>
        if (payload.compare(0, 6, "ocr://") == 0) {
            std::string sub = payload.substr(6);
            size_t slash = sub.find('/');
            size_t qmark = sub.find("?psk=");
            if (slash != std::string::npos) {
                out_lan = sub.substr(0, slash);
                if (qmark != std::string::npos && qmark > slash) {
                    out_wan = sub.substr(slash + 1, qmark - (slash + 1));
                    out_psk = sub.substr(qmark + 5);
                } else {
                    out_wan = sub.substr(slash + 1);
                }
            }
            return;
        }

        // 4. Fallback: Assume the whole payload is a raw PSK if sufficiently long
        if (payload.length() >= 20 && payload.find(' ') == std::string::npos) {
            out_psk = payload;
        }
    }

    static int recv_encrypted_frame(int sock, sha256_stream_t *rx, void *buf, size_t len);
    static int OCR_ConnectServer(std::string *out_diag);
    static bool OCR_PerformHandshake(int sock, sha256_stream_t *tx, sha256_stream_t *rx);

    static void OCR_GetDeviceIdentity(std::string &outId, std::string &outName, std::string &outModel)
    {
        outId.clear();
        outName.clear();
        outModel.clear();

        if (R_SUCCEEDED(cfguInit())) {
            u64 consoleHash = 0;
            if (R_SUCCEEDED(CFGU_GenHashConsoleUnique(0x3D50C801, &consoleHash))) {
                char idBuf[20];
                snprintf(idBuf, sizeof(idBuf), "%016llX", (unsigned long long)consoleHash);
                outId = idBuf;
            }

            u16 rawName[14];
            memset(rawName, 0, sizeof(rawName));
            if (R_SUCCEEDED(CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, rawName))) {
                for (int i = 0; i < 10; ++i) {
                    u16 ch = rawName[i];
                    if (ch == 0) break;
                    if (ch == '\r' || ch == '\n') continue;
                    if (ch < 0x80) {
                        outName.push_back((char)ch);
                    } else if (ch < 0x800) {
                        outName.push_back((char)(0xC0 | (ch >> 6)));
                        outName.push_back((char)(0x80 | (ch & 0x3F)));
                    } else {
                        outName.push_back((char)(0xE0 | (ch >> 12)));
                        outName.push_back((char)(0x80 | ((ch >> 6) & 0x3F)));
                        outName.push_back((char)(0x80 | (ch & 0x3F)));
                    }
                }
            }

            u8 model = 0xFF;
            if (R_SUCCEEDED(CFGU_GetSystemModel(&model))) {
                switch (model) {
                    case 0: outModel = "Old 3DS"; break;
                    case 1: outModel = "Old 3DS XL"; break;
                    case 2: outModel = "New 3DS"; break;
                    case 3: outModel = "2DS"; break;
                    case 4: outModel = "New 3DS XL"; break;
                    case 5: outModel = "New 2DS XL"; break;
                    default: outModel = "3DS"; break;
                }
            }
            cfguExit();
        }

        if (outId.empty()) {
            char idBuf[64];
            memset(idBuf, 0, sizeof(idBuf));
            File file;
            if (File::Open(file, OCR_DEVICE_ID_SD_PATH, File::READ) == File::SUCCESS) {
                u64 sz = file.GetSize();
                if (sz > 0 && sz < sizeof(idBuf)) {
                    file.Read(idBuf, (u32)sz);
                }
                file.Close();
            } else {
                FILE *fp = fopen(OCR_DEVICE_ID_SD_PATH, "r");
                if (fp) {
                    fread(idBuf, 1, sizeof(idBuf) - 1, fp);
                    fclose(fp);
                }
            }
            outId = TrimWhitespace(std::string(idBuf));
            if (outId.empty()) {
                u8 rnd[8];
                if (R_SUCCEEDED(psInit())) {
                    PS_GenerateRandomBytes(rnd, sizeof(rnd));
                    psExit();
                } else {
                    u64 tk = svcGetSystemTick();
                    memcpy(rnd, &tk, sizeof(rnd));
                }
                char genBuf[20];
                snprintf(genBuf, sizeof(genBuf), "%02X%02X%02X%02X%02X%02X%02X%02X",
                         rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);
                outId = genBuf;
                File wf;
                if (File::Open(wf, OCR_DEVICE_ID_SD_PATH, File::WRITE | File::CREATE | File::TRUNCATE) == File::SUCCESS) {
                    wf.Write(outId.c_str(), outId.size());
                    wf.Close();
                } else {
                    FILE *fp = fopen(OCR_DEVICE_ID_SD_PATH, "w");
                    if (fp) {
                        fputs(outId.c_str(), fp);
                        fclose(fp);
                    }
                }
            }
        }

        outName = TrimWhitespace(outName);
        if (outName.empty()) outName = "3DS";
        if (outModel.empty()) outModel = "3DS";
    }

    static std::string OCR_BuildDeviceInfoPayload(std::string *outName = nullptr)
    {
        std::string devId, devName, devModel;
        OCR_GetDeviceIdentity(devId, devName, devModel);
        if (outName) *outName = devName;
        std::string payload = devId + "\n" + devName + "\n" + devModel;
        if (payload.size() > 256) payload.resize(256);
        return payload;
    }

    static bool OCR_SendDeviceInfo(int sock, sha256_stream_t *tx)
    {
        if (sock < 0 || tx == nullptr) return false;
        std::string payload = OCR_BuildDeviceInfoPayload();
        uint16_t len = (uint16_t)payload.size();
        uint8_t hdr[5] = {
            CMD_SET_DEVICE_INFO,
            (uint8_t)((len >> 8) & 0xFF),
            (uint8_t)(len & 0xFF),
            0,
            0
        };
        if (send_encrypted_frame(sock, tx, hdr, sizeof(hdr)) < 0) return false;
        return send_encrypted_frame(sock, tx, payload.data(), len) >= 0;
    }

    static bool OCR_PingServerPairing(std::string &outName)
    {
        std::string payload = OCR_BuildDeviceInfoPayload(&outName);
        int sock = OCR_ConnectServer(nullptr);
        if (sock < 0) return false;

        sha256_stream_t tx, rx;
        bool ok = OCR_PerformHandshake(sock, &tx, &rx);
        if (ok) {
            const uint8_t magic[8] = { 'P', 'A', 'I', 'R', 'P', 'I', 'N', 'G' };
            uint16_t netLen = htons((uint16_t)payload.size());
            uint8_t ack = 0;
            ok = (send_encrypted_frame(sock, &tx, magic, sizeof(magic)) >= 0) &&
                 (send_encrypted_frame(sock, &tx, &netLen, sizeof(netLen)) >= 0) &&
                 (send_encrypted_frame(sock, &tx, payload.data(), payload.size()) >= 0) &&
                 (recv_encrypted_frame(sock, &rx, &ack, 1) >= 0) &&
                 (ack == 1);
        }
        closesocket(sock);
        crypto_wipe(&tx, sizeof(tx));
        crypto_wipe(&rx, sizeof(rx));
        return ok;
    }

    static void OCR_ApplyScannedPayload(std::string &payload)
    {
        std::string psk, lan, wan;
        OCR_ParseQrPayload(payload, psk, lan, wan);

        if (!psk.empty()) {
            if (IsValidPsk(psk)) {
                OCR_SavePskToSd(psk);
            } else {
                OSD::Notify("Security: Scanned PSK invalid, discarded!", Color::Red);
                return;
            }
        }
        if (!lan.empty()) {
            std::string host;
            uint16_t port = 8888;
            SplitHostPort(lan, host, port, 8888);
            if (IsValidHost(host)) {
                s_serverLanHost = host;
                s_serverLanPort = port;
            }
        }
        if (!wan.empty()) {
            std::string host;
            uint16_t port = 8888;
            SplitHostPort(wan, host, port, 8888);
            if (IsValidHost(host)) {
                s_serverWanHost = host;
                s_serverWanPort = port;
            }
        }
        OCR_SaveConfig(s_serverLanHost + ":" + std::to_string(s_serverLanPort),
                       s_serverWanHost + ":" + std::to_string(s_serverWanPort));

        if (!psk.empty()) {
            std::fill(psk.begin(), psk.end(), '\0');
            psk.clear();
        }
        if (!payload.empty()) {
            std::fill(payload.begin(), payload.end(), '\0');
            payload.clear();
        }

        OSD::Notify("QR Code Scanned! Config Saved.", Color::Green);
        OSD::Notify("LAN: " + s_serverLanHost + ":" + std::to_string(s_serverLanPort), Color::Cyan);

        std::string devName;
        if (OCR_PingServerPairing(devName)) {
            OSD::Notify("Paired with server as " + devName + "!", Color::Green);
        } else {
            OSD::Notify("QR saved (server offline - will register on connect)", Color::Yellow);
        }
    }

    bool OCR_ScanQrCode(void)
    {
        if (OCR_ShouldAbortForSleep()) {
            return false;
        }
        bool wasPaused = Process::IsPaused();
        ScopedPausedOverlay pauseGuard(!wasPaused);
        if (!wasPaused) {
            Process::Pause();
            Sleep(Milliseconds(20));
        }

        Result res = camInit();
        if (R_FAILED(res)) {
            OSD::Notify("Camera Error: camInit failed", Color::Red);
            if (!wasPaused) Process::Play();
            return false;
        }

        CAMU_SetSize(SELECT_OUT1, SIZE_CTR_BOTTOM_LCD, CONTEXT_A);
        CAMU_SetOutputFormat(SELECT_OUT1, OUTPUT_RGB_565, CONTEXT_A);
        CAMU_SetFrameRate(SELECT_OUT1, FRAME_RATE_30);
        CAMU_SetNoiseFilter(SELECT_OUT1, true);
        CAMU_SetAutoExposure(SELECT_OUT1, true);
        CAMU_SetAutoWhiteBalance(SELECT_OUT1, true);
        CAMU_SetTrimming(PORT_CAM1, false);

        u32 bufSize = 0;
        CAMU_GetMaxBytes(&bufSize, 320, 240);
        CAMU_SetTransferBytes(PORT_CAM1, bufSize, 320, 240);

        CAMU_Activate(SELECT_OUT1);

        // 4KB page-aligned buffer from CTRPF heap for camera DMA reception
        u16 *camBuf = (u16 *)memalign(0x1000, 320 * 240 * sizeof(u16));
        if (!camBuf) {
            OSD::Notify("Camera Error: memalign failed", Color::Red);
            CAMU_Activate(SELECT_NONE);
            camExit();
            if (!wasPaused) Process::Play();
            return false;
        }

        struct quirc *q = quirc_new();
        struct quirc_code *code = (struct quirc_code *)malloc(sizeof(struct quirc_code));
        struct quirc_data *data = (struct quirc_data *)malloc(sizeof(struct quirc_data));
        if (!q || !code || !data || quirc_resize(q, 320, 240) < 0) {
            if (q) quirc_destroy(q);
            if (code) free(code);
            if (data) free(data);
            free(camBuf);
            CAMU_Activate(SELECT_NONE);
            camExit();
            if (!wasPaused) Process::Play();
            return false;
        }

        bool success = false;
        std::string scannedPayload;

        while (true) {
            if (OCR_ShouldAbortForSleep()) {
                break;
            }
            Controller::Update();
            if (Controller::IsKeyPressed(Key::B)) {
                break;
            }

            CAMU_ClearBuffer(PORT_CAM1);
            CAMU_StartCapture(PORT_CAM1);

            Handle camEvent = 0;
            Result rxRes = CAMU_SetReceiving(&camEvent, camBuf, PORT_CAM1, 320 * 240 * sizeof(u16), (s16)bufSize);
            if (R_SUCCEEDED(rxRes)) {
                Result waitRes = -1;
                for (int s = 0; s < 6; ++s) {
                    if (OCR_ShouldAbortForSleep()) break;
                    waitRes = svcWaitSynchronization(camEvent, 50000000ULL);
                    if (R_SUCCEEDED(waitRes)) break;
                }
                svcCloseHandle(camEvent);
                CAMU_StopCapture(PORT_CAM1);
                if (OCR_ShouldAbortForSleep()) {
                    break;
                }

                const Screen &topScreen = OSD::GetTopScreen();
                const Screen &botScreen = OSD::GetBottomScreen();

                // Draw instructions on Top Screen every frame for double buffering
                topScreen.DrawRect(0, 0, 400, 240, Color(15, 18, 25));
                topScreen.Draw("========================================", 20, 20, Color(0, 229, 255), Color(15, 18, 25));
                topScreen.Draw("     3DS OCR Server QR Code Scanner", 40, 42, Color::White, Color(15, 18, 25));
                topScreen.Draw("========================================", 20, 64, Color(0, 229, 255), Color(15, 18, 25));
                topScreen.Draw("Point outer camera at QR code on PC screen.", 20, 95, Color::Yellow, Color(15, 18, 25));
                topScreen.Draw("Hold 3DS steady ~25 cm from display.", 20, 118, Color::Silver, Color(15, 18, 25));
                topScreen.Draw("Align QR code inside green viewfinder brackets.", 20, 140, Color::Silver, Color(15, 18, 25));
                topScreen.Draw("Press [B] to Cancel", 130, 190, Color::Red, Color(15, 18, 25));

                if (R_SUCCEEDED(waitRes)) {
                    GSPGPU_InvalidateDataCache(camBuf, 320 * 240 * sizeof(u16));
                    int w, h;
                    uint8_t *gray = quirc_begin(q, &w, &h);

                    // Draw live video feed to bottom screen and feed grayscale to quirc
                    for (int y = 0; y < 240; ++y) {
                        for (int x = 0; x < 320; ++x) {
                            u16 px = camBuf[y * 320 + x];
                            u8 r = (px >> 11) & 0x1F; r = (r << 3) | (r >> 2);
                            u8 g = (px >> 5)  & 0x3F; g = (g << 2) | (g >> 4);
                            u8 b =  px        & 0x1F; b = (b << 3) | (b >> 2);
                            botScreen.DrawPixel(x, y, Color(r, g, b));
                            gray[y * 320 + x] = (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
                        }
                    }

                    // Draw green viewfinder reticle corners on bottom screen
                    Color reticleColor = Color(0, 255, 64);
                    botScreen.DrawRect(90, 50, 25, 3, reticleColor);
                    botScreen.DrawRect(90, 50, 3, 25, reticleColor);
                    botScreen.DrawRect(205, 50, 25, 3, reticleColor);
                    botScreen.DrawRect(227, 50, 3, 25, reticleColor);
                    botScreen.DrawRect(90, 187, 25, 3, reticleColor);
                    botScreen.DrawRect(90, 165, 3, 25, reticleColor);
                    botScreen.DrawRect(205, 187, 25, 3, reticleColor);
                    botScreen.DrawRect(227, 165, 3, 25, reticleColor);

                    quirc_end(q);

                    int count = quirc_count(q);
                    for (int i = 0; i < count; ++i) {
                        quirc_extract(q, i, code);
                        if (quirc_decode(code, data) == QUIRC_SUCCESS) {
                            scannedPayload = std::string((char *)data->payload, data->payload_len);
                            crypto_wipe(data->payload, sizeof(data->payload));
                            success = true;
                            break;
                        }
                    }
                } else {
                    botScreen.DrawRect(0, 0, 320, 240, Color(20, 20, 25));
                    botScreen.Draw("Initializing camera sensor...", 55, 110, Color::Yellow, Color(20, 20, 25));
                }

                if (OCR_ShouldAbortForSleep()) {
                    break;
                }
                // Swap buffers to display frame
                OSD::SwapBuffers();

                if (success) {
                    break;
                }
            } else {
                CAMU_StopCapture(PORT_CAM1);
                Sleep(Milliseconds(10));
            }
        }

        // Halt capture and clear DMA buffers before releasing memory
        CAMU_StopCapture(PORT_CAM1);
        CAMU_ClearBuffer(PORT_CAM1);
        CAMU_Activate(SELECT_NONE);
        camExit();
        quirc_destroy(q);
        free(code);
        free(data);
        crypto_wipe(camBuf, 320 * 240 * sizeof(u16));
        free(camBuf);

        if (!wasPaused) {
            Process::Play();
        }

        if (success) {
            OCR_ApplyScannedPayload(scannedPayload);
            return true;
        }
        return false;
    }

    // =========================================================================
    // OCR Session Main Loop
    // =========================================================================

    static void DrawConnectionDialog(const std::string &title, const std::string &diag)
    {
        const Screen &bot = OSD::GetBottomScreen();
        // Dark background
        bot.DrawRect(0, 0, 320, 240, Color(20, 22, 28));

        // Red top banner
        bot.DrawRect(0, 0, 320, 22, Color(175, 35, 35));
        bot.Draw(title.c_str(), 10, 4, Color::White, Color(175, 35, 35));

        // Draw diagnostic message lines
        int y = 28;
        size_t start = 0;
        while (start < diag.length() && y < 185) {
            size_t end = diag.find('\n', start);
            if (end == std::string::npos) end = diag.length();
            std::string line = diag.substr(start, end - start);
            start = end + 1;
            if (!line.empty()) {
                Color c = Color::White;
                if (line.find("Warning:") != std::string::npos || line.find("Failed") != std::string::npos ||
                    line.find("ERR") != std::string::npos || line.find("failed") != std::string::npos) {
                    c = Color(255, 120, 120);
                } else if (line.find("3DS IP:") != std::string::npos) {
                    c = Color(0, 229, 255);
                } else if (line.find("Handshake") != std::string::npos) {
                    c = Color::Yellow;
                }
                bot.Draw(line.c_str(), 10, y, c, Color(20, 22, 28));
                y += 14;
            }
        }

        // Action controls footer
        bot.DrawRect(0, 192, 320, 48, Color(12, 14, 20));
        bot.DrawRect(0, 192, 320, 1, Color(60, 65, 80));
        bot.Draw("[Start] Scan QR Code (Pair with PC)", 10, 197, Color(0, 255, 128), Color(12, 14, 20));
        bot.Draw("[A] Retry Connection   [B] Cancel / Exit", 10, 217, Color::Yellow, Color(12, 14, 20));
    }

    void OCR_LaunchSession(void)
    {
        if (OCR_ShouldAbortForSleep()) {
            return;
        }

        // Allocate screen buffers dynamically for the session (popup buffer is allocated on-demand)
        if (!s_topBuffer.Alloc(0x07250000, TOP_FB_565_SIZE) ||
            !s_botBuffer.Alloc(0x072A0000, BOT_FB_565_SIZE)) {
            OSD::Notify("OCR Error: Failed to allocate memory", Color::Red);
            OCR_FreeSessionBuffers();
            return;
        }

        // 1. Pause game process
        ScopedPausedOverlay pauseGuard;
        Process::Pause();
        Sleep(Milliseconds(20));

        // Wait until trigger buttons are released
        while (Controller::IsKeysDown(g_ocrKeys.activation) && !OCR_ShouldAbortForSleep()) {
            Controller::Update();
            Sleep(Milliseconds(16));
        }
        if (OCR_ShouldAbortForSleep()) {
            OCR_FreeSessionBuffers();
            Process::Play();
            return;
        }

        // 2. Snapshot game screens
        const Screen &topScreen = OSD::GetTopScreen();
        const Screen &botScreen = OSD::GetBottomScreen();
        CaptureScreen(topScreen, TOP_WIDTH, TOP_HEIGHT, s_top_565);
        CaptureScreen(botScreen, BOT_WIDTH, BOT_HEIGHT, s_bot_565);

        // Ensure server endpoints and PSK are configured on SD card
        OCR_LoadConfig();
        OCR_LoadAnkiCredentials();
        char testPsk[OCR_PSK_MAX_LEN];
        size_t pskLen = OCR_LoadPskFromSd(testPsk, sizeof(testPsk));
        crypto_wipe(testPsk, sizeof(testPsk));

        if ((s_serverLanHost.empty() && s_serverWanHost.empty()) || pskLen == 0) {
            OSD::Notify("OCR Not Paired! Opening QR Scanner...", Color::Yellow);
            if (OCR_ScanQrCode()) {
                if (OCR_ShouldAbortForSleep()) {
                    OCR_FreeSessionBuffers();
                    Process::Play();
                    return;
                }
                for (int b = 0; b < 2; ++b) {
                    RestoreRect(OSD::GetTopScreen(), 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                    RestoreRect(OSD::GetBottomScreen(), 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                    OSD::SwapBuffers();
                }
            } else {
                OCR_FreeSessionBuffers();
                Process::Play();
                return;
            }
        }

        // 3. Connect TCP socket & mutual X25519 ECDH + SHA256-CTR + HMAC handshake
        int sock = -1;
        sha256_stream_t tx_stream, rx_stream;

        while (true) {
            if (OCR_ShouldAbortForSleep()) {
                OCR_FreeSessionBuffers();
                Process::Play();
                return;
            }

            // Display "Connecting..." status bar on bottom screen on both buffers
            for (int b = 0; b < 2; ++b) {
                const Screen &top = OSD::GetTopScreen();
                const Screen &bot = OSD::GetBottomScreen();
                RestoreRect(top, 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                RestoreRect(bot, 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                bot.DrawRect(0, 224, 320, 16, Color(20, 20, 30));
                bot.Draw("Connecting to OCR Server...", 10, 227, Color(255, 230, 0), Color::Black);
                OSD::SwapBuffers();
            }

            std::string diag;
            sock = OCR_ConnectServer(&diag);
            if (OCR_ShouldAbortForSleep()) {
                if (sock >= 0) {
                    s_activeSessionSock = -1;
                    s_activeSessionTx = nullptr;
                    closesocket(sock);
                    sock = -1;
                }
                OCR_FreeSessionBuffers();
                Process::Play();
                return;
            }

            bool handshakeOk = false;
            if (sock >= 0) {
                handshakeOk = OCR_PerformHandshake(sock, &tx_stream, &rx_stream);
                if (!handshakeOk) {
                    s_activeSessionSock = -1;
                    s_activeSessionTx = nullptr;

                    closesocket(sock);
                    sock = -1;
                    diag += "Handshake Failed: Invalid PSK or mismatch.\n";
                }
                if (OCR_ShouldAbortForSleep()) {
                    if (sock >= 0) {
                        s_activeSessionSock = -1;
                        s_activeSessionTx = nullptr;
                        closesocket(sock);
                        sock = -1;
                    }
                    OCR_FreeSessionBuffers();
                    Process::Play();
                    return;
                }
            }

            if (sock >= 0 && handshakeOk) {
                // Connected and authenticated successfully!
                break;
            }

            // Connection or Handshake Failed: Show interactive failure screen
            bool retry = false;
            while (true) {
                if (OCR_ShouldAbortForSleep()) {
                    OCR_FreeSessionBuffers();
                    Process::Play();
                    return;
                }
                Controller::Update();
                u32 kDown = Controller::GetKeysDown();

                if (kDown & Key::Start) {
                    if (OCR_ScanQrCode()) {
                        OCR_LoadConfig();
                        retry = true;
                        break;
                    }
                    if (OCR_ShouldAbortForSleep()) {
                        OCR_FreeSessionBuffers();
                        Process::Play();
                        return;
                    }
                } else if (kDown & Key::A) {
                    OCR_LoadConfig();
                    retry = true;
                    break;
                } else if (kDown & Key::B) {
                    for (int b = 0; b < 2; ++b) {
                        RestoreRect(OSD::GetBottomScreen(), 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                        OSD::SwapBuffers();
                    }
                    OCR_FreeSessionBuffers();
                    Process::Play();
                    return;
                }

                // Render failure screen on bottom screen
                const Screen &top = OSD::GetTopScreen();
                RestoreRect(top, 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                DrawConnectionDialog("OCR Server Connection Failed", diag);
                OSD::SwapBuffers();
                Sleep(Milliseconds(16));
            }

            if (!retry) {
                OCR_FreeSessionBuffers();
                Process::Play();
                return;
            }
        }

        // Update status: Uploading screen capture
        for (int b = 0; b < 2; ++b) {
            const Screen &bot = OSD::GetBottomScreen();
            bot.DrawRect(0, 224, 320, 16, Color(20, 20, 30));
            bot.Draw("Uploading Screen Capture...", 10, 227, Color(0, 229, 255), Color::Black);
            OSD::SwapBuffers();
        }

        // 4. Send Active Title ID (8 bytes) + Encrypted Screen Framebuffers
        u64 titleId = Process::GetTitleID();
        uint8_t titleIdBuf[8];
        store_be64(titleIdBuf, titleId);
        if (send_encrypted_frame(sock, &tx_stream, titleIdBuf, sizeof(titleIdBuf)) < 0 ||
            send_encrypted_rgb565_as_rgb24(sock, &tx_stream, s_top_565, TOP_WIDTH * TOP_HEIGHT) < 0 ||
            send_encrypted_rgb565_as_rgb24(sock, &tx_stream, s_bot_565, BOT_WIDTH * BOT_HEIGHT) < 0) {
            if (!OCR_ShouldAbortForSleep()) {
                OSD::Notify("OCR Error: Screen upload failed", Color::Red);
            }
            s_activeSessionSock = -1;
            s_activeSessionTx = nullptr;

            closesocket(sock);
            if (!OCR_ShouldAbortForSleep()) {
                for (int b = 0; b < 2; ++b) {
                    RestoreRect(OSD::GetBottomScreen(), 0, 224, 320, 16, s_bot_565, BOT_WIDTH, 240);
                    OSD::SwapBuffers();
                }
            }
            OCR_FreeSessionBuffers();
            Process::Play();
            return;
        }

        // Send active key labels packet so server popup footer displays current keybindings
        uint8_t keyLabelsPkt[5] = {
            CMD_SET_KEY_LABELS,
            OCR_KeyToCharCode(g_ocrKeys.lookup),
            OCR_KeyToCharCode(g_ocrKeys.modeToggle),
            OCR_KeyToCharCode(g_ocrKeys.addAnki),
            OCR_KeyToCharCode(g_ocrKeys.switchScreen)
        };
        send_encrypted_frame(sock, &tx_stream, keyLabelsPkt, sizeof(keyLabelsPkt));

        // Advertise client-side POPUP_BUNDLE (bit 0) + SCREEN_BUNDLE (bit 1) capability
        uint8_t capsPkt[5] = { CMD_SET_CLIENT_CAPS, 0, 3, 0, 1 };
        send_encrypted_frame(sock, &tx_stream, capsPkt, sizeof(capsPkt));

        // Send per-user AnkiWeb credentials if configured on SD card
        if (!s_ankiUser.empty() && !s_ankiPass.empty()) {
            OCR_SendAnkiCredentials(sock, &tx_stream);
        }

        // Send 3DS console identity so server records/updates this device
        OCR_SendDeviceInfo(sock, &tx_stream);

        // Clear status bar region on both buffers
        for (int b = 0; b < 2; ++b) {
            RestoreRect(OSD::GetBottomScreen(), 0, 224, 320, 16, s_bot_565, BOT_WIDTH, 240);
            OSD::SwapBuffers();
        }

        // 5. Initialize Interactive Session State & Double-Buffering
        OCR_InitCStick();
        int curScreen = SCREEN_ID_TOP;
        int curX = 200, curY = 120;
        float cstickAccX = 0.0f, cstickAccY = 0.0f;
        bool translateMode = false;
        bool hasPopup = false;
        int popupTargetScreen = SCREEN_ID_BOTTOM;
        bool hasKanjiWindow = false;
        int kanjiTargetScreen = SCREEN_ID_TOP;
        s_kanjiScrollY = 0;
        s_kanjiMaxScrollY = 0;
        int kanjiScrollY = 0;
        uint32_t lastKanjiCp = 0;
        PopupBundle popupBundle;
        ScreenBundle screenBundles[2];
        CachedPopupState cachedPopup;
        bool popupDirty[2] = {false, false};
        int dpadHoldTicks = 0;
        SelBoxState selbox;

        BufferState bufStates[2];
        int activeBuf = 0;

        auto getScreen565 = [&](int scr) -> const u16* {
            return (scr == SCREEN_ID_TOP) ? s_top_565 : s_bot_565;
        };

        auto isClientBundlePopup = [&](void) -> bool {
            return cachedPopup.active || popupBundle.active;
        };

        auto PresentFrame = [&](void) {
            // 1. Restore dirty areas on THIS buffer
            bool bundleActive = isClientBundlePopup();
            bool cursorWasOnBundle = (hasPopup && bundleActive && bufStates[activeBuf].hasCursor && bufStates[activeBuf].curScreen == popupTargetScreen);
            bool selWasOnBundle = (hasPopup && bundleActive && bufStates[activeBuf].hasSelBox && bufStates[activeBuf].selbox.screenId == popupTargetScreen);
            if (bufStates[activeBuf].hasCursor) {
                RestoreCursorArea(bufStates[activeBuf].curScreen,
                                  bufStates[activeBuf].curX,
                                  bufStates[activeBuf].curY,
                                  hasPopup, popupTargetScreen,
                                  hasKanjiWindow, kanjiTargetScreen);
                bufStates[activeBuf].hasCursor = false;
            }
            if (bufStates[activeBuf].hasSelBox) {
                RestoreSelBoxArea(bufStates[activeBuf].selbox, hasPopup, popupTargetScreen,
                                  hasKanjiWindow, kanjiTargetScreen);
                bufStates[activeBuf].hasSelBox = false;
            }
            if (hasPopup && bundleActive && (popupDirty[activeBuf] || cursorWasOnBundle || selWasOnBundle)) {
                DrawActivePopup(popupTargetScreen, popupBundle, cachedPopup, screenBundles);
                popupDirty[activeBuf] = false;
            }

            // 2. Draw active selbox on THIS buffer
            if (selbox.active) {
                SelBoxState sbCopy = selbox;
                DrawSelBox(sbCopy);
                bufStates[activeBuf].hasSelBox = true;
                bufStates[activeBuf].selbox = sbCopy;
            }

            // 3. Draw cursor on THIS buffer
            DrawCursor(curScreen, curX, curY, translateMode, selbox.active);
            bufStates[activeBuf].hasCursor = true;
            bufStates[activeBuf].curScreen = curScreen;
            bufStates[activeBuf].curX = curX;
            bufStates[activeBuf].curY = curY;

            // 4. Swap buffers and advance buffer index
            OSD::SwapBuffers();
            activeBuf = 1 - activeBuf;
        };

        auto SyncBothBuffers = [&](void) {
            for (int b = 0; b < 2; ++b) {
                PresentFrame();
            }
        };

        auto RedrawBundlePopup = [&](void) {
            popupDirty[0] = true;
            popupDirty[1] = true;
            SyncBothBuffers();
        };

        auto DismissKanjiWindow = [&](void) {
            if (hasKanjiWindow) {
                hasKanjiWindow = false;
                OCR_FreeKanjiBuffer();
                for (int b = 0; b < 2; ++b) {
                    const Screen &scr = (kanjiTargetScreen == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
                    int scrW = (kanjiTargetScreen == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
                    RestoreRect(scr, 0, 0, scrW, 240, getScreen565(kanjiTargetScreen), scrW, 240);
                    PresentFrame();
                }
            }
        };

        bool serverDisconnected = false;
        auto ShowOfflineStatus = [&](const char *msg) {
            if (cachedPopup.active) {
                cachedPopup.statusBanner = msg;
                cachedPopup.statusLevel = 2;
                RedrawBundlePopup();
            } else if (popupBundle.active) {
                popupBundle.statusBanner = msg;
                popupBundle.statusLevel = 2;
                RedrawBundlePopup();
            }
        };
        auto MarkDisconnected = [&](bool showBanner = false) {
            if (sock >= 0) {
                s_activeSessionSock = -1;
                s_activeSessionTx = nullptr;
                closesocket(sock);
                sock = -1;
            }
            if (!serverDisconnected) {
                serverDisconnected = true;
                if (!OCR_ShouldAbortForSleep()) {
                    ShowOfflineStatus("Server disconnected (cached lookups still work)");
                }
            } else if (showBanner && !OCR_ShouldAbortForSleep()) {
                ShowOfflineStatus("Offline: server unavailable");
            }
        };
        auto TryReconnectSession = [&](void) -> bool {
            if (OCR_ShouldAbortForSleep()) return false;
            if (sock >= 0) return true;

            bool drewBottomBar = false;
            if (cachedPopup.active || popupBundle.active) {
                ShowOfflineStatus("Reconnecting to OCR server...");
            } else if (!hasPopup) {
                for (int b = 0; b < 2; ++b) {
                    const Screen &bot = OSD::GetBottomScreen();
                    bot.DrawRect(0, 224, 320, 16, Color(20, 20, 30));
                    bot.Draw("Reconnecting to OCR Server...", 10, 227, Color(255, 230, 0), Color::Black);
                    OSD::SwapBuffers();
                }
                drewBottomBar = true;
            }

            auto RestoreBottomBarIfNeeded = [&](void) {
                if (drewBottomBar && !OCR_ShouldAbortForSleep()) {
                    for (int b = 0; b < 2; ++b) {
                        RestoreRect(OSD::GetBottomScreen(), 0, 224, 320, 16, s_bot_565, BOT_WIDTH, 240);
                        PresentFrame();
                    }
                }
            };

            std::string diag;
            int newSock = OCR_ConnectServer(&diag);
            if (OCR_ShouldAbortForSleep() || newSock < 0) {
                if (newSock >= 0) closesocket(newSock);
                RestoreBottomBarIfNeeded();
                ShowOfflineStatus("Offline: reconnect failed");
                return false;
            }

            sha256_stream_t newTx, newRx;
            if (!OCR_PerformHandshake(newSock, &newTx, &newRx)) {
                closesocket(newSock);
                crypto_wipe(&newTx, sizeof(newTx));
                crypto_wipe(&newRx, sizeof(newRx));
                RestoreBottomBarIfNeeded();
                ShowOfflineStatus("Offline: handshake failed");
                return false;
            }

            u64 reTitleId = Process::GetTitleID();
            uint8_t reTitleBuf[8];
            store_be64(reTitleBuf, reTitleId);
            uint8_t reKeyLabels[5] = {
                CMD_SET_KEY_LABELS,
                OCR_KeyToCharCode(g_ocrKeys.lookup),
                OCR_KeyToCharCode(g_ocrKeys.modeToggle),
                OCR_KeyToCharCode(g_ocrKeys.addAnki),
                OCR_KeyToCharCode(g_ocrKeys.switchScreen)
            };
            uint8_t reCaps[5] = { CMD_SET_CLIENT_CAPS, 0, 3, 0, 1 };

            bool uploadOk =
                send_encrypted_frame(newSock, &newTx, reTitleBuf, sizeof(reTitleBuf)) >= 0 &&
                send_encrypted_rgb565_as_rgb24(newSock, &newTx, s_top_565, TOP_WIDTH * TOP_HEIGHT) >= 0 &&
                send_encrypted_rgb565_as_rgb24(newSock, &newTx, s_bot_565, BOT_WIDTH * BOT_HEIGHT) >= 0 &&
                send_encrypted_frame(newSock, &newTx, reKeyLabels, sizeof(reKeyLabels)) >= 0 &&
                send_encrypted_frame(newSock, &newTx, reCaps, sizeof(reCaps)) >= 0;

            if (uploadOk && !s_ankiUser.empty() && !s_ankiPass.empty()) {
                if (!OCR_SendAnkiCredentials(newSock, &newTx)) uploadOk = false;
            }
            if (uploadOk) {
                if (!OCR_SendDeviceInfo(newSock, &newTx)) uploadOk = false;
            }
            if (uploadOk && translateMode) {
                uint8_t modePacket[5] = { CMD_SET_MODE, 1, 0, 0, 0 };
                if (send_encrypted_frame(newSock, &newTx, modePacket, sizeof(modePacket)) < 0) uploadOk = false;
            }

            if (!uploadOk || OCR_ShouldAbortForSleep()) {
                closesocket(newSock);
                crypto_wipe(&newTx, sizeof(newTx));
                crypto_wipe(&newRx, sizeof(newRx));
                RestoreBottomBarIfNeeded();
                ShowOfflineStatus("Offline: reconnect upload failed");
                return false;
            }

            crypto_wipe(&tx_stream, sizeof(tx_stream));
            crypto_wipe(&rx_stream, sizeof(rx_stream));
            tx_stream = newTx;
            rx_stream = newRx;
            crypto_wipe(&newTx, sizeof(newTx));
            crypto_wipe(&newRx, sizeof(newRx));
            sock = newSock;
            s_activeSessionSock = sock;
            s_activeSessionTx = &tx_stream;
            serverDisconnected = false;

            RestoreBottomBarIfNeeded();
            ShowOfflineStatus("Reconnected to OCR server!");
            return true;
        };

        auto SendSessionPacket = [&](const void *buf, size_t len, bool showBannerOnOffline = true) -> bool {
            if (OCR_ShouldAbortForSleep()) return false;
            if (sock < 0) {
                if (!TryReconnectSession()) {
                    if (showBannerOnOffline && !OCR_ShouldAbortForSleep()) {
                        ShowOfflineStatus("Offline: server unavailable");
                    }
                    return false;
                }
            }
            if (send_encrypted_frame(sock, &tx_stream, buf, len) < 0) {
                MarkDisconnected(false);
                if (!OCR_ShouldAbortForSleep() && TryReconnectSession()) {
                    if (send_encrypted_frame(sock, &tx_stream, buf, len) >= 0) {
                        return true;
                    }
                    MarkDisconnected(showBannerOnOffline);
                } else if (showBannerOnOffline && !OCR_ShouldAbortForSleep()) {
                    ShowOfflineStatus("Offline: server unavailable");
                }
                return false;
            }
            return true;
        };

        auto SendKanjiLookup = [&](int scrollY) -> bool {
            uint8_t targetKanjiScr = (popupTargetScreen == SCREEN_ID_TOP) ? SCREEN_ID_BOTTOM : SCREEN_ID_TOP;
            uint8_t scrCode = (targetKanjiScr == SCREEN_ID_TOP) ? 1 : 2;
            uint8_t scrollSteps = (uint8_t)std::min(63, scrollY / 4);
            scrCode = (uint8_t)(scrCode | (scrollSteps << 2));
            uint8_t kanjiPacket[5];
            kanjiPacket[0] = CMD_LOOKUP_KANJI;
            kanjiPacket[1] = scrCode;
            kanjiPacket[2] = (uint8_t)((lastKanjiCp >> 16) & 0xFF);
            uint16_t cpL = htons((uint16_t)(lastKanjiCp & 0xFFFF));
            memcpy(&kanjiPacket[3], &cpL, 2);
            return SendSessionPacket(kanjiPacket, sizeof(kanjiPacket), true);
        };

        auto DismissPopupWindow = [&](void) {
            DismissKanjiWindow();
            if (hasPopup) {
                hasPopup = false;
                popupBundle.active = false;
                cachedPopup.active = false;
                OCR_FreePopupBuffer();
                for (int b = 0; b < 2; ++b) {
                    const Screen &scr = (popupTargetScreen == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
                    int scrW = (popupTargetScreen == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
                    RestoreRect(scr, 0, 0, scrW, 240, getScreen565(popupTargetScreen), scrW, 240);
                    PresentFrame();
                }
            }
        };

        auto getActivePopupInfo = [&](const PopupEntry *&outEntry, const GlyphMap *&outGlyphs, int &outGlossScroll, int &outPosScroll, const std::vector<std::string> *&outContexts) -> bool {
            if (cachedPopup.active && cachedPopup.sourceScreenId < 2 &&
                screenBundles[cachedPopup.sourceScreenId].ready &&
                cachedPopup.spotIdx < screenBundles[cachedPopup.sourceScreenId].spots.size()) {
                const ScreenBundle &sb = screenBundles[cachedPopup.sourceScreenId];
                const ScreenSpot &sp = sb.spots[cachedPopup.spotIdx];
                if (!sp.variants.empty()) {
                    size_t vIdx = std::min(cachedPopup.curVariantIdx, sp.variants.size() - 1);
                    const ScreenSpotVariant &sv = sp.variants[vIdx];
                    if (!sv.entryIndices.empty()) {
                        size_t eIdx = std::min(cachedPopup.curEntryIdx, sv.entryIndices.size() - 1);
                        uint16_t poolIdx = sv.entryIndices[eIdx];
                        if (poolIdx < sb.entries.size()) {
                            outEntry = &sb.entries[poolIdx];
                            outGlyphs = &sb.glyphs;
                            outGlossScroll = cachedPopup.glossScrollY;
                            outPosScroll = cachedPopup.posScrollX;
                            outContexts = &sb.contexts;
                            return true;
                        }
                    }
                }
            } else if (popupBundle.active && !popupBundle.variants.empty()) {
                size_t vIdx = std::min(popupBundle.curVariantIdx, popupBundle.variants.size() - 1);
                const PopupVariant &var = popupBundle.variants[vIdx];
                if (!var.entries.empty()) {
                    size_t eIdx = std::min(popupBundle.curEntryIdx, var.entries.size() - 1);
                    outEntry = &var.entries[eIdx];
                    outGlyphs = &popupBundle.glyphs;
                    outGlossScroll = popupBundle.glossScrollY;
                    outPosScroll = popupBundle.posScrollX;
                    outContexts = nullptr;
                    return true;
                }
            }
            return false;
        };

        // Render initial cursor on both buffers
        SyncBothBuffers();

        s_activeSessionSock = sock;
        s_activeSessionTx = &tx_stream;

        // 6. Interactive Modal Loop
        while (true) {
            if (OCR_ShouldAbortForSleep()) {
                break;
            }
            Controller::Update();
            u32 kDown = Controller::GetKeysPressed();

            // Mid-session AnkiWeb credentials dialog (post-handshake live update)
            if (g_ocrKeys.slimMenu != 0 && Controller::IsKeysPressed(g_ocrKeys.slimMenu)) {
                while (!OCR_ShouldAbortForSleep()) {
                    Controller::Update();
                    if ((Controller::GetKeysDown(false) & g_ocrKeys.slimMenu) == 0) break;
                    Sleep(Milliseconds(16));
                }
                if (OCR_ShouldAbortForSleep()) break;
                OCR_OpenAnkiCredentialsDialog();
                if (OCR_ShouldAbortForSleep()) break;
                for (int b = 0; b < 2; ++b) {
                    RestoreRect(OSD::GetTopScreen(), 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                    RestoreRect(OSD::GetBottomScreen(), 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                    if (hasPopup) {
                        if (isClientBundlePopup()) popupDirty[activeBuf] = true;
                        else if (s_popup_rgb) DrawPopupImage(popupTargetScreen, s_popup_rgb);
                    }
                    PresentFrame();
                }
                continue;
            }

            // Exit / Dismiss Key: Dismiss popup or exit session
            if (kDown & g_ocrKeys.exitSession) {
                if (hasKanjiWindow) {
                    DismissKanjiWindow();
                    continue;
                }
                if (hasPopup) {
                    DismissPopupWindow();
                    continue;
                }
                break;
            }

            // [Start] Button: Launch Camera QR Scanner to scan new PSK and IP/port
            if (kDown & Key::Start) {
                if (OCR_ScanQrCode() || OCR_ShouldAbortForSleep()) {
                    break;
                }
                for (int b = 0; b < 2; ++b) {
                    RestoreRect(OSD::GetTopScreen(), 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                    RestoreRect(OSD::GetBottomScreen(), 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                    if (hasPopup) {
                        if (isClientBundlePopup()) popupDirty[activeBuf] = true;
                        else if (s_popup_rgb) DrawPopupImage(popupTargetScreen, s_popup_rgb);
                    }
                    PresentFrame();
                }
                continue;
            }

            // Mode Toggle Key: Toggle between Single-Word Lookup and Region Translation
            if (kDown & g_ocrKeys.modeToggle) {
                translateMode = !translateMode;
                if (selbox.active) {
                    selbox.active = false;
                    for (int b = 0; b < 2; ++b) {
                        PresentFrame();
                    }
                } else {
                    PresentFrame();
                }

                if (hasPopup) {
                    if (cachedPopup.active && cachedPopup.sourceScreenId < 2) {
                        const ScreenBundle &sb = screenBundles[cachedPopup.sourceScreenId];
                        cachedPopup.statusLevel = 0;
                        cachedPopup.statusBanner = translateMode
                            ? ("Mode: Selection Translate ([" + sb.footerLookup + "] 2 Corners)")
                            : ("Mode: Dictionary Lookup ([" + sb.footerLookup + "] Single Word)");
                        RedrawBundlePopup();
                    } else if (popupBundle.active) {
                        popupBundle.statusLevel = 0;
                        popupBundle.statusBanner = translateMode
                            ? ("Mode: Selection Translate ([" + popupBundle.footerLookup + "] 2 Corners)")
                            : ("Mode: Dictionary Lookup ([" + popupBundle.footerLookup + "] Single Word)");
                        RedrawBundlePopup();
                    } else {
                        uint8_t modePacket[5] = { CMD_SET_MODE, (uint8_t)(translateMode ? 1 : 0), 0, 0, 0 };
                        SendSessionPacket(modePacket, sizeof(modePacket), false);
                    }
                }
            }

            // Circle Pad / C-Stick / D-Pad movement
            int newScreen = curScreen;
            int newX = curX, newY = curY;
            bool moved = false;

            // Circle Pad movement
            shortVector cpad = Controller::GetCirclePadPosition();
            if (abs(cpad.x) > CPAD_DEADZONE || abs(cpad.y) > CPAD_DEADZONE) {
                newX += cpad.x / CPAD_SPEED_DIV;
                newY -= cpad.y / CPAD_SPEED_DIV;
                moved = true;
            }

            // C-Stick movement (New 3DS pointer control)
            shortVector cstick = GetCStickPosition();
            if (abs(cstick.x) > CSTICK_DEADZONE) {
                cstickAccX += (float)cstick.x / CSTICK_SPEED_DIV;
                int stepX = (int)cstickAccX;
                if (stepX != 0) {
                    newX += stepX;
                    cstickAccX -= (float)stepX;
                    moved = true;
                }
            } else {
                cstickAccX = 0.0f;
            }

            if (abs(cstick.y) > CSTICK_DEADZONE) {
                cstickAccY += (float)cstick.y / CSTICK_SPEED_DIV;
                int stepY = (int)cstickAccY;
                if (stepY != 0) {
                    newY -= stepY;
                    cstickAccY -= (float)stepY;
                    moved = true;
                }
            } else {
                cstickAccY = 0.0f;
            }

            // Fallback: Digital C-Stick directions (e.g. if analog mapping is unavailable or tapped)
            if (abs(cstick.x) <= CSTICK_DEADZONE && abs(cstick.y) <= CSTICK_DEADZONE) {
                u32 kHeld = Controller::GetKeysDown();
                if (kHeld & Key::CStickUp)    { newY -= 2; moved = true; }
                if (kHeld & Key::CStickDown)  { newY += 2; moved = true; }
                if (kHeld & Key::CStickLeft)  { newX -= 2; moved = true; }
                if (kHeld & Key::CStickRight) { newX += 2; moved = true; }
            }

            // D-Pad fine-stepping if not navigating popup
            if (!hasPopup) {
                if (kDown & Key::DPadUp)    { newY -= 1; moved = true; }
                if (kDown & Key::DPadDown)  { newY += 1; moved = true; }
                if (kDown & Key::DPadLeft)  { newX -= 1; moved = true; }
                if (kDown & Key::DPadRight) { newX += 1; moved = true; }
            }

            // Screen Switch Key: Switch screen directly
            if (kDown & g_ocrKeys.switchScreen) {
                if (!selbox.active) {
                    newScreen = (curScreen == SCREEN_ID_TOP) ? SCREEN_ID_BOTTOM : SCREEN_ID_TOP;
                    newX = (curScreen == SCREEN_ID_TOP) ? (curX * BOT_WIDTH / TOP_WIDTH) : (curX * TOP_WIDTH / BOT_WIDTH);
                    moved = true;
                }
            }

            bool touchTap = (kDown & Key::Touchpad) != 0;
            bool validTouch = false;
            // Touch Screen: Instantly jump cursor to bottom screen touch position
            if (Touch::IsDown() && (!selbox.active || selbox.screenId == SCREEN_ID_BOTTOM)) {
                UIntVector touch = Touch::GetPosition();
                if (touch.x < BOT_WIDTH && touch.y < BOT_HEIGHT) {
                    newScreen = SCREEN_ID_BOTTOM;
                    newX = (int)touch.x;
                    newY = (int)touch.y;
                    moved = true;
                    validTouch = true;
                }
            }

            // Clamp and wrap cursor between screens vertically if crossing boundary
            if (moved) {
                if (!selbox.active) {
                    if (newScreen == SCREEN_ID_TOP && newY >= TOP_HEIGHT) {
                        newScreen = SCREEN_ID_BOTTOM;
                        newY = 0;
                        newX = (newX * BOT_WIDTH) / TOP_WIDTH;
                    } else if (newScreen == SCREEN_ID_BOTTOM && newY < 0) {
                        newScreen = SCREEN_ID_TOP;
                        newY = TOP_HEIGHT - 1;
                        newX = (newX * TOP_WIDTH) / BOT_WIDTH;
                    }
                }
                int maxW = (newScreen == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
                newX = std::max(0, std::min(newX, maxW - 1));
                newY = std::max(0, std::min(newY, 239));

                if (newScreen != curScreen || newX != curX || newY != curY) {
                    curScreen = newScreen;
                    curX = newX;
                    curY = newY;

                    if (selbox.active) {
                        selbox.x2 = curX;
                        selbox.y2 = curY;
                    }
                    PresentFrame();
                }
            }

            // Lookup / Select Point Key or Touch Screen Tap: Execute action
            bool lookupTriggered = (kDown & g_ocrKeys.lookup) != 0;
            bool touchPopupTap = touchTap && hasPopup && popupTargetScreen == SCREEN_ID_BOTTOM && !selbox.active;
            bool touchKanjiTap = touchTap && hasKanjiWindow && kanjiTargetScreen == SCREEN_ID_BOTTOM && !selbox.active;
            bool touchLookupTap = touchTap && validTouch && !translateMode && !selbox.active;
            if (lookupTriggered || touchPopupTap || touchKanjiTap || touchLookupTap) {
                bool isTouchAction = touchPopupTap || touchKanjiTap || touchLookupTap;
                int actionScreen = isTouchAction ? SCREEN_ID_BOTTOM : curScreen;
                int actionX = isTouchAction ? newX : curX;
                int actionY = isTouchAction ? newY : curY;

                // 0. Check if clicking/tapping inside the active Kanji Details window
                if (hasKanjiWindow && kanjiTargetScreen == actionScreen) {
                    int xOff = (actionScreen == SCREEN_ID_TOP) ? ((TOP_WIDTH - POPUP_WIDTH) / 2) : 0;
                    int popX = actionX - xOff;
                    int popY = actionY;
                    if (popX >= 0 && popX < POPUP_WIDTH && popY >= 0 && popY < POPUP_HEIGHT) {
                        int tallY = popY;
                        if (popY >= 32 && popY < 216) {
                            tallY = popY + std::max(0, std::min(s_kanjiScrollY, s_kanjiMaxScrollY));
                        } else if (popY >= 216) {
                            tallY = (KANJI_TALL_HEIGHT - 24) + (popY - 216);
                        }
                        uint8_t kWinPkt[5];
                        uint16_t netX = htons((uint16_t)popX);
                        uint16_t netY = htons((uint16_t)tallY);
                        kWinPkt[0] = CMD_CLICK_KANJI_WIN;
                        memcpy(&kWinPkt[1], &netX, 2);
                        memcpy(&kWinPkt[3], &netY, 2);
                        SendSessionPacket(kWinPkt, sizeof(kWinPkt), true);
                        continue;
                    }
                }

                // 1. Check if clicking/tapping inside the active translation popup
                if (hasPopup && popupTargetScreen == actionScreen) {
                    int xOff = (actionScreen == SCREEN_ID_TOP) ? ((TOP_WIDTH - POPUP_WIDTH) / 2) : 0;
                    int popX = actionX - xOff;
                    int popY = actionY;
                    if (popX >= 0 && popX < POPUP_WIDTH && popY >= 0 && popY < POPUP_HEIGHT) {
                        const PopupEntry *pe = nullptr;
                        const GlyphMap *pg = nullptr;
                        const std::vector<std::string> *pCtx = nullptr;
                        int gs = 0, ps = 0;
                        if (getActivePopupInfo(pe, pg, gs, ps, pCtx)) {
                            uint32_t kanjiCp = 0;
                            if (TryGetKanjiAtPopupCoord(popX, popY, *pg, *pe, gs, ps, kanjiCp, pCtx)) {
                                lastKanjiCp = kanjiCp;
                                kanjiScrollY = 0;
                                SendKanjiLookup(0);
                                continue;
                            }
                        }
                        // Clicked inside popup, but not on a kanji:
                        // Dismiss kanji window if open, but DO NOT dismiss the translation popup
                        if (hasKanjiWindow) {
                            DismissKanjiWindow();
                        }
                        continue;
                    }
                }

                if (!translateMode) {
                    // Clicking outside popup or on other screen dismisses any kanji window
                    DismissKanjiWindow();

                    // Clear existing popup if it occupies the clicked screen
                    if (hasPopup && popupTargetScreen == curScreen) {
                        DismissPopupWindow();
                    }

                    size_t hitSpotIdx = 0;
                    if (curScreen < 2 && screenBundles[curScreen].ready &&
                        FindScreenSpot(screenBundles[curScreen], curX, curY, hitSpotIdx)) {
                        // Instant 0-network local lookup from pre-pushed ScreenBundle
                        OCR_FreePopupBuffer();
                        popupBundle.active = false;
                        cachedPopup.active = true;
                        cachedPopup.sourceScreenId = (uint8_t)curScreen;
                        cachedPopup.clickX = (uint16_t)curX;
                        cachedPopup.clickY = (uint16_t)curY;
                        cachedPopup.spotIdx = hitSpotIdx;
                        cachedPopup.curVariantIdx = 0;
                        cachedPopup.curEntryIdx = 0;
                        cachedPopup.glossScrollY = 0;
                        cachedPopup.posScrollX = 0;
                        cachedPopup.statusBanner.clear();
                        cachedPopup.statusLevel = 0;
                        popupTargetScreen = (curScreen == SCREEN_ID_TOP) ? SCREEN_ID_BOTTOM : SCREEN_ID_TOP;
                        hasPopup = true;
                        dpadHoldTicks = 0;
                        RedrawBundlePopup();
                        continue;
                    }

                    // Fallback to server click if screen bundle is still arriving or click was far from detected text
                    uint8_t clickPacket[5];
                    uint16_t netX = htons((uint16_t)curX);
                    uint16_t netY = htons((uint16_t)curY);
                    clickPacket[0] = (uint8_t)curScreen;
                    memcpy(&clickPacket[1], &netX, 2);
                    memcpy(&clickPacket[3], &netY, 2);

                    SendSessionPacket(clickPacket, sizeof(clickPacket), false);
                } else if (!selbox.active) {
                    DismissKanjiWindow();
                    // Region translation Point 1
                    selbox.screenId = curScreen;
                    selbox.x1 = curX;
                    selbox.y1 = curY;
                    selbox.x2 = curX;
                    selbox.y2 = curY;
                    selbox.active = true;
                    PresentFrame();
                } else {
                    // Region translation Point 2: send region bounding box
                    selbox.active = false;
                    for (int b = 0; b < 2; ++b) {
                        PresentFrame();
                    }

                    uint8_t boxCmd = (selbox.screenId == SCREEN_ID_TOP) ? CMD_TRANSLATE_TOP : CMD_TRANSLATE_BOT;
                    uint8_t pkt1[5], pkt2[5];
                    uint16_t netX1 = htons((uint16_t)selbox.x1);
                    uint16_t netY1 = htons((uint16_t)selbox.y1);
                    uint16_t netX2 = htons((uint16_t)curX);
                    uint16_t netY2 = htons((uint16_t)curY);

                    pkt1[0] = boxCmd;
                    memcpy(&pkt1[1], &netX1, 2);
                    memcpy(&pkt1[3], &netY1, 2);

                    pkt2[0] = boxCmd;
                    memcpy(&pkt2[1], &netX2, 2);
                    memcpy(&pkt2[3], &netY2, 2);

                    if (SendSessionPacket(pkt1, sizeof(pkt1), false)) {
                        SendSessionPacket(pkt2, sizeof(pkt2), false);
                    }
                }
            } else if (hasPopup) {
                if ((kDown & g_ocrKeys.addAnki) && hasKanjiWindow) {
                    uint8_t kanjiAnkiPkt[5] = { CMD_ADD_ANKI_KANJI, 0, 0, 0, 0 };
                    SendSessionPacket(kanjiAnkiPkt, sizeof(kanjiAnkiPkt), true);
                    continue;
                }
                // D-pad Up/Down scroll the kanji window locally (no network round-trip)
                if (hasKanjiWindow && s_kanji_tall_565) {
                    u32 dpadMask = Key::DPadUp | Key::DPadDown;
                    u32 held = Controller::GetKeysDown(false) & dpadMask;
                    u32 trig = kDown & dpadMask;
                    if (held == 0) {
                        dpadHoldTicks = 0;
                    } else {
                        dpadHoldTicks++;
                        if (dpadHoldTicks > 14 && ((dpadHoldTicks - 14) % 3) == 0) trig |= held;
                    }
                    if (trig & Key::DPadUp) {
                        s_kanjiScrollY = std::max(0, s_kanjiScrollY - 8);
                        for (int b = 0; b < 2; ++b) {
                            DrawKanjiCrop(kanjiTargetScreen, s_kanji_tall_565, s_kanjiScrollY);
                            PresentFrame();
                        }
                        continue;
                    } else if (trig & Key::DPadDown) {
                        s_kanjiScrollY = std::min(s_kanjiMaxScrollY, s_kanjiScrollY + 8);
                        for (int b = 0; b < 2; ++b) {
                            DrawKanjiCrop(kanjiTargetScreen, s_kanji_tall_565, s_kanjiScrollY);
                            PresentFrame();
                        }
                        continue;
                    }
                }
                auto handleLocalNav = [&](size_t numVars, size_t numEntries, const std::string &curEngLbl,
                                          const std::string &nextEngLbl, const std::string &nextOcrTxt,
                                          const GlyphMap &activeGlyphs,
                                          const PopupEntry &curEntry, size_t &vIdx, size_t &eIdx,
                                          int &gScroll, int &pScroll, std::string &sBanner, uint8_t &sLevel,
                                          bool &sendAnki, bool &needRedraw) {
                    if (kDown & g_ocrKeys.prevEntry) {
                        if (numEntries > 0) {
                            eIdx = (eIdx + numEntries - 1) % numEntries;
                            gScroll = 0;
                            pScroll = 0;
                            sBanner.clear();
                            needRedraw = true;
                        }
                    } else if (kDown & g_ocrKeys.nextEntry) {
                        if (numEntries > 0) {
                            eIdx = (eIdx + 1) % numEntries;
                            gScroll = 0;
                            pScroll = 0;
                            sBanner.clear();
                            needRedraw = true;
                        }
                    } else if ((kDown & g_ocrKeys.prevOcr) || (kDown & g_ocrKeys.nextOcr)) {
                        if (numVars <= 1) {
                            sLevel = 0;
                            sBanner = (curEngLbl == "Gemini")
                                ? "OCR performed by Gemini"
                                : ("All 4 OCR engines agree (" + curEngLbl + ")");
                            needRedraw = true;
                        } else {
                            if (kDown & g_ocrKeys.prevOcr) vIdx = (vIdx + numVars - 1) % numVars;
                            else vIdx = (vIdx + 1) % numVars;
                            eIdx = 0;
                            gScroll = 0;
                            pScroll = 0;
                            sLevel = 0;
                            sBanner = "OCR [" + std::to_string((int)vIdx + 1) + "/" +
                                std::to_string((int)numVars) + "] (" + nextEngLbl + "): " +
                                nextOcrTxt.substr(0, 20);
                            needRedraw = true;
                        }
                    } else if (kDown & g_ocrKeys.addAnki) {
                        sendAnki = true;
                    } else {
                        u32 dpadMask = Key::DPadUp | Key::DPadDown | Key::DPadLeft | Key::DPadRight;
                        u32 held = Controller::GetKeysDown(false) & dpadMask;
                        u32 trig = kDown & dpadMask;
                        if (held == 0) {
                            dpadHoldTicks = 0;
                        } else {
                            dpadHoldTicks++;
                            if (dpadHoldTicks > 14 && ((dpadHoldTicks - 14) % 3) == 0) trig |= held;
                        }
                        if (trig != 0) {
                            int visGloss = ComputePopupTitleLayout(activeGlyphs, curEntry).visibleGlossLines;
                            int maxGlossY = std::max(0, (int)curEntry.wrappedLines.size() - visGloss);
                            int maxPosX = std::max(0, (int)Utf8ToCodepoints(curEntry.pos).size() - POS_WINDOW_CHARS);
                            int oldG = gScroll;
                            int oldP = pScroll;
                            if (trig & Key::DPadUp)        gScroll = std::max(0, gScroll - 1);
                            else if (trig & Key::DPadDown) gScroll = std::min(maxGlossY, gScroll + 1);
                            else if (trig & Key::DPadLeft) pScroll = std::max(0, pScroll - 4);
                            else if (trig & Key::DPadRight) pScroll = std::min(maxPosX, pScroll + 4);
                            if (gScroll != oldG || pScroll != oldP) {
                                sBanner.clear();
                                needRedraw = true;
                            }
                        }
                    }
                };

                if (cachedPopup.active && cachedPopup.sourceScreenId < 2 &&
                    screenBundles[cachedPopup.sourceScreenId].ready &&
                    cachedPopup.spotIdx < screenBundles[cachedPopup.sourceScreenId].spots.size()) {
                    const ScreenBundle &sb = screenBundles[cachedPopup.sourceScreenId];
                    const ScreenSpot &sp = sb.spots[cachedPopup.spotIdx];
                    if (!sp.variants.empty()) {
                        if (cachedPopup.curVariantIdx >= sp.variants.size()) cachedPopup.curVariantIdx = 0;
                        const ScreenSpotVariant &sv = sp.variants[cachedPopup.curVariantIdx];
                        if (!sv.entryIndices.empty()) {
                            if (cachedPopup.curEntryIdx >= sv.entryIndices.size()) cachedPopup.curEntryIdx = 0;
                            uint16_t poolIdx = sv.entryIndices[cachedPopup.curEntryIdx];
                            if (poolIdx < sb.entries.size()) {
                                const PopupEntry &ce = sb.entries[poolIdx];
                                size_t nextV = (kDown & g_ocrKeys.prevOcr)
                                    ? ((cachedPopup.curVariantIdx + sp.variants.size() - 1) % sp.variants.size())
                                    : ((cachedPopup.curVariantIdx + 1) % sp.variants.size());
                                bool needRedraw = false, sendAnki = false;
                                handleLocalNav(sp.variants.size(), sv.entryIndices.size(), sv.enginesLabel,
                                               sp.variants[nextV].enginesLabel, sp.variants[nextV].ocrText,
                                               sb.glyphs, ce,
                                               cachedPopup.curVariantIdx, cachedPopup.curEntryIdx,
                                               cachedPopup.glossScrollY, cachedPopup.posScrollX,
                                               cachedPopup.statusBanner, cachedPopup.statusLevel, sendAnki, needRedraw);
                                if (sendAnki) {
                                    uint8_t cmdPacket[5];
                                    uint16_t netSpot = htons((uint16_t)cachedPopup.spotIdx);
                                    cmdPacket[0] = (cachedPopup.sourceScreenId == SCREEN_ID_TOP) ? CMD_ADD_ANKI_TOP : CMD_ADD_ANKI_BOT;
                                    memcpy(&cmdPacket[1], &netSpot, 2);
                                    cmdPacket[3] = (uint8_t)std::min<size_t>(255, cachedPopup.curVariantIdx);
                                    cmdPacket[4] = (uint8_t)std::min<size_t>(255, cachedPopup.curEntryIdx);
                                    SendSessionPacket(cmdPacket, sizeof(cmdPacket), true);
                                } else if (needRedraw) {
                                    RedrawBundlePopup();
                                }
                            }
                        }
                    }
                } else if (popupBundle.active && !popupBundle.variants.empty()) {
                    PopupVariant &curVar = popupBundle.variants[popupBundle.curVariantIdx];
                    PopupEntry &curEntry = curVar.entries[popupBundle.curEntryIdx];
                    size_t nextV = (kDown & g_ocrKeys.prevOcr)
                        ? ((popupBundle.curVariantIdx + popupBundle.variants.size() - 1) % popupBundle.variants.size())
                        : ((popupBundle.curVariantIdx + 1) % popupBundle.variants.size());
                    bool needRedraw = false, sendAnki = false;
                    handleLocalNav(popupBundle.variants.size(), curVar.entries.size(), curVar.enginesLabel,
                                   popupBundle.variants[nextV].enginesLabel, popupBundle.variants[nextV].ocrText,
                                   popupBundle.glyphs, curEntry,
                                   popupBundle.curVariantIdx, popupBundle.curEntryIdx,
                                   popupBundle.glossScrollY, popupBundle.posScrollX,
                                   popupBundle.statusBanner, popupBundle.statusLevel, sendAnki, needRedraw);
                    if (sendAnki) {
                        uint8_t cmdPacket[5];
                        uint16_t netV = htons((uint16_t)popupBundle.curVariantIdx);
                        uint16_t netE = htons((uint16_t)popupBundle.curEntryIdx);
                        cmdPacket[0] = CMD_ADD_ANKI;
                        memcpy(&cmdPacket[1], &netV, 2);
                        memcpy(&cmdPacket[3], &netE, 2);
                        SendSessionPacket(cmdPacket, sizeof(cmdPacket), true);
                    } else if (needRedraw) {
                        RedrawBundlePopup();
                    }
                } else {
                    // Legacy server-rendered popup navigation
                    uint8_t navCmd = 0;
                    if (kDown & g_ocrKeys.prevEntry)   navCmd = CMD_PREV_ENTRY;
                    else if (kDown & g_ocrKeys.nextEntry) navCmd = CMD_NEXT_ENTRY;
                    else if (kDown & g_ocrKeys.prevOcr)   navCmd = CMD_PREV_OCR;
                    else if (kDown & g_ocrKeys.nextOcr)   navCmd = CMD_NEXT_OCR;
                    else if (kDown & Key::DPadUp)      navCmd = CMD_SCROLL_UP;
                    else if (kDown & Key::DPadDown)    navCmd = CMD_SCROLL_DOWN;
                    else if (kDown & Key::DPadLeft)    navCmd = CMD_POS_LEFT;
                    else if (kDown & Key::DPadRight)   navCmd = CMD_POS_RIGHT;
                    else if (kDown & g_ocrKeys.addAnki) navCmd = CMD_ADD_ANKI;

                    if (navCmd != 0) {
                        uint8_t cmdPacket[5] = { navCmd, 0, 0, 0, 0 };
                        SendSessionPacket(cmdPacket, sizeof(cmdPacket), false);
                    }
                }
            }

            // Non-blocking poll for incoming popup image or bundle from server
            if (sock >= 0) {
                struct pollfd pfd;
                pfd.fd = sock;
                pfd.events = POLLIN;
                pfd.revents = 0;
                int pollRes = poll(&pfd, 1, 0);
                if (pollRes < 0 || (pollRes > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(pfd.revents & POLLIN))) {
                    MarkDisconnected(false);
                } else if (pollRes > 0 && (pfd.revents & POLLIN)) {
                    uint8_t hdr[5];
                    if (recv_encrypted_frame(sock, &rx_stream, hdr, sizeof(hdr)) < 0) {
                        MarkDisconnected(false);
                    } else {
                        uint8_t flagByte = hdr[0];
                        uint8_t targetScreenId = flagByte & 0x01;
                        uint32_t netImgSize = 0;
                        memcpy(&netImgSize, &hdr[1], 4);
                        uint32_t payloadSize = ntohl(netImgSize);

                        if ((flagByte & POPUP_SCREEN_BUNDLE_FLAG) && payloadSize > 0 && payloadSize <= 2 * 1024 * 1024 &&
                            (targetScreenId == SCREEN_ID_TOP || targetScreenId == SCREEN_ID_BOTTOM)) {
                            SessionBuffer rawSb;
                            AutoFreeSessionBuffer rawGuard{rawSb};
                            if (!rawSb.Alloc(0x073E0000, payloadSize, 0x07420000)) {
                                if (!DrainEncryptedPayload(sock, &rx_stream, payloadSize)) MarkDisconnected(false);
                                continue;
                            }
                            if (recv_encrypted_frame(sock, &rx_stream, rawSb.ptr, payloadSize) < 0) {
                                MarkDisconnected(false);
                                continue;
                            }
                            bool hadCachedForScr = (cachedPopup.active && cachedPopup.sourceScreenId == targetScreenId);
                            uint16_t savedX = cachedPopup.clickX;
                            uint16_t savedY = cachedPopup.clickY;
                            screenBundles[targetScreenId] = ScreenBundle();

                            ScreenBundle nextSb;
                            if (ParseScreenBundle(rawSb, payloadSize, nextSb) && nextSb.sourceScreenId < 2) {
                                rawSb.Free();
                                uint8_t replacedScr = nextSb.sourceScreenId;
                                screenBundles[replacedScr] = std::move(nextSb);
                                if (cachedPopup.active && cachedPopup.sourceScreenId == replacedScr) {
                                    size_t newSpotIdx = 0;
                                    if (FindScreenSpot(screenBundles[replacedScr], cachedPopup.clickX, cachedPopup.clickY, newSpotIdx)) {
                                        cachedPopup.spotIdx = newSpotIdx;
                                        cachedPopup.curVariantIdx = 0;
                                        cachedPopup.curEntryIdx = 0;
                                        cachedPopup.glossScrollY = 0;
                                        cachedPopup.posScrollX = 0;
                                        cachedPopup.statusBanner.clear();
                                        cachedPopup.statusLevel = 0;
                                        RedrawBundlePopup();
                                    } else {
                                        cachedPopup.active = false;
                                        hasPopup = false;
                                        for (int b = 0; b < 2; ++b) {
                                            const Screen &scr = (popupTargetScreen == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
                                            int scrW = (popupTargetScreen == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
                                            RestoreRect(scr, 0, 0, scrW, 240, getScreen565(popupTargetScreen), scrW, 240);
                                            PresentFrame();
                                        }
                                    }
                                }
                            } else {
                                rawSb.Free();
                                if (hadCachedForScr) {
                                    cachedPopup.active = false;
                                    uint8_t clickPacket[5];
                                    uint16_t netX = htons(savedX);
                                    uint16_t netY = htons(savedY);
                                    clickPacket[0] = targetScreenId;
                                    memcpy(&clickPacket[1], &netX, 2);
                                    memcpy(&clickPacket[3], &netY, 2);
                                    SendSessionPacket(clickPacket, sizeof(clickPacket), false);
                                }
                            }
                        } else if ((flagByte & POPUP_BUNDLE_FLAG) && payloadSize > 0 && payloadSize <= 2 * 1024 * 1024 &&
                            (targetScreenId == SCREEN_ID_TOP || targetScreenId == SCREEN_ID_BOTTOM)) {
                            SessionBuffer rawSb;
                            AutoFreeSessionBuffer rawGuard{rawSb};
                            if (!rawSb.Alloc(0x073E0000, payloadSize, 0x07420000)) {
                                if (!DrainEncryptedPayload(sock, &rx_stream, payloadSize)) MarkDisconnected(false);
                                continue;
                            }
                            if (recv_encrypted_frame(sock, &rx_stream, rawSb.ptr, payloadSize) < 0) {
                                MarkDisconnected(false);
                                continue;
                            }
                            if (!HeapCanFit(payloadSize * 2 + 49152)) {
                                screenBundles[0] = ScreenBundle();
                                screenBundles[1] = ScreenBundle();
                                popupBundle = PopupBundle();
                            }
                            PopupBundle nextBundle;
                            bool parsed = ParsePopupBundle(rawSb.ptr, payloadSize, nextBundle);
                            rawSb.Free();
                            if (parsed) {
                                DismissKanjiWindow();
                                if (hasPopup && popupTargetScreen != targetScreenId) {
                                    for (int b = 0; b < 2; ++b) {
                                        const Screen &scr = (popupTargetScreen == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
                                        int scrW = (popupTargetScreen == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
                                        RestoreRect(scr, 0, 0, scrW, 240, getScreen565(popupTargetScreen), scrW, 240);
                                        PresentFrame();
                                    }
                                }
                                OCR_FreePopupBuffer();
                                cachedPopup.active = false;
                                popupBundle = std::move(nextBundle);
                                popupTargetScreen = targetScreenId;
                                hasPopup = true;
                                dpadHoldTicks = 0;
                                RedrawBundlePopup();
                            }
                        } else if ((flagByte & POPUP_STATUS_FLAG) && payloadSize > 0 && payloadSize <= 65536 &&
                                   (targetScreenId == SCREEN_ID_TOP || targetScreenId == SCREEN_ID_BOTTOM)) {
                            SessionBuffer rawSb;
                            AutoFreeSessionBuffer rawGuard{rawSb};
                            if (!rawSb.Alloc(0x073E0000, payloadSize, 0x07420000)) {
                                if (!DrainEncryptedPayload(sock, &rx_stream, payloadSize)) MarkDisconnected(false);
                                continue;
                            }
                            if (recv_encrypted_frame(sock, &rx_stream, rawSb.ptr, payloadSize) < 0) {
                                MarkDisconnected(false);
                                continue;
                            }
                            if (cachedPopup.active && cachedPopup.sourceScreenId < 2) {
                                if (ParseStatusBannerPacket(rawSb.ptr, payloadSize,
                                                            screenBundles[cachedPopup.sourceScreenId].glyphs,
                                                            cachedPopup.statusBanner, cachedPopup.statusLevel)) {
                                    RedrawBundlePopup();
                                }
                            } else if (popupBundle.active &&
                                       ParseStatusBannerPacket(rawSb.ptr, payloadSize, popupBundle.glyphs,
                                                               popupBundle.statusBanner, popupBundle.statusLevel)) {
                                RedrawBundlePopup();
                            }
                            rawSb.Free();
                            if (flagByte & POPUP_ANKI_LOGIN_PROMPT_FLAG) {
                                bool updated = OCR_OpenAnkiCredentialsDialog();
                                if (updated && !s_ankiUser.empty() && !s_ankiPass.empty()) {
                                    std::string readyMsg = "AnkiWeb login set! Press [" + OCR_KeyToString(g_ocrKeys.addAnki) + "] to add";
                                    if (cachedPopup.active) {
                                        cachedPopup.statusBanner = readyMsg;
                                        cachedPopup.statusLevel = 1;
                                    } else if (popupBundle.active) {
                                        popupBundle.statusBanner = readyMsg;
                                        popupBundle.statusLevel = 1;
                                    }
                                }
                                for (int b = 0; b < 2; ++b) {
                                    RestoreRect(OSD::GetTopScreen(), 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                                    RestoreRect(OSD::GetBottomScreen(), 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                                    if (hasPopup) {
                                        if (isClientBundlePopup()) popupDirty[activeBuf] = true;
                                        else if (s_popup_rgb) DrawPopupImage(popupTargetScreen, s_popup_rgb);
                                    }
                                    PresentFrame();
                                }
                            }
                        } else if ((flagByte & POPUP_KANJI_FLAG) && payloadSize == KANJI_TALL_PAYLOAD_SIZE &&
                                   (targetScreenId == SCREEN_ID_TOP || targetScreenId == SCREEN_ID_BOTTOM)) {
                            if (!s_kanjiTallBuffer.Alloc(0x07360000, KANJI_TALL_565_SIZE)) {
                                if (!cachedPopup.active) {
                                    screenBundles[0] = ScreenBundle();
                                    screenBundles[1] = ScreenBundle();
                                } else {
                                    screenBundles[1 - cachedPopup.sourceScreenId] = ScreenBundle();
                                }
                                if (!s_kanjiTallBuffer.Alloc(0x07360000, KANJI_TALL_565_SIZE, 0, true)) {
                                    if (!DrainEncryptedPayload(sock, &rx_stream, payloadSize)) MarkDisconnected(false);
                                    else OSD::Notify("OCR Error: Out of memory for kanji window", Color::Red);
                                    continue;
                                }
                            }
                            u16 rawContentH = 0;
                            if (!recv_encrypted_kanji_as_rgb565(sock, &rx_stream, s_kanji_tall_565, POPUP_WIDTH * KANJI_TALL_HEIGHT, rawContentH)) {
                                OCR_FreeKanjiBuffer();
                                hasKanjiWindow = false;
                                MarkDisconnected(false);
                            } else {
                                int contentH = (int)rawContentH;
                                if (contentH > KANJI_TALL_HEIGHT) contentH = KANJI_TALL_HEIGHT;
                                // Scrollable viewport is POPUP_HEIGHT - 24 (216px; footer is fixed)
                                s_kanjiMaxScrollY = std::max(0, contentH - (POPUP_HEIGHT - 24));
                                s_kanjiScrollY = 0;
                                kanjiTargetScreen = targetScreenId;
                                hasKanjiWindow = true;
                                for (int b = 0; b < 2; ++b) {
                                    DrawKanjiCrop(kanjiTargetScreen, s_kanji_tall_565, 0);
                                    PresentFrame();
                                }
                            }
                        } else if (payloadSize == POPUP_RGB_SIZE && (targetScreenId == SCREEN_ID_TOP || targetScreenId == SCREEN_ID_BOTTOM)) {
                            DismissKanjiWindow();
                            if (!s_popupBuffer.Alloc(0x072E0000, POPUP_RGB_SIZE) ||
                                recv_encrypted_frame(sock, &rx_stream, s_popup_rgb, POPUP_RGB_SIZE) < 0) {
                                OCR_FreePopupBuffer();
                                MarkDisconnected(false);
                            } else {
                                if (hasPopup && popupTargetScreen != targetScreenId) {
                                    for (int b = 0; b < 2; ++b) {
                                        const Screen &scr = (popupTargetScreen == SCREEN_ID_TOP) ? OSD::GetTopScreen() : OSD::GetBottomScreen();
                                        int scrW = (popupTargetScreen == SCREEN_ID_TOP) ? TOP_WIDTH : BOT_WIDTH;
                                        RestoreRect(scr, 0, 0, scrW, 240, getScreen565(popupTargetScreen), scrW, 240);
                                        PresentFrame();
                                    }
                                }

                                cachedPopup.active = false;
                                popupBundle.active = false;
                                popupTargetScreen = targetScreenId;
                                hasPopup = true;

                                for (int b = 0; b < 2; ++b) {
                                    DrawPopupImage(popupTargetScreen, s_popup_rgb);
                                    PresentFrame();
                                }
                            }
                        } else {
                            // Drain unhandled/unexpected payload so TCP stream stays synchronized
                            if (payloadSize > 0) {
                                if (!DrainEncryptedPayload(sock, &rx_stream, payloadSize)) {
                                    MarkDisconnected(false);
                                }
                            }
                        }
                    }
                }
            }

            if (OCR_ShouldAbortForSleep()) {
                break;
            }

            // Control frame rate
            Sleep(Milliseconds(16));
        }

        // 7. Cleanup & Restore Original Game Screens (both buffers to prevent tearing)
        const bool abortedForSleep = OCR_ShouldAbortForSleep();
        if (!abortedForSleep) {
            for (int b = 0; b < 2; ++b) {
                RestoreRect(OSD::GetTopScreen(), 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                RestoreRect(OSD::GetBottomScreen(), 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                OSD::SwapBuffers();
            }
        }

        s_activeSessionSock = -1;
        s_activeSessionTx = nullptr;

        if (sock >= 0) {
            closesocket(sock);
            sock = -1;
        }
        crypto_wipe(&tx_stream, sizeof(tx_stream));
        crypto_wipe(&rx_stream, sizeof(rx_stream));

        // Tear down heap/aliased socInit state before resuming Mode 3 games (e.g. Smash Bros)
        if (s_socBufferIsHeap || s_socAliasAddr != 0) {
            OCR_CloseSocLocked(false);
        }

        // Free session snapshot buffers to return ~750 KB to the game process
        OCR_FreeSessionBuffers();

        OCR_ExitCStick();

        if (!abortedForSleep) {
            while (Controller::IsKeysDown(g_ocrKeys.exitSession) && !OCR_ShouldAbortForSleep()) {
                Controller::Update();
                Sleep(Milliseconds(16));
            }
        }

        // Unpause game
        Process::Play();
        if (!abortedForSleep) {
            Sleep(Milliseconds(30));
        }
    }

    void OCR_OpenKeyBindingsMenu(void)
    {
        if (OCR_ShouldAbortForSleep()) {
            return;
        }
        ScopedPausedOverlay pauseGuard;
        Process::Pause();

        bool hasTopSnap = s_topBuffer.Alloc(0x07250000, TOP_FB_565_SIZE);
        bool hasBotSnap = s_botBuffer.Alloc(0x072A0000, BOT_FB_565_SIZE);
        if (hasTopSnap) CaptureScreen(OSD::GetTopScreen(), TOP_WIDTH, TOP_HEIGHT, s_top_565);
        if (hasBotSnap) CaptureScreen(OSD::GetBottomScreen(), BOT_WIDTH, BOT_HEIGHT, s_bot_565);

        // Wait for opening combo keys to be released
        while (!OCR_ShouldAbortForSleep()) {
            Controller::Update();
            if (Controller::GetKeysDown(false) == 0) break;
            Sleep(Milliseconds(16));
        }

        enum ItemKind { ITEM_KEY, ITEM_ANKI, ITEM_RESET };
        struct MenuItem {
            const char *label;
            u32 *target;
            ItemKind kind;
        };
        MenuItem items[13] = {
            { "Activation",     &g_ocrKeys.activation,   ITEM_KEY },
            { "Slim Menu",      &g_ocrKeys.slimMenu,     ITEM_KEY },
            { "Lookup / Point", &g_ocrKeys.lookup,       ITEM_KEY },
            { "Mode Toggle",    &g_ocrKeys.modeToggle,   ITEM_KEY },
            { "Switch Screen",  &g_ocrKeys.switchScreen, ITEM_KEY },
            { "Add to Anki",    &g_ocrKeys.addAnki,      ITEM_KEY },
            { "Prev Entry",     &g_ocrKeys.prevEntry,    ITEM_KEY },
            { "Next Entry",     &g_ocrKeys.nextEntry,    ITEM_KEY },
            { "Prev OCR",       &g_ocrKeys.prevOcr,      ITEM_KEY },
            { "Next OCR",       &g_ocrKeys.nextOcr,      ITEM_KEY },
            { "Exit Session",   &g_ocrKeys.exitSession,  ITEM_KEY },
            { "AnkiWeb Login",  nullptr,                 ITEM_ANKI },
            { "Reset Defaults", nullptr,                 ITEM_RESET }
        };

        const u32 validMask = Key::A | Key::B | Key::X | Key::Y | Key::L | Key::R |
                              Key::ZL | Key::ZR | Key::Start | Key::Select |
                              Key::DPadUp | Key::DPadDown | Key::DPadLeft | Key::DPadRight;

        int sel = 0;
        bool capturing = false;
        u32 capturedMask = 0;
        u32 prevCaptureHeld = 0;
        int statusTimer = 0;
        int needRedraw = 2;

        while (!OCR_ShouldAbortForSleep()) {
            Controller::Update();
            u32 kDown = Controller::GetKeysPressed();
            u32 held = Controller::GetKeysDown(false) & validMask;

            if (capturing) {
                if (held != 0) {
                    u32 nextMask = capturedMask | held;
                    if (nextMask != capturedMask || held != prevCaptureHeld) {
                        capturedMask = nextMask;
                        prevCaptureHeld = held;
                        needRedraw = 2;
                    }
                } else if (capturedMask != 0) {
                    if (items[sel].target) {
                        *items[sel].target = capturedMask;
                        OCR_SaveKeyBindings();
                    }
                    capturing = false;
                    capturedMask = 0;
                    prevCaptureHeld = 0;
                    needRedraw = 2;
                }
            } else {
                if (kDown & Key::B) {
                    break;
                } else if (kDown & Key::DPadUp) {
                    sel = (sel + 12) % 13;
                    needRedraw = 2;
                } else if (kDown & Key::DPadDown) {
                    sel = (sel + 1) % 13;
                    needRedraw = 2;
                } else if (kDown & Key::A) {
                    if (items[sel].kind == ITEM_RESET) {
                        OCR_ResetKeyBindings();
                        statusTimer = 90;
                        needRedraw = 2;
                    } else if (items[sel].kind == ITEM_ANKI) {
                        while (!OCR_ShouldAbortForSleep()) {
                            Controller::Update();
                            if ((Controller::GetKeysDown(false) & Key::A) == 0) break;
                            Sleep(Milliseconds(16));
                        }
                        if (OCR_ShouldAbortForSleep()) break;
                        OCR_OpenAnkiCredentialsDialog();
                        if (OCR_ShouldAbortForSleep()) break;
                        for (int b = 0; b < 2; ++b) {
                            if (hasTopSnap) RestoreRect(OSD::GetTopScreen(), 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                            OSD::SwapBuffers();
                        }
                        needRedraw = 2;
                    } else {
                        while (!OCR_ShouldAbortForSleep()) {
                            Controller::Update();
                            if ((Controller::GetKeysDown(false) & Key::A) == 0) break;
                            Sleep(Milliseconds(16));
                        }
                        if (OCR_ShouldAbortForSleep()) break;
                        capturing = true;
                        capturedMask = 0;
                        prevCaptureHeld = 0;
                        needRedraw = 2;
                    }
                }
            }

            if (statusTimer > 0) {
                --statusTimer;
                if (statusTimer == 0) needRedraw = 2;
            }

            if (needRedraw > 0) {
                const Screen &bot = OSD::GetBottomScreen();
                bot.DrawRect(0, 0, BOT_WIDTH, BOT_HEIGHT, Color(20, 24, 34), true);
                bot.DrawRect(0, 0, BOT_WIDTH, 22, Color(34, 42, 64), true);
                bot.Draw("OCR Settings & Anki Login", 8, 5, Color(255, 230, 115), Color(34, 42, 64));

                for (int i = 0; i < 13; ++i) {
                    int y = 24 + i * 15;
                    bool isSel = (i == sel);
                    Color rowBg = isSel ? Color(48, 62, 96) : Color(20, 24, 34);
                    if (isSel) {
                        bot.DrawRect(4, y - 1, BOT_WIDTH - 8, 14, rowBg, true);
                    }
                    Color labelCol = isSel ? Color(255, 255, 255) : Color(195, 210, 230);
                    bot.Draw(items[i].label, 10, y + 1, labelCol, rowBg);

                    if (items[i].kind == ITEM_KEY && items[i].target != nullptr) {
                        std::string keyStr = "[" + OCR_KeyToString(*items[i].target) + "]";
                        if (keyStr.size() > 21) keyStr = keyStr.substr(0, 20) + "]";
                        Color valCol = isSel ? Color(125, 230, 165) : Color(145, 195, 245);
                        bot.Draw(keyStr, 130, y + 1, valCol, rowBg);
                    } else if (items[i].kind == ITEM_ANKI) {
                        std::string ankiStr = "[" + OCR_GetAnkiCredentialsLabel() + "]";
                        if (ankiStr.size() > 21) ankiStr = ankiStr.substr(0, 18) + "...]";
                        Color valCol = isSel ? Color(125, 230, 165) : Color(255, 215, 120);
                        bot.Draw(ankiStr, 130, y + 1, valCol, rowBg);
                    } else {
                        bot.Draw("[Restore All]", 130, y + 1, Color(255, 175, 80), rowBg);
                    }
                }

                bot.DrawRect(0, 220, BOT_WIDTH, 20, Color(28, 34, 50), true);
                if (statusTimer > 0) {
                    bot.Draw("Defaults restored!", 8, 224, Color(125, 230, 165), Color(28, 34, 50));
                } else {
                    bot.Draw("[D-Pad]Nav  [A]Assign  [B]Save&Exit", 8, 224, Color(150, 175, 210), Color(28, 34, 50));
                }

                if (capturing) {
                    bot.DrawRect(28, 74, 264, 92, Color(88, 166, 255), true);
                    bot.DrawRect(30, 76, 260, 88, Color(22, 28, 42), true);
                    std::string title = std::string("Assign: ") + items[sel].label;
                    bot.Draw(title, 42, 86, Color(255, 230, 115), Color(22, 28, 42));
                    bot.Draw("Hold button(s) & release", 42, 106, Color(195, 210, 230), Color(22, 28, 42));
                    std::string cur = (capturedMask != 0) ? OCR_KeyToString(capturedMask) : "Waiting...";
                    if (cur.size() > 34) cur = cur.substr(0, 34);
                    bot.Draw(cur, 42, 132, Color(125, 230, 165), Color(22, 28, 42));
                }

                if (OCR_ShouldAbortForSleep()) break;
                OSD::SwapBuffers();
                --needRedraw;
            }

            Sleep(Milliseconds(16));
        }

        const bool abortedForSleep = OCR_ShouldAbortForSleep();

        // Wait for B release before resuming game
        while (!abortedForSleep && !OCR_ShouldAbortForSleep()) {
            Controller::Update();
            if ((Controller::GetKeysDown(false) & Key::B) == 0) break;
            Sleep(Milliseconds(16));
        }

        OCR_SaveKeyBindings();

        if (!abortedForSleep && !OCR_ShouldAbortForSleep()) {
            for (int b = 0; b < 2; ++b) {
                if (hasTopSnap) RestoreRect(OSD::GetTopScreen(), 0, 0, TOP_WIDTH, 240, s_top_565, TOP_WIDTH, 240);
                if (hasBotSnap) RestoreRect(OSD::GetBottomScreen(), 0, 0, BOT_WIDTH, 240, s_bot_565, BOT_WIDTH, 240);
                OSD::SwapBuffers();
            }
        }

        OCR_FreeSessionBuffers();
        Process::Play();
        if (!abortedForSleep) {
            Sleep(Milliseconds(30));
        }
    }

    void OCR_UpdateFrame(void)
    {
        if (OCR_ShouldAbortForSleep()) {
            return;
        }
        // Wait for active rendered frames after boot before locking NDMU/AC so nnMain startup isn't raced
        static u32 s_bootFrames = 0;
        if (!s_networkLocked) {
            if (++s_bootFrames >= 10) {
                OCR_LockNetwork();
            }
        }

        // Configurable Hotkey Combo
        if (Controller::IsKeysPressed(g_ocrKeys.activation)) {
            OCR_LaunchSession();
        }
    }
}
