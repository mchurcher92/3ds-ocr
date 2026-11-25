#ifndef CTRPLUGINFRAMEWORK_SOUND_SOUNDENGINE_IMPL_HPP
#define CTRPLUGINFRAMEWORK_SOUND_SOUNDENGINE_IMPL_HPP

#include <3ds.h>
#include "CTRPluginFramework/Sound.hpp"

namespace CTRPluginFramework
{
    class SoundEngineImpl
    {
    public:
        static void Initializelibcwav() {}
        static void NotifyAptEvent(APT_HookType event) {}
        template<typename T>
        static void SetVaToPaConvFunction(T function) {}
        static bool RegisterMenuSoundEvent(SoundEngine::Event eventType, Sound& sound) { return false; }
        static Sound& GetMenuSoundEvent(SoundEngine::Event eventType) { static Sound s; return s; }
        static Sound::CWAVStatus PlayMenuSound(SoundEngine::Event eventType) { return Sound::CWAVStatus::NOT_ALLOCATED; }
        static void StopMenuSound(SoundEngine::Event eventType) {}
        static void DeRegisterMenuSoundEvent(SoundEngine::Event eventType) {}
        static void InitializeMenuSounds() {}
        static void ClearMenuSounds() {}
    };
}

#endif
