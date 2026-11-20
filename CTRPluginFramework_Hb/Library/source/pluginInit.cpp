#include <3ds.h>
#include <cstdio>
#include <ctime>
#include "ctrulibExtension.h"
#include "CTRPluginFrameworkImpl.hpp"
#include "CTRPluginFramework.hpp"
#include "CTRPluginFrameworkImpl/Graphics/Font.hpp"
#include "CTRPluginFrameworkImpl/System/Screenshot.hpp"
#include "CTRPluginFrameworkImpl/System/HookManager.hpp"
#include "CTRPluginFrameworkImpl/System/Services/Gsp.hpp"
#include "CTRPluginFrameworkImpl/Sound.hpp"
#include "csvc.h"
#include "plgldr.h"

#define PA_PTR(addr)            (void *)((u32)(addr) | 1 << 31)
#define REG32(addr)             (*(vu32 *)(PA_PTR(addr)))

extern "C"
{
    u32     g_gspEventThreadPriority;
    void*   g_temporaryInitLibStack;

    void    LoadCROHooked(void);
    void    OnLoadCro(void);
    void    abort(void);
    void    initSystem(void);
    void    initLib(void);
    void    __ctru_exit(int rc);
    Result  __sync_init(void);
    void    __system_initSyscalls(void);

    s32     PLGLDR__FetchEvent(void);
    void    PLGLDR__Reply(s32 event);

    void hidInitFake();
    void hidExitFake();
    void hidSetSharedMem(vu32* sharedMem);
    void CTRPF_InitHidLock(void);
    void CTRPF_SetPendingIrrstAddr(u32 addr);
    bool CTRPF_HasPendingIrrstAddr(void);
    void CTRPF_OnHidMemMapped(u32 addr, bool isIrrst);
    bool CTRPF_OnHidMemUnmapping(u32 addr);
    Result CTRPF_InitPluginIrrst(void);
    bool CTRPF_ExitPluginIrrst(void);
    u32  CTRPF_GetGameHidAddr(void);
    u32  CTRPF_GetGameIrrstAddr(void);
    void HbLogDisplayState(const char *tag);
    extern volatile u32 g_hbLibctruHookCount;
    extern volatile s32 g_hbLibctruHookLastRes;
    extern volatile u32 g_hbGspDiagTextSize;
    extern volatile u32 g_hbGspDiagHits;
    extern volatile u32 g_hbGspDiagFirstAddr;
    extern volatile u32 g_hbGspDiagFirstWord;

    extern u32 __ctru_heap;
    extern u32 __ctru_heap_size;
}

using CTRPluginFramework::Hook;
using namespace CTRPluginFrameworkImpl::Services;

namespace CTRPluginFramework
{
    // Threads stacks
    static u8  keepThreadStack[0x1000] ALIGN(8) = {0};

    // Some globals
    FS_Archive  _sdmcArchive;
    Handle      g_continueGameEvent = 0;
    Handle      g_keepThreadHandle;
    Handle      g_keepEvent = 0;
    Handle      g_resumeEvent = 0;

    void    MainThreadInit(void *arg);
    void    MainThreadExit(void);
    void    InstallOSD(void); ///< OSDImpl
    void    InitializeRandomEngine(void);
    static ThreadEx g_mainThread(MainThreadInit, 0x4000, 0x19, -1);

    // From main.cpp
    int     main(void);
}

namespace Kernel
{
    void    Initialize(void);
}

static Hook         g_onLoadCroHook;
static Hook         g_onSharedMemMapHook;
static Hook         g_onSharedMemUnmapHook;
static LightLock    g_onLoadCroLock;

extern "C" void HbBootLog(const char *msg)
{
    static LightLock logLock;
    static bool lockInit = false;
    static char logPath[128] = {0};

    if (!lockInit)
    {
        LightLock_Init(&logLock);
        lockInit = true;
    }

    LightLock_Lock(&logLock);
    {
        if (logPath[0] == '\0')
        {
            if (!CTRPluginFramework::Directory::IsExists("/luma/hb_logs"))
                CTRPluginFramework::Directory::Create("/luma/hb_logs");

            const u64 ms = osGetTime();
            const u64 unixU64 = (ms / 1000ULL > 2208988800ULL) ? (ms / 1000ULL - 2208988800ULL) : (ms / 1000ULL);
            const time_t sec = static_cast<time_t>(unixU64);
            struct tm tmNow{};
            gmtime_r(&sec, &tmNow);

            std::string rawName;
            CTRPluginFramework::Process::GetName(rawName);
            char safeName[16] = {0};
            size_t j = 0;
            for (size_t i = 0; i < rawName.size() && j + 1 < sizeof(safeName); ++i)
            {
                char c = rawName[i];
                if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_' || c == '-')
                    safeName[j++] = c;
            }
            if (j == 0)
            {
                safeName[0] = 'a'; safeName[1] = 'p'; safeName[2] = 'p'; safeName[3] = '\0';
            }

            std::snprintf(logPath, sizeof(logPath), "/luma/hb_logs/%04d%02d%02d_%02d%02d%02d_%s_p%lu.log",
                tmNow.tm_year + 1900, tmNow.tm_mon + 1, tmNow.tm_mday,
                tmNow.tm_hour, tmNow.tm_min, tmNow.tm_sec,
                safeName, CTRPluginFramework::Process::GetProcessID());
        }

        CTRPluginFramework::File f;
        if (CTRPluginFramework::File::Open(f, logPath,
            CTRPluginFramework::File::WRITE | CTRPluginFramework::File::CREATE | CTRPluginFramework::File::APPEND | CTRPluginFramework::File::SYNC) == 0)
        {
            f.Write(msg, strlen(msg));
            f.Write("\n", 1);
        }
    }
    LightLock_Unlock(&logLock);
}

void abort(void)
{
    void* lr = __builtin_return_address(0);

    if (CTRPluginFramework::System::OnAbort)
        CTRPluginFramework::System::OnAbort();

    PLGLDR__DisplayErrMessage("Fatal plugin error", "abort() called from the following location:", (u32)lr);
    svcSleepThread(500000000ULL);

    svcBreak(USERBREAK_PANIC);
    while (true);
}

static void    ExecuteLoopOnEvent(void)
{
    using CTRPluginFramework::PluginMenuExecuteLoop;

    // Execute AR codes
    PluginMenuExecuteLoop::ExecuteAR();

    // Execute builtin codes
    PluginMenuExecuteLoop::ExecuteBuiltin();
}

void     OnLoadCro(void)
{
    LightLock_Lock(&g_onLoadCroLock);
    ExecuteLoopOnEvent();
    LightLock_Unlock(&g_onLoadCroLock);
}

Result     OnSharedMemMap(Handle handle, void* sharedMem, MemPerm myPerm, MemPerm otherPerm)
{
    Result res = svcMapMemoryBlock(handle, (u32)sharedMem, myPerm, otherPerm);
    if (R_SUCCEEDED(res) && sharedMem && CTRPluginFramework::FwkSettings::Get().UseGameHidMemory
        && myPerm == MEMPERM_READ && otherPerm == MEMPERM_DONTCARE)
    {
        MemInfo mi{};
        PageInfo pi{};
        const u32 addr = reinterpret_cast<u32>(sharedMem);
        if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, addr))
            && mi.state != MEMSTATE_FREE && (mi.perm & MEMPERM_READ) != 0
            && addr >= mi.base_addr && (addr - mi.base_addr + 0xC0) <= mi.size)
        {
            u64 firstSysTick = reinterpret_cast<vu64*>(sharedMem)[0];
            u32 arrayIndex = reinterpret_cast<vu32*>(sharedMem)[4];

            u32 sharedMemTouchScreen = addr + 0xA8;
            u64 firstSysTickTS = reinterpret_cast<vu64*>(sharedMemTouchScreen)[0];
            u32 arrayIndexTS = reinterpret_cast<vu32*>(sharedMemTouchScreen)[4];

            u64 currSysTick = svcGetSystemTick();

            // VERY HACKY VERIFICATION THIS IS HID SHARED MEM!!!
            // Probably breaks if hid thread is paused just here, but the likelyness of this
            // happening is very low, so I guess it's good enough...
            if (CTRPF_GetGameHidAddr() == 0 && firstSysTick != 0 && firstSysTickTS != 0
                && llabs(currSysTick - firstSysTick) < 1000000000ULL
                && llabs(currSysTick - firstSysTickTS) < 1000000000ULL
                && arrayIndex < 8 && arrayIndexTS < 8)
            {
                CTRPF_OnHidMemMapped(addr, false);
                HbBootLog(CTRPluginFramework::Utils::Format("[hidmap] hid=%p", sharedMem).c_str());
            }
            else if (CTRPF_GetGameHidAddr() != 0 && CTRPF_GetGameIrrstAddr() == 0
                && CTRPluginFramework::System::IsNew3DS()
                && addr != CTRPF_GetGameHidAddr()
                && firstSysTickTS == 0 && arrayIndex < 8)
            {
                if (firstSysTick != 0 && llabs(currSysTick - firstSysTick) < 1000000000ULL)
                {
                    CTRPF_OnHidMemMapped(addr, true);
                    HbBootLog(CTRPluginFramework::Utils::Format("[hidmap] irrst=%p", sharedMem).c_str());
                }
                else if (firstSysTick == 0 && arrayIndexTS == 0)
                {
                    CTRPF_SetPendingIrrstAddr(addr);
                    HbBootLog(CTRPluginFramework::Utils::Format("[hidmap] irrst_wait=%p", sharedMem).c_str());
                }
            }
        }
    }
    return res;
}

Result     OnSharedMemUnmap(Handle handle, void* sharedMem)
{
    const u32 addr = reinterpret_cast<u32>(sharedMem);
    if (CTRPF_OnHidMemUnmapping(addr))
        HbBootLog(CTRPluginFramework::Utils::Format("[hidunmap] addr=0x%lX", addr).c_str());
    return svcUnmapMemoryBlock(handle, addr);
}

#ifdef _MSC_VER
#define WEAK_SYMBOL
#else
#define WEAK_SYMBOL __attribute__((weak))
#endif

namespace CTRPluginFramework
{
    void WEAK_SYMBOL    PatchProcess(FwkSettings& settings) {}
    void WEAK_SYMBOL    EarlyCallback(u32* savedInstructions);
    void WEAK_SYMBOL    UseGameStackToInit();

    namespace Heap
    {
        extern u8* __ctrpf_heap;
        extern u32 __ctrpf_heap_size;
    }

    static void OpenSdmcArchive(void)
    {
        if (_sdmcArchive != 0)
            return;
        FS_Path sdmcPath = { PATH_EMPTY, 1, (u8*)"" };
        FSUSER_OpenArchive(&_sdmcArchive, ARCHIVE_SDMC, sdmcPath);
    }

    static void InitFS(void)
    {
        // Init sdmcArchive
        OpenSdmcArchive();

        // Set current working directory
        if (FwkSettings::Header->isDefaultPlugin)
        {
            std::string path = "/luma/plugins/ActionReplay";

            if (!Directory::IsExists(path))
                Directory::Create(path);

            path += Utils::Format("/%016llX", Process::GetTitleID());

            if (!Directory::IsExists(path))
                Directory::Create(path);

            Directory::ChangeWorkingDirectory(path + "/");
        }
        else
        {
            char path[255] = {0};

            PLGLDR__GetPluginPath(path);
            for (u32 i = 254; i > 0; --i)
            {
                if (path[i] != '/')
                    continue;
                path[i] = 0;
                break;
            }
            Directory::ChangeWorkingDirectory(path);
        }
    }

    static void     InitHeap(void)
    {
        u32         size = 0x10000; ///< 64KB pool for Heap::Alloc

        // Init System::Heap
        Heap::__ctrpf_heap_size = size;
        Heap::__ctrpf_heap = static_cast<u8 *>(::operator new(size, std::nothrow));
        if (!Heap::__ctrpf_heap)
        {
            size = 0x8000; ///< 32KB fallback
            Heap::__ctrpf_heap_size = size;
            Heap::__ctrpf_heap = static_cast<u8 *>(::operator new(size, std::nothrow));
        }
    }

    void NORETURN KeepThreadMain(void *arg UNUSED)
    {
        // Initialize the synchronization subsystem
        __sync_init();

        // Initialize newlib's syscalls
        __system_initSyscalls();

        // Init heap and newlib's syscalls
        initLib();

        // Initialize services
        srvInit();
        acInit();
        amInit();
        fsInit();
        hidInitFake();
        cfguInit();
        // ncsndInit(false);
        plgLdrInit();

        // Set cwav VA to PA function
        SoundEngineImpl::Initializelibcwav();
        SoundEngineImpl::SetVaToPaConvFunction([](const void* addr) {return svcConvertVAToPA(addr, false);});

        // Initialize Kernel stuff
        Kernel::Initialize();

        // Init Framework's system constants
        SystemImpl::Initialize();

        // Init Process info
        ProcessImpl::Initialize();

        // Init Screen
        ScreenImpl::Initialize();

        // Init default settings
        FwkSettings &settings = FwkSettings::Get();

        settings.ThreadPriority = 0x30;
        settings.AllowActionReplay = true;
        settings.AllowSearchEngine = true;
        settings.WaitTimeToBoot = Seconds(5.f);
        settings.AreN3DSButtonsAvailable = true;
        settings.TryLoadSDSounds = true;
        settings.CloseMenuWithB = false;

        // Set default theme
        FwkSettings::SetThemeDefault();

        // Init scheduler
        Scheduler::Initialize();

        // Init OSD hook
        OSDImpl::_Initialize();

        // Install CRO hook
        {
            const std::vector<u32> LoadCroPattern =
            {
                0xE92D5FFF, 0xE28D4038, 0xE89407E0, 0xE28D4054,
                0xE8944800, 0xEE1D4F70, 0xE59FC058, 0xE3A00000,
                0xE5A4C080, 0xE284C028, 0xE584500C, 0xE584A020
            };

            /*const std::vector<u32> UnloadCroPattern =
            {
                0xE92D4070, 0xEE1D4F70, 0xE59F502C, 0xE3A0C000,
                0xE5A45080, 0xE2846004, 0xE5840014, 0xE59F001C,
                0xE886100E, 0xE5900000, 0xEF000032, 0xE2001102
            };*/

            // TODO: the new plugin system make this useless
            u32     loadCroAddress = Utils::Search<u32>(0x00100000, Process::GetTextSize(), LoadCroPattern);

            if (loadCroAddress)
            {
                LightLock_Init(&g_onLoadCroLock);
                g_onLoadCroHook.Initialize(loadCroAddress, (u32)LoadCROHooked);
                g_onLoadCroHook.Enable();
            }
        }

        // Init sdmc & paths
        InitFS();

        auto HbLog = [](const char *msg) { HbBootLog(msg); };
        HbLog(Utils::Format("[1] InitFS done tid=0x%016llX", Process::GetTitleID()).c_str());

        // Init System::Heap
        InitHeap();

        // Initialize radom engine
        InitializeRandomEngine();

        // Patch process before it starts & let the dev init some settings
        PatchProcess(settings);

        // Init hid properly depending on the settings
        CTRPF_InitHidLock();
        if (settings.UseGameHidMemory)
        {
            const u32 textStart = 0x00100000;
            const u32 textEnd = textStart + Process::GetTextSize();
            u32 hookAddr = 0;
            u32 unmapHookAddr = 0;
            int hookMode = 0;

            // Pass 1: inline `svc 0x1F` preceded by `MOV R3, #0x10000000` (CTR SDK)
            u32 svcMapMemoryBlockAddr = textStart - 4;
            do
            {
                if (svcMapMemoryBlockAddr + 4 >= textEnd)
                    break;
                svcMapMemoryBlockAddr = Utils::Search<u32>(svcMapMemoryBlockAddr + 4, textEnd - (svcMapMemoryBlockAddr + 4), {0xEF00001F});
                if (svcMapMemoryBlockAddr)
                {
                    for (int i = 0; i > -10 && (svcMapMemoryBlockAddr + i * 4) >= textStart; i--)
                    {
                        if (reinterpret_cast<u32*>(svcMapMemoryBlockAddr)[i] == 0xE3A03201) // MOV R3, #0x10000000
                        {
                            hookAddr = svcMapMemoryBlockAddr;
                            hookMode = 1;
                            svcMapMemoryBlockAddr = 0;
                            break;
                        }
                    }
                }
            } while (svcMapMemoryBlockAddr);

            // Pass 2: `MOV R3, #0x10000000` followed by `BL svcMapMemoryBlock` (libctru homebrew)
            if (!hookAddr)
            {
                u32 scanAddr = textStart - 4;
                while (scanAddr + 4 < textEnd && !hookAddr)
                {
                    scanAddr = Utils::Search<u32>(scanAddr + 4, textEnd - (scanAddr + 4), {0xE3A03201});
                    if (!scanAddr)
                        break;

                    for (u32 i = 1; i <= 10 && scanAddr + (i * 4) < textEnd; ++i)
                    {
                        const u32 callAddr = scanAddr + (i * 4);
                        const u32 insn = *reinterpret_cast<u32 *>(callAddr);
                        if ((insn & 0xFF000000) == 0xEB000000)
                        {
                            const s32 rel = static_cast<s32>((insn & 0x00FFFFFF) << 8) >> 6;
                            const u32 target = callAddr + 8 + static_cast<u32>(rel);
                            if (target >= textStart && target + 8 <= textEnd)
                            {
                                const u32 *fn = reinterpret_cast<const u32 *>(target);
                                if (fn[0] == 0xEF00001F && fn[1] == 0xE12FFF1E)
                                {
                                    hookAddr = target;
                                    hookMode = 2;
                                    break;
                                }
                            }
                        }
                    }
                }
            }

            if (hookAddr)
            {
                g_onSharedMemMapHook.Initialize(hookAddr, (u32)OnSharedMemMap);
                g_onSharedMemMapHook.SetFlags(USE_LR_TO_RETURN);
                g_onSharedMemMapHook.Enable();

                // Hook libctru's svcUnmapMemoryBlock thunk so we detach before game unmaps hid/irrst shared memory
                if (hookMode == 2 && hookAddr + 16 <= textEnd)
                {
                    const u32 *nextFn = reinterpret_cast<const u32 *>(hookAddr + 8);
                    if (nextFn[0] == 0xEF000020 && nextFn[1] == 0xE12FFF1E)
                        unmapHookAddr = hookAddr + 8;
                }
                if (!unmapHookAddr)
                    unmapHookAddr = Utils::Search<u32>(textStart, textEnd - textStart, {0xEF000020, 0xE12FFF1E});

                if (unmapHookAddr)
                {
                    g_onSharedMemUnmapHook.Initialize(unmapHookAddr, (u32)OnSharedMemUnmap);
                    g_onSharedMemUnmapHook.SetFlags(USE_LR_TO_RETURN);
                    g_onSharedMemUnmapHook.Enable();
                }
            }

            HbLog(Utils::Format("[hid] useGame=1 mode=%d addr=0x%lX unmap=0x%lX en=%d osd=%d",
                hookMode, hookAddr, unmapHookAddr, g_onSharedMemMapHook.IsEnabled() ? 1 : 0, OSDImpl::OSDHook.IsEnabled() ? 1 : 0).c_str());

            // Check the svcMapMemoryBlock hook was installed properly
            if (!g_onSharedMemMapHook.IsEnabled())
                abort();
        } else {
            hidExitFake();
            Result hidRes = hidInit();
            Result irrstRes = 0;
            if (System::IsNew3DS() && irrstSharedMem == nullptr)
                irrstRes = irrstInit();
            HbLog(Utils::Format("[hid] hid=0x%lX irrst=0x%lX mem=%p",
                hidRes, irrstRes, (void *)irrstSharedMem).c_str());
        }

        // Init menu sounds.
        SoundEngineImpl::InitializeMenuSounds();

        // Load settings
        Preferences::LoadSettings();

        // Check threads priorities
        settings.ThreadPriority = std::min(settings.ThreadPriority, (u32)0x3E);

        // Install GSP interrupt hook BEFORE resuming the process so fast-booting
        // libctru homebrew does not call gspInit() before the hook is active.
        Result gspInitRes = GSP::Initialize();
        HbLog(Utils::Format("[2] GSP::Initialize res=%ld text=0x%lX hits=%lu addr=0x%lX word=0x%lX",
            gspInitRes, g_hbGspDiagTextSize, g_hbGspDiagHits, g_hbGspDiagFirstAddr, g_hbGspDiagFirstWord).c_str());

        // Clamp libctru heap requests to remaining RESLIMIT_COMMIT minus plugin memory.
        {
            const u32 textStart = 0x00100000;
            const u32 textSize = Process::GetTextSize();
            const u32 textEnd = textStart + textSize;

            u32 plgMappedBytes = 0;
            for (u32 probe = 0x06000000; probe < 0x08000000; )
            {
                MemInfo miPlg{};
                PageInfo piPlg{};
                if (R_FAILED(svcQueryMemory(&miPlg, &piPlg, probe)) || miPlg.size == 0)
                    break;
                if (miPlg.state != MEMSTATE_FREE)
                    plgMappedBytes += miPlg.size;
                u32 next = miPlg.base_addr + miPlg.size;
                if (next <= probe)
                    break;
                probe = next;
            }
            const u32 totalPlgBytes = std::max<u32>(plgMappedBytes, FwkSettings::Header->heapSize + 0x100000U);
            const u32 reserveMiB = std::min<u32>(15, std::max<u32>(6, ((totalPlgBytes + 0xFFFFFU) >> 20) + 1));
            const u64 reserveBytes = static_cast<u64>(reserveMiB) << 20;

            s64 limit = 0;
            s64 current = 0;
            Handle resLimit = 0;
            u64 remaining = 0;
            if (R_SUCCEEDED(svcGetResourceLimit(&resLimit, CUR_PROCESS_HANDLE)))
            {
                ResourceLimitType resType = RESLIMIT_COMMIT;
                svcGetResourceLimitLimitValues(&limit, resLimit, &resType, 1);
                svcGetResourceLimitCurrentValues(&current, resLimit, &resType, 1);
                svcCloseHandle(resLimit);
                if (limit > current)
                    remaining = static_cast<u64>(limit - current) & ~0xFFFULL;
            }

            auto isValidSizePtr = [textEnd](u32 ptr) -> bool
            {
                if (ptr < textEnd || ptr >= 0x06000000 || (ptr & 3))
                    return false;
                MemInfo mi{};
                PageInfo pi{};
                if (R_FAILED(svcQueryMemory(&mi, &pi, ptr)))
                    return false;
                if ((mi.perm & MEMPERM_READWRITE) != MEMPERM_READWRITE)
                    return false;
                u32 val = *reinterpret_cast<u32 *>(ptr);
                return val <= 0x08000000 && (val & 0xFFF) == 0;
            };

            u32 litMatch = 0;
            u32 bicPatchedAddr = 0;
            u32 bicReg = 0;
            u32 oldHeap = 0;
            u32 oldLinear = 0;

            for (u32 addr = textStart; addr + 12 <= textEnd; addr += 4)
            {
                if (*reinterpret_cast<u32 *>(addr) != 0xFFFF8001)
                    continue;

                // Verify a PC-relative LDR inside the function targets this 0xFFFF8001 pool word
                const u32 scanFnLo = (addr >= textStart + 0x400) ? (addr - 0x400) : textStart;
                bool hasPoolRef = false;
                for (u32 p = addr - 4; p >= scanFnLo; p -= 4)
                {
                    const u32 insn = *reinterpret_cast<u32 *>(p);
                    if ((insn & 0x0FFF0000) == 0x059F0000 && (p + 8 + (insn & 0xFFF)) == addr)
                    {
                        hasPoolRef = true;
                        break;
                    }
                    if (p == scanFnLo)
                        break;
                }
                if (!hasPoolRef)
                    continue;

                u32 candHeap = *reinterpret_cast<u32 *>(addr + 4);
                u32 candLinear = *reinterpret_cast<u32 *>(addr + 8);
                if (!isValidSizePtr(candHeap) || !isValidSizePtr(candLinear))
                    continue;

                litMatch = addr;
                u32 *pHeapSize = reinterpret_cast<u32 *>(candHeap);
                u32 *pLinearSize = reinterpret_cast<u32 *>(candLinear);
                oldHeap = *pHeapSize;
                oldLinear = *pLinearSize;

                // Patch runtime __system_allocateHeaps (`bic rD, rD, #0xFF0` ... `bic rD, rD, #0xF`)
                // so auto-sized libctru apps (where __ctru_heap_size is in .bss and zeroed by crt0)
                // reserve space for the plugin before splitting remaining memory into heap + linear.
                for (u32 p = scanFnLo; p + 20 <= addr; p += 4)
                {
                    const u32 insn = *reinterpret_cast<u32 *>(p);
                    if ((insn & 0xFFF00FFF) != 0xE3C00EFF)
                        continue;
                    const u32 rn = (insn >> 16) & 0xF;
                    const u32 rd = (insn >> 12) & 0xF;
                    if (rn != rd)
                        continue;

                    const u32 expectBicF = 0xE3C0000F | (rd << 16) | (rd << 12);
                    for (u32 q = p + 4; q <= p + 20 && q < addr; q += 4)
                    {
                        if (*reinterpret_cast<u32 *>(q) == expectBicF)
                        {
                            if (remaining > reserveBytes + 0x200000ULL)
                            {
                                *reinterpret_cast<vu32 *>(q) = 0xE2400600 | (rd << 16) | (rd << 12) | reserveMiB;
                                svcFlushProcessDataCache(CUR_PROCESS_HANDLE, q, 4);
                                svcInvalidateEntireInstructionCache();
                                bicPatchedAddr = q;
                                bicReg = rd;
                            }
                            break;
                        }
                    }
                    if (bicPatchedAddr)
                        break;
                }

                // Also clamp hardcoded non-zero .data heap sizes
                if (remaining > reserveBytes + 0x200000ULL && (*pHeapSize != 0 || *pLinearSize != 0))
                {
                    u64 usable = remaining - reserveBytes;
                    u64 total = static_cast<u64>(*pHeapSize) + static_cast<u64>(*pLinearSize);
                    if (total > usable && usable > static_cast<u64>(*pLinearSize) + 0x100000ULL)
                        *pHeapSize = static_cast<u32>((usable - *pLinearSize) & ~0xFFFULL);
                }
                break;
            }

            HbLog(Utils::Format("[heap] lit=0x%lX bic=0x%lX r=%lu res=%luM plg=0x%lX rem=0x%llX sz=%lX/%lX",
                litMatch, bicPatchedAddr, bicReg, reserveMiB, plgMappedBytes, remaining, oldHeap, oldLinear).c_str());
        }

        // Continue game
        HbLog("[3] signaling continueGameEvent");
        svcSignalEvent(g_continueGameEvent);

        // Wait for the required time while allowing fast-exiting forwarders (PLG_ABOUT_TO_EXIT) to exit immediately
        s32 event = 0;
        bool earlyExit = false;
        bool isSleeping = false;
        bool ncsndInitialised = true;
        bool restoreIrrstOnWake = false;
        bool mainThreadStarted = false;
        {
            const s64 totalMs = settings.WaitTimeToBoot.AsMilliseconds();
            s64 elapsedMs = 0;
            while (elapsedMs < totalMs)
            {
                const s32 ev = PLGLDR__FetchEvent();
                if (ev == PLG_ABOUT_TO_EXIT)
                {
                    HbLog("[plg] early exit during WaitTimeToBoot");
                    event = ev;
                    earlyExit = true;
                    break;
                }
                if (ev == PLG_SLEEP_ENTRY || ev == PLG_HOME_ENTER)
                {
                    if (!isSleeping)
                    {
                        SystemImpl::AptStatus |= BIT(6);
                        SoundEngineImpl::NotifyAptEvent(ev == PLG_SLEEP_ENTRY ? APT_HookType::APTHOOK_ONSLEEP : APT_HookType::APTHOOK_ONSUSPEND);
                        ncsndInitialised = false;
                        isSleeping = true;
                    }
                    PLGLDR__Reply(ev);
                }
                else if (ev == PLG_SLEEP_EXIT || ev == PLG_HOME_EXIT)
                {
                    if (isSleeping)
                    {
                        SystemImpl::WakeUpFromSleep();
                        ncsndInitialised = true;
                        SoundEngineImpl::NotifyAptEvent(ev == PLG_SLEEP_EXIT ? APT_HookType::APTHOOK_ONWAKEUP : APT_HookType::APTHOOK_ONRESTORE);
                        isSleeping = false;
                    }
                    PLGLDR__Reply(ev);
                }
                const s64 stepMs = (totalMs - elapsedMs > 50) ? 50 : (totalMs - elapsedMs);
                Sleep(Milliseconds(stepMs));
                elapsedMs += stepMs;
            }
        }
        HbLog(Utils::Format("[4] after WaitTimeToBoot gspHook=%lu res=0x%lX exit=%d",
            g_hbLibctruHookCount, g_hbLibctruHookLastRes, earlyExit ? 1 : 0).c_str());

        if (!earlyExit)
        {
            if (settings.UseGameHidMemory && System::IsNew3DS()
                && CTRPF_GetGameIrrstAddr() == 0 && !CTRPF_HasPendingIrrstAddr() && irrstSharedMem == nullptr)
            {
                Result irRes = CTRPF_InitPluginIrrst();
                HbLog(Utils::Format("[hid] pluginIrrst r=0x%lX mem=%p", irRes, (void *)irrstSharedMem).c_str());
            }

            svcCreateEvent(&g_keepEvent, RESET_ONESHOT);
            // Create plugin's main thread
            g_mainThread.Start(nullptr);
            mainThreadStarted = true;
            HbLog("[5] mainThread started, waiting keepEvent");

            // Reduce priority (fall back cleanly if process maxPriority rejects ThreadPriority - 1)
            if (R_FAILED(svcSetThreadPriority(g_keepThreadHandle, settings.ThreadPriority - 1)))
            {
                svcSetThreadPriority(g_keepThreadHandle, settings.ThreadPriority);
            }

            // Wait until Main Thread finished all it's initializing
            svcWaitSynchronization(g_keepEvent, U64_MAX);
            svcCloseHandle(g_keepEvent);
            HbLog("[6] keepEvent signaled, entering PLGLDR loop");

            Handle memLayoutChanged;
            u32 lastHookLog = g_hbLibctruHookCount;
            u32 homeElapsedTicks = 0;
            svcControlProcess(CUR_PROCESS_HANDLE, PROCESSOP_GET_ON_MEMORY_CHANGE_EVENT, (u32)&memLayoutChanged, 0);
            while (true)
            {
                if (g_hbLibctruHookCount != lastHookLog) {
                    lastHookLog = g_hbLibctruHookCount;
                    HbLog(Utils::Format("[gsp] hookCount=%lu res=0x%lX", lastHookLog, g_hbLibctruHookLastRes).c_str());
                }
                event = PLGLDR__FetchEvent();
                if (event != 0 && event != PLG_SLEEP_ENTRY)
                    HbLog(Utils::Format("[plg] event=%ld sleep=%d", event, isSleeping ? 1 : 0).c_str());
                if (event == PLG_ABOUT_TO_EXIT)
                {
                    break;
                }
                if (event == PLG_SLEEP_ENTRY || event == PLG_HOME_ENTER)
                {
                    const bool isSleepEntry = (event == PLG_SLEEP_ENTRY);
                    if (!isSleepEntry) {
                        HbLogDisplayState("home_enter");
                        HbLog("[plg] before UserCb");
                    }
                    ProcessImpl::UserProcessEventCallback(isSleepEntry ? Process::Event::SLEEP_ENTER : Process::Event::HOME_ENTER);
                    if (isSleepEntry && settings.UseGameHidMemory && CTRPF_ExitPluginIrrst())
                        restoreIrrstOnWake = true;
                    if (!isSleeping) {
                        SystemImpl::AptStatus |= BIT(6);
                        if (!isSleepEntry)
                            HbLog("[plg] before NotifyAptEvent");
                        SoundEngineImpl::NotifyAptEvent(isSleepEntry ? APT_HookType::APTHOOK_ONSLEEP : APT_HookType::APTHOOK_ONSUSPEND);
                        if (ncsndInitialised) {
                            ncsndInitialised = false;
                            // ncsndExit();
                        }
                        isSleeping = true;
                    }
                    if (!isSleepEntry)
                        HbLog("[plg] before PLGLDR__Reply");
                    PLGLDR__Reply(event);
                    if (isSleepEntry) {
                        HbLog(Utils::Format("[plg] sleep_replied irrst=%d", restoreIrrstOnWake ? 1 : 0).c_str());
                    } else {
                        HbLog("[plg] after PLGLDR__Reply");
                        homeElapsedTicks = 1;
                    }
                }
                else if (event == PLG_SLEEP_EXIT || event == PLG_HOME_EXIT)
                {
                    homeElapsedTicks = 0;
                    ProcessImpl::UserProcessEventCallback(event == PLG_SLEEP_EXIT ? Process::Event::SLEEP_EXIT : Process::Event::HOME_EXIT);
                    if (isSleeping) {
                        SystemImpl::WakeUpFromSleep();
                        if (!ncsndInitialised) {
                            ncsndInitialised = true;
                            // ncsndInit(false);
                        }
                        SoundEngineImpl::NotifyAptEvent(event == PLG_SLEEP_EXIT ? APT_HookType::APTHOOK_ONWAKEUP : APT_HookType::APTHOOK_ONRESTORE);
                        isSleeping = false;
                    }
                    if (event == PLG_SLEEP_EXIT && restoreIrrstOnWake) {
                        restoreIrrstOnWake = false;
                        CTRPF_InitPluginIrrst();
                    }
                    PLGLDR__Reply(event);
                    HbLog(Utils::Format("[plg] exitReply=%ld", event).c_str());
                }
                else if (event == PLG_ABOUT_TO_SWAP)
                {
                    ProcessImpl::UserProcessEventCallback(Process::Event::SWAP_ENTER);

                    SoundEngineImpl::NotifyAptEvent(APT_HookType::APTHOOK_ONSUSPEND);

                    // Close csnd as it may be needed by other processes (4 sessions max.)
                    if (ncsndInitialised) {
                        ncsndInitialised = false;
                        // ncsndExit();
                    }

                    // Un-map hook memory
                    HookManager::Lock();
                    HookManager::PrepareToUnmapMemory();
                    svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x01E80000, 0x2000);

                    // Reply and wait
                    PLGLDR__Reply(event);

                    // Re-map hook memory
                    svcMapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x1E80000, CUR_PROCESS_HANDLE,
                        __ctru_heap + __ctru_heap_size, 0x2000, static_cast<MapExFlags>(0));

                    HookManager::RecoverFromUnmapMemory();
                    HookManager::Unlock();

                    // Init csnd again.
                    if (!ncsndInitialised) {
                        ncsndInitialised = true;
                        // ncsndInit(false);
                    }

                    SoundEngineImpl::NotifyAptEvent(APT_HookType::APTHOOK_ONRESTORE);

                    ProcessImpl::UserProcessEventCallback(Process::Event::SWAP_EXIT);
                }

                if (homeElapsedTicks > 0)
                {
                    if (homeElapsedTicks == 5 || homeElapsedTicks == 10 || homeElapsedTicks == 20 ||
                        homeElapsedTicks == 40 || homeElapsedTicks == 80 || homeElapsedTicks == 160 ||
                        homeElapsedTicks == 320 || homeElapsedTicks == 600)
                    {
                        HbLog(Utils::Format("[plg] homeHb tick=%lu sleep=%d apt=0x%lX",
                            homeElapsedTicks, isSleeping ? 1 : 0, (unsigned long)SystemImpl::AptStatus).c_str());
                        if (homeElapsedTicks == 20)
                            HbLogDisplayState("home_1s");
                        else if (homeElapsedTicks == 80)
                            HbLogDisplayState("home_4s");
                    }
                    if (homeElapsedTicks <= 600)
                        ++homeElapsedTicks;
                }

                Result res = svcWaitSynchronization(memLayoutChanged, 50000000ULL); // 50ms wait
                if (R_SUCCEEDED(res))
                {
                    // Memory layout changed, update memory
                    ProcessImpl::UpdateMemRegions();
                }
            }
        }

        // exit
        ProcessImpl::UserProcessEventCallback(Process::Event::EXIT);
        ProcessImpl::SignalExit();

        SoundEngineImpl::NotifyAptEvent(APT_HookType::APTHOOK_ONEXIT);
        SoundEngineImpl::ClearMenuSounds();

        SystemImpl::AptStatus |= BIT(3);
        Scheduler::Exit();

        // Close PluginMenu to quit main thread
        if (mainThreadStarted)
        {
            PluginMenuImpl::ForceExit();
            g_mainThread.JoinTimeout(true, 100000000ULL);
        }

        // Close some handles
        if (ncsndInitialised) {
            ncsndInitialised = false;
            // ncsndExit();
        }
        if (settings.UseGameHidMemory)
        {
            CTRPF_ExitPluginIrrst();
            hidExitFake();
        }
        else
            hidExit();

        // Un-map hook wrapper memory
        HookManager::Lock();
        HookManager::PrepareToUnmapMemory();
        svcUnmapProcessMemoryEx(CUR_PROCESS_HANDLE, 0x01E80000, 0x2000);

        ProcessImpl::DisableExceptionHandlers();

        // NOTE: Do not call __ctru_exit(0) here because it calls envDestroyHandles()
        // which destroys shared host application handles during exit.

        // This function does not return and exits the thread
        PLGLDR__Reply(event);
        svcExitThread();
        for(;;);
    }

    // Initialize most subsystem / Global variables
    void    Initialize(void)
    {
        // Init sysfont
        Font::Initialize();
        {
            // If /cheats/ doesn't exists, create it
            const char *dirpath = "/cheats";
            if (!Directory::IsExists(dirpath))
                Directory::Create(dirpath);
        }

        // Set AR file path
        Preferences::CheatsFile = "cheats.txt";

        // Default: cheats.txt in cwd
        if (!File::Exists(Preferences::CheatsFile))
            Preferences::CheatsFile = Utils::Format("/cheats/%016llX.txt", Process::GetTitleID());

        {
            // If /Screenshots/ doesn't exists, create it
            const char *dirpath = "/Screenshots";
            if (!Directory::IsExists(dirpath))
                Directory::Create(dirpath);

            // Set default screenshot path
            Screenshot::Path = dirpath;
            Screenshot::Path.append("/");

            // Set default screenshot prefix
            Screenshot::Prefix = "[";
            Process::GetName(Screenshot::Prefix);
            Screenshot::Prefix += Utils::Format(" - %08X] - Screenshot", (u32)Process::GetTitleID());
            Screenshot::Initialize();
        }
    }

    // Main thread's start
    void  MainThreadInit(void *arg)
    {
        Initialize();

        // Wake up init thread
        svcSignalEvent(g_keepEvent);

        // Reduce thread priority
        svcSetThreadPriority(threadGetHandle(threadGetCurrent()), FwkSettings::Get().ThreadPriority);

        // Update memory layout
        ProcessImpl::UpdateMemRegions();

        // Start plugin
        main();

        MainThreadExit();
    }

    void  MainThreadExit(void)
    {
        // ## Main Thread ##

        // Remove the OSD Hook
        OSDImpl::OSDHook.Disable();

        // Release process in case it's currently paused
        ProcessImpl::IsPaused = std::min((u32)ProcessImpl::IsPaused, (u32)1);
        ProcessImpl::Play(true);

        // Exit services
        gspExit();

        g_mainThread.Exit(1);
        return;
    }

    static char *AppendStr(char *p, const char *s)
    {
        while (*s)
            *p++ = *s++;
        return p;
    }

    static char *AppendHex(char *p, u64 val, int digits)
    {
        static const char hex[] = "0123456789ABCDEF";
        *p++ = '0';
        *p++ = 'x';
        for (int i = (digits - 1) * 4; i >= 0; i -= 4)
            *p++ = hex[(val >> i) & 0xF];
        return p;
    }

    struct EarlyKInfo
    {
        u32 resLimitPtr;
        s32 maxPrio;
        s32 maxThread;
        s32 curThread;
    };

    static bool LogEarlyThreadFailure(Result r1, Result r2, s32 curPrio, const EarlyKInfo &ki)
    {
        bool wrote = false;
        Result srvRes = srvInit();
        bool srvOk = R_SUCCEEDED(srvRes);
        Result fsRes = srvOk ? fsInit() : srvRes;
        bool fsOk = srvOk && R_SUCCEEDED(fsRes);
        if (fsOk)
        {
            FS_Archive sdmc = 0;
            FS_Path empty = { PATH_EMPTY, 1, (u8*)"" };
            if (R_SUCCEEDED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, empty)))
            {
                Handle file = 0;
                static const char16_t pathU16[] = u"/luma/hb_logs/early_err.log";
                FS_Path fp = { PATH_UTF16, sizeof(pathU16), reinterpret_cast<const u8 *>(pathU16) };
                if (R_SUCCEEDED(FSUSER_OpenFile(&file, sdmc, fp,
                    FS_OPEN_CREATE | FS_OPEN_WRITE, 0)))
                {
                    s64 tid = 0;
                    svcGetProcessInfo(&tid, CUR_PROCESS_HANDLE, 0x10001);

                    char msg[192];
                    char *p = msg;
                    p = AppendStr(p, "tid=");
                    p = AppendHex(p, static_cast<u64>(tid), 16);
                    p = AppendStr(p, " r1=");
                    p = AppendHex(p, static_cast<u32>(r1), 8);
                    p = AppendStr(p, " r2=");
                    p = AppendHex(p, static_cast<u32>(r2), 8);
                    p = AppendStr(p, " p=");
                    p = AppendHex(p, static_cast<u32>(curPrio), 2);
                    p = AppendStr(p, " n3=");
                    p = AppendHex(p, SystemImpl::IsNew3DS ? 1u : 0u, 1);
                    p = AppendStr(p, " rl=");
                    p = AppendHex(p, ki.resLimitPtr, 8);
                    p = AppendStr(p, " mp=");
                    p = AppendHex(p, static_cast<u32>(ki.maxPrio), 8);
                    p = AppendStr(p, " mt=");
                    p = AppendHex(p, static_cast<u32>(ki.maxThread), 8);
                    p = AppendStr(p, " ct=");
                    p = AppendHex(p, static_cast<u32>(ki.curThread), 8);
                    *p++ = '\n';

                    u64 offset = 0;
                    FSFILE_GetSize(file, &offset);
                    Result wr = FSFILE_Write(file, nullptr, offset, msg, static_cast<u32>(p - msg), FS_WRITE_FLUSH);
                    wrote = R_SUCCEEDED(wr);
                    FSFILE_Close(file);
                }
                FSUSER_CloseArchive(sdmc);
            }
        }
        if (fsOk)
            fsExit();
        if (srvOk)
            srvExit();
        return wrote;
    }

    extern "C"
    int   __entrypoint(int arg, void* temporaryStack)
    {
        // Store temporary stack to use by initlib
        g_temporaryInitLibStack = UseGameStackToInit ? temporaryStack : nullptr;

        // Set ProcessImpl::MainThreadTls
        ProcessImpl::MainThreadTls = (u32)getThreadLocalStorage();

        // Call early callback, with pointer to the 2 saved instructions
        if (EarlyCallback)
            EarlyCallback((u32*)arg);

        // Set exception handlers
        ProcessImpl::EnableExceptionHandlers();
        // Check if we are on citra / New3DS before any KProcess offset access
        SystemImpl::CheckCitra();
        if (!SystemImpl::IsCitra)
        {
            s64 out = 0;
            SystemImpl::IsNew3DS = R_SUCCEEDED(svcGetSystemInfo(&out, 0x10001, 1));
        }

        // Populate manifest
        {
            volatile PluginHeader* header = (volatile PluginHeader*)PA_FROM_VA_PTR(FwkSettings::Header);
            header->notifyHomeEvent = true;
        }

        // Create event
        svcCreateEvent(&g_continueGameEvent, RESET_ONESHOT);
        // Start ctrpf's primary thread; if ExHeader maxThread/maxPriority blocks it, patch KResourceLimit and retry
        Result thRes = svcCreateThread(&g_keepThreadHandle, KeepThreadMain, arg, (u32 *)&keepThreadStack[0x1000], 0x1A, 0);
        if (R_FAILED(thRes))
        {
            Result firstRes = thRes;
            s32 curPrio = 0x30;
            svcGetThreadPriority(&curPrio, CUR_THREAD_HANDLE);
            EarlyKInfo ki{};

            if (!SystemImpl::IsCitra)
            {
                KProcess *proc = KProcess::GetCurrent();
                proc->PatchMaxThreads();
                u32 oldPrio = proc->PatchMaxPriority(0);
                thRes = svcCreateThread(&g_keepThreadHandle, KeepThreadMain, arg, (u32 *)&keepThreadStack[0x1000], 0x1A, 0);
                if (R_FAILED(thRes))
                    thRes = svcCreateThread(&g_keepThreadHandle, KeepThreadMain, arg, (u32 *)&keepThreadStack[0x1000], 0x1A, -2);
                if (R_FAILED(thRes))
                    thRes = svcCreateThread(&g_keepThreadHandle, KeepThreadMain, arg, (u32 *)&keepThreadStack[0x1000], curPrio, -2);
                if (R_FAILED(thRes))
                    thRes = svcCreateThread(&g_keepThreadHandle, KeepThreadMain, arg, (u32 *)&keepThreadStack[0x1000], 0x3F, -2);
                proc->PatchMaxPriority(oldPrio);

                if (R_FAILED(thRes) && proc)
                {
                    auto K_ReadRes = [](KProcess *p, u32 off, EarlyKInfo *out)
                    {
                        KResourceLimit *rl = reinterpret_cast<KResourceLimit *>(*reinterpret_cast<u32 *>(reinterpret_cast<u32>(p) + off));
                        out->resLimitPtr = reinterpret_cast<u32>(rl);
                        if (rl)
                        {
                            out->maxPrio = rl->maxPriority;
                            out->maxThread = rl->maxThread;
                            out->curThread = rl->currentThread;
                        }
                    };
                    svcCustomBackdoor(reinterpret_cast<void *>(+K_ReadRes), proc, SystemImpl::IsNew3DS ? 0x84u : 0x7Cu, &ki);
                }
            }
            else
            {
                thRes = svcCreateThread(&g_keepThreadHandle, KeepThreadMain, arg, (u32 *)&keepThreadStack[0x1000], curPrio, -2);
                if (R_FAILED(thRes) && curPrio != 0x30)
                    thRes = svcCreateThread(&g_keepThreadHandle, KeepThreadMain, arg, (u32 *)&keepThreadStack[0x1000], 0x30, -2);
            }

            if (R_FAILED(thRes))
                LogEarlyThreadFailure(firstRes, thRes, curPrio, ki);
        }
        // Wait until basic initialization has been made before returning to game
        if (R_SUCCEEDED(thRes))
            svcWaitSynchronization(g_continueGameEvent, U64_MAX);
        // Close the event
        svcCloseHandle(g_continueGameEvent);

        return 0;
    }
}
