#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include "ocr_client.hpp"

namespace CTRPluginFramework
{
    static bool s_running = true;

    void PatchProcess(FwkSettings &settings)
    {
        settings.CloseMenuWithB = true;
        settings.AllowActionReplay = false;
        settings.AllowSearchEngine = false;
        settings.WaitTimeToBoot = Seconds(5.f);
    }

    static void OnProcessEvent(Process::Event event)
    {
        if (event == Process::Event::EXIT) {
            s_running = false;
            OCR_Cleanup();
        }
    }

    int main(void)
    {
        OCR_Initialize();
        Process::SetProcessEventCallback(OnProcessEvent);

        // Headless background event loop: No PluginMenu UI, Action Replay, or Search engine
        while (s_running)
        {
            Sleep(Milliseconds(16));
            Controller::Update();
            if (g_ocrKeys.slimMenu != 0 && Controller::IsKeysPressed(g_ocrKeys.slimMenu)) {
                OCR_OpenKeyBindingsMenu();
            } else {
                OCR_UpdateFrame();
            }
        }

        OCR_Cleanup();
        return 0;
    }
}
