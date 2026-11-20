#include "types.h"
#include "3ds.h"
#include <cstdio>
#include "CTRPluginFramework/System/Controller.hpp"
#include "CTRPluginFramework/System/FwkSettings.hpp"

extern "C" {
    extern vu32* hidSharedMem;
    void hidSetSharedMem(vu32* sharedMem);
    void HbBootLog(const char *msg);

    static vu32 s_dummyHidMem[0x2B0 / 4] = {0};
    static vu32 s_dummyIrrstMem[0x98 / 4] = {0};
    static LightLock s_hidMemLock;
    static volatile bool s_hidLockReady = false;
    static volatile u32 s_gameHidAddr = 0;
    static volatile u32 s_gameIrrstAddr = 0;
    static volatile u32 s_pendingIrrstAddr = 0;
    static volatile bool s_pluginOwnIrrst = false;

    static void EnsureHidLock(void)
    {
        if (!s_hidLockReady)
        {
            LightLock_Init(&s_hidMemLock);
            s_hidLockReady = true;
        }
    }

    void CTRPF_InitHidLock(void)
    {
        EnsureHidLock();
    }

    void CTRPF_SetPendingIrrstAddr(u32 addr)
    {
        EnsureHidLock();
        LightLock_Lock(&s_hidMemLock);
        if (s_gameIrrstAddr == 0)
            s_pendingIrrstAddr = addr;
        __dmb();
        LightLock_Unlock(&s_hidMemLock);
    }

    bool CTRPF_HasPendingIrrstAddr(void)
    {
        return s_pendingIrrstAddr != 0;
    }

    void CTRPF_OnHidMemMapped(u32 addr, bool isIrrst)
    {
        EnsureHidLock();
        LightLock_Lock(&s_hidMemLock);
        if (isIrrst)
        {
            s_pendingIrrstAddr = 0;
            if (s_pluginOwnIrrst)
            {
                s_pluginOwnIrrst = false;
                irrstExit();
            }
            s_gameIrrstAddr = addr;
            irrstSharedMem = reinterpret_cast<vu32*>(addr);
        }
        else
        {
            s_gameHidAddr = addr;
            hidSetSharedMem(reinterpret_cast<vu32*>(addr));
        }
        __dmb();
        LightLock_Unlock(&s_hidMemLock);
    }

    bool CTRPF_OnHidMemUnmapping(u32 addr)
    {
        if (addr == 0 || (addr != s_gameHidAddr && addr != s_gameIrrstAddr && addr != s_pendingIrrstAddr))
            return false;

        EnsureHidLock();
        LightLock_Lock(&s_hidMemLock);
        bool detached = false;
        if (addr == s_gameHidAddr)
        {
            s_gameHidAddr = 0;
            hidSetSharedMem(s_dummyHidMem);
            detached = true;
        }
        if (addr == s_pendingIrrstAddr)
        {
            s_pendingIrrstAddr = 0;
            detached = true;
        }
        if (addr == s_gameIrrstAddr)
        {
            s_gameIrrstAddr = 0;
            irrstSharedMem = s_dummyIrrstMem;
            detached = true;
        }
        __dmb();
        LightLock_Unlock(&s_hidMemLock);
        return detached;
    }

    Result CTRPF_InitPluginIrrst(void)
    {
        EnsureHidLock();
        LightLock_Lock(&s_hidMemLock);
        Result res = 0;
        if (s_gameIrrstAddr == 0 && s_pendingIrrstAddr == 0 && !s_pluginOwnIrrst && irrstSharedMem == nullptr)
        {
            res = irrstInit();
            if (R_SUCCEEDED(res))
                s_pluginOwnIrrst = true;
        }
        __dmb();
        LightLock_Unlock(&s_hidMemLock);
        return res;
    }

    bool CTRPF_ExitPluginIrrst(void)
    {
        EnsureHidLock();
        LightLock_Lock(&s_hidMemLock);
        bool exited = false;
        if (s_pluginOwnIrrst)
        {
            s_pluginOwnIrrst = false;
            irrstExit();
            exited = true;
        }
        __dmb();
        LightLock_Unlock(&s_hidMemLock);
        return exited;
    }

    u32 CTRPF_GetGameHidAddr(void)
    {
        return s_gameHidAddr;
    }

    u32 CTRPF_GetGameIrrstAddr(void)
    {
        return s_gameIrrstAddr;
    }
}

namespace CTRPluginFramework
{
    u32     Controller::_keysDown = 0;
    u32     Controller::_keysHeld = 0;
    u32     Controller::_keysReleased = 0;

    static bool IsAddrReadable(u32 addr, u32 minSize)
    {
        MemInfo mi{};
        PageInfo pi{};
        if (R_FAILED(svcQueryMemory(&mi, &pi, addr)))
            return false;
        return mi.state != MEMSTATE_FREE && (mi.perm & MEMPERM_READ) != 0
            && addr >= mi.base_addr && (addr - mi.base_addr + minSize) <= mi.size;
    }

    static u32 ReadGameIrrstKeysHeld(vu32* imem)
    {
        if (imem == nullptr || imem == s_dummyIrrstMem)
            return 0;

        u32 idx = imem[4];
        if (idx > 7)
            idx = 7;

        if (idx == 0)
        {
            u64 tickOld = reinterpret_cast<vu64*>(imem)[1];
            u64 tickNew = reinterpret_cast<vu64*>(imem)[0];
            if (tickNew == tickOld || static_cast<s64>(tickNew) < 0)
                return 0;
        }

        return imem[6 + idx * 4];
    }

    shortVector  Controller::GetCirclePadPosition()
    {
        circlePosition cPos;

        hidCircleRead(&cPos);
        return (shortVector(cPos.dx, cPos.dy));
    }

    // TODO: a better injection, touch input injection being not good enough
    void    Controller::InjectTouch(u16 posX, u16 posY)
    {
        touchPosition tpos = {.px = posX, .py = posY};

        for (int i = 0; i < 8; i++)
        {
            int j = 42 + 8 + i * 2;
            *(touchPosition *)(&hidSharedMem[j]) = tpos;
            hidSharedMem[j + 1] = 1;
        }

        u64 tick = svcGetSystemTick();
        hidSharedMem[42] = 0;
        *((u64*)&hidSharedMem[42]) = tick;
    }

    void    Controller::InjectKey(u32 key)
    {
        for (int i = 0; i < 8; i++)
        {
            int j = 10 + i * 4;
            *(u32 *)(&hidSharedMem[j]) |= key;
        }
    }

    void    Controller::Update(void)
    {
        static u32 s_prevIrrstHeld = 0;
        static bool s_loggedHidDetach = false;

        EnsureHidLock();
        LightLock_Lock(&s_hidMemLock);

        // Detach from game shared memory if unmapped (e.g., homebrew hidExit() before PLG_ABOUT_TO_EXIT)
        if (s_gameHidAddr != 0 && !IsAddrReadable(s_gameHidAddr, 0x2B0))
        {
            if (!s_loggedHidDetach)
            {
                s_loggedHidDetach = true;
                MemInfo mi{};
                PageInfo pi{};
                Result qRes = svcQueryMemory(&mi, &pi, s_gameHidAddr);
                char buf[128];
                std::snprintf(buf, sizeof(buf), "[hid] detach hid=0x%lX res=0x%lX st=%lu perm=0x%lX base=0x%lX sz=0x%lX",
                    s_gameHidAddr, qRes, static_cast<u32>(mi.state), mi.perm, mi.base_addr, mi.size);
                HbBootLog(buf);
            }
            s_gameHidAddr = 0;
            hidSetSharedMem(s_dummyHidMem);
            __dmb();
        }
        if (s_gameIrrstAddr != 0 && !IsAddrReadable(s_gameIrrstAddr, 0x98))
        {
            s_gameIrrstAddr = 0;
            irrstSharedMem = s_dummyIrrstMem;
            __dmb();
        }
        if (s_pendingIrrstAddr != 0 && s_gameIrrstAddr == 0)
        {
            if (!IsAddrReadable(s_pendingIrrstAddr, 0xC0))
            {
                s_pendingIrrstAddr = 0;
            }
            else
            {
                u64 firstSysTick = reinterpret_cast<vu64*>(s_pendingIrrstAddr)[0];
                u32 arrayIndex = reinterpret_cast<vu32*>(s_pendingIrrstAddr)[4];
                u64 firstSysTickTS = reinterpret_cast<vu64*>(s_pendingIrrstAddr + 0xA8)[0];
                u64 currSysTick = svcGetSystemTick();
                u64 delta = (currSysTick >= firstSysTick) ? (currSysTick - firstSysTick) : (firstSysTick - currSysTick);

                if (firstSysTick != 0 && firstSysTickTS == 0 && arrayIndex < 8 && delta < 1000000000ULL)
                {
                    if (s_pluginOwnIrrst)
                    {
                        s_pluginOwnIrrst = false;
                        irrstExit();
                    }
                    s_gameIrrstAddr = s_pendingIrrstAddr;
                    s_pendingIrrstAddr = 0;
                    irrstSharedMem = reinterpret_cast<vu32*>(s_gameIrrstAddr);
                    __dmb();
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "[hidmap] irrst_pend=0x%lX", s_gameIrrstAddr);
                    HbBootLog(buf);
                }
            }
        }

        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        u32 released = hidKeysUp();

        // UseGameHidMemory skips irrstInit(), leaving libctru's irrstRefCount at 0
        if (FwkSettings::Get().UseGameHidMemory && s_gameIrrstAddr != 0)
        {
            u32 irrstHeld = ReadGameIrrstKeysHeld(irrstSharedMem);
            u32 irrstDown = irrstHeld & ~s_prevIrrstHeld;
            u32 irrstUp = s_prevIrrstHeld & ~irrstHeld;
            s_prevIrrstHeld = irrstHeld;

            down |= irrstDown;
            held |= irrstHeld;
            released |= irrstUp;
        }
        else if (s_prevIrrstHeld != 0)
        {
            released |= s_prevIrrstHeld;
            s_prevIrrstHeld = 0;
        }

        _keysDown = down;
        _keysHeld = held;
        _keysReleased = released;

        LightLock_Unlock(&s_hidMemLock);
    }
}
