#ifndef CTRPLUGINFRAMEWORK_SOUND_SOUND_IMPL_HPP
#define CTRPLUGINFRAMEWORK_SOUND_SOUND_IMPL_HPP

#include "../../CTRPluginFramework/Sound/Sound.hpp"
#include <string>

namespace CTRPluginFramework
{
    class SoundImpl
    {
    public:
        SoundImpl(const std::string& bcwavFile, int maxSimultPlays = 1) : _refcount(1) {}
        SoundImpl(const u8* bcwavBuffer, int maxSimultPlays = 1) : _refcount(1) {}
        ~SoundImpl() {}

        int GetLoadStatus() { return 0; }
        void SetVolume(float volume) {}
        float GetVolume() { return 0.0f; }
        void SetPan(float pan) {}
        float GetPan() { return 0.0f; }
        u32 GetChannelAmount() { return 0; }
        bool IsLooped() { return false; }
        int Play(int leftEarChannel, int rightEarChannel) { return 0; }
        bool PlayDirectly(int leftEarChannel, int rightEarChannel, u32 directSoundChannel, u32 priority, DirectSoundModifiers& modifiers) { return false; }
        void Stop(int leftEarChannel, int rightEarChannel) {}

        s32 _refcount = 1;
    };
}

#endif
