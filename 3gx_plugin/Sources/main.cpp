#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include "ocr_client.hpp"

namespace CTRPluginFramework
{
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
            OCR_Cleanup();
        }
    }

    static void MenuEntry_OcrTest(MenuEntry *entry)
    {
        std::string resultMsg;
        bool ok = OCR_TestServerConnection(resultMsg);
        std::string title = ok ? "Server Online" : "Connection Error";
        MessageBox(title, resultMsg)();
    }

    static void MenuEntry_ScanQr(MenuEntry *entry)
    {
        OCR_ScanQrCode();
    }

    static void MenuEntry_OcrInstructions(MenuEntry *entry)
    {
        MessageBox("How to Use OCR", OCR_GetInstructionsText())();
    }

    static MenuEntry *s_entryActivation = nullptr;
    static MenuEntry *s_entryLookup = nullptr;
    static MenuEntry *s_entryModeToggle = nullptr;
    static MenuEntry *s_entrySwitchScreen = nullptr;
    static MenuEntry *s_entryAddAnki = nullptr;
    static MenuEntry *s_entryPrevEntry = nullptr;
    static MenuEntry *s_entryNextEntry = nullptr;
    static MenuEntry *s_entryPrevOcr = nullptr;
    static MenuEntry *s_entryNextOcr = nullptr;
    static MenuEntry *s_entryExitSession = nullptr;

    static void UpdateSettingsEntryNames(void)
    {
        if (s_entryActivation)
            s_entryActivation->Name() = "OCR Activation: [" + OCR_KeyToString(g_ocrKeys.activation) + "]";
        if (s_entryLookup)
            s_entryLookup->Name() = "Word / Region Point: [" + OCR_KeyToString(g_ocrKeys.lookup) + "]";
        if (s_entryModeToggle)
            s_entryModeToggle->Name() = "Mode Toggle: [" + OCR_KeyToString(g_ocrKeys.modeToggle) + "]";
        if (s_entrySwitchScreen)
            s_entrySwitchScreen->Name() = "Screen Switch: [" + OCR_KeyToString(g_ocrKeys.switchScreen) + "]";
        if (s_entryAddAnki)
            s_entryAddAnki->Name() = "Add to Anki: [" + OCR_KeyToString(g_ocrKeys.addAnki) + "]";
        if (s_entryPrevEntry)
            s_entryPrevEntry->Name() = "Previous Match: [" + OCR_KeyToString(g_ocrKeys.prevEntry) + "]";
        if (s_entryNextEntry)
            s_entryNextEntry->Name() = "Next Match: [" + OCR_KeyToString(g_ocrKeys.nextEntry) + "]";
        if (s_entryPrevOcr)
            s_entryPrevOcr->Name() = "Previous OCR Output: [" + OCR_KeyToString(g_ocrKeys.prevOcr) + "]";
        if (s_entryNextOcr)
            s_entryNextOcr->Name() = "Next OCR Output: [" + OCR_KeyToString(g_ocrKeys.nextOcr) + "]";
        if (s_entryExitSession)
            s_entryExitSession->Name() = "Exit / Dismiss: [" + OCR_KeyToString(g_ocrKeys.exitSession) + "]";
    }

    static void MenuEntry_SetActivation(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.activation, "Activation Hotkey");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.activation = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Activation hotkey updated!", Color::Green);
        }
    }

    static void MenuEntry_SetLookup(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.lookup, "Lookup / Point Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.lookup = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Lookup key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetModeToggle(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.modeToggle, "Mode Toggle Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.modeToggle = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Mode toggle key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetSwitchScreen(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.switchScreen, "Screen Switch Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.switchScreen = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Screen switch key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetAddAnki(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.addAnki, "Add to Anki Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.addAnki = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Anki key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetPrevEntry(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.prevEntry, "Previous Match Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.prevEntry = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Previous match key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetNextEntry(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.nextEntry, "Next Match Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.nextEntry = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Next match key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetPrevOcr(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.prevOcr, "Previous OCR Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.prevOcr = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Previous OCR key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetNextOcr(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.nextOcr, "Next OCR Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.nextOcr = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Next OCR key updated!", Color::Green);
        }
    }

    static void MenuEntry_SetExitSession(MenuEntry *entry)
    {
        Hotkey hk(g_ocrKeys.exitSession, "Exit Session Key");
        hk.AskForKeys();
        if (hk.GetKeys() != 0) {
            g_ocrKeys.exitSession = hk.GetKeys();
            OCR_SaveKeyBindings();
            UpdateSettingsEntryNames();
            OSD::Notify("Exit key updated!", Color::Green);
        }
    }

    static void MenuEntry_ResetKeys(MenuEntry *entry)
    {
        OCR_ResetKeyBindings();
        UpdateSettingsEntryNames();
        OSD::Notify("All OCR keys reset to default!", Color::Green);
    }

    void InitMenu(PluginMenu &menu)
    {
        menu += new MenuEntry("Scan Server QR Code", nullptr, MenuEntry_ScanQr,
            "Launch outer camera to scan the Server GUI QR code. Automatically saves new PSK and server IPs.");

        menu += new MenuEntry("Test OCR Server Connection", nullptr, MenuEntry_OcrTest,
            "Tests TCP connection and PSK authentication with the OCR server.");

        menu += new MenuEntry("OCR Instructions & Controls", nullptr, MenuEntry_OcrInstructions,
            "View hotkeys and controls for in-game Japanese OCR and translation.");

        // Settings Folder
        MenuFolder *settingsFolder = new MenuFolder("OCR Settings", "Customize button combinations and controls.");

        s_entryActivation = new MenuEntry("OCR Activation", nullptr, MenuEntry_SetActivation,
            "Press [A] to change the hotkey combination that activates OCR during gameplay.");

        s_entryLookup = new MenuEntry("Word / Region Point", nullptr, MenuEntry_SetLookup,
            "Press [A] to change the button used to look up a single word or place selection box corners.");

        s_entryModeToggle = new MenuEntry("Mode Toggle", nullptr, MenuEntry_SetModeToggle,
            "Press [A] to change the button that toggles between Single Word Lookup and Region Translation.");

        s_entrySwitchScreen = new MenuEntry("Screen Switch", nullptr, MenuEntry_SetSwitchScreen,
            "Press [A] to change the button that switches the cursor between Top and Bottom screens.");

        s_entryAddAnki = new MenuEntry("Add to Anki", nullptr, MenuEntry_SetAddAnki,
            "Press [A] to change the button that exports the active dictionary entry to Anki.");

        s_entryPrevEntry = new MenuEntry("Previous Match", nullptr, MenuEntry_SetPrevEntry,
            "Press [A] to change the button that navigates to the previous dictionary entry.");

        s_entryNextEntry = new MenuEntry("Next Match", nullptr, MenuEntry_SetNextEntry,
            "Press [A] to change the button that navigates to the next dictionary entry.");

        s_entryPrevOcr = new MenuEntry("Previous OCR Output", nullptr, MenuEntry_SetPrevOcr,
            "Press [A] to change the button that cycles to the previous differing OCR engine output.");

        s_entryNextOcr = new MenuEntry("Next OCR Output", nullptr, MenuEntry_SetNextOcr,
            "Press [A] to change the button that cycles to the next differing OCR engine output.");

        s_entryExitSession = new MenuEntry("Exit / Dismiss", nullptr, MenuEntry_SetExitSession,
            "Press [A] to change the button that closes the popup or exits OCR back to the game.");

        MenuEntry *entryReset = new MenuEntry("Reset All Keys to Default", nullptr, MenuEntry_ResetKeys,
            "Press [A] to restore all OCR keys to their default controls.");

        UpdateSettingsEntryNames();

        *settingsFolder += s_entryActivation;
        *settingsFolder += s_entryLookup;
        *settingsFolder += s_entryModeToggle;
        *settingsFolder += s_entrySwitchScreen;
        *settingsFolder += s_entryAddAnki;
        *settingsFolder += s_entryPrevEntry;
        *settingsFolder += s_entryNextEntry;
        *settingsFolder += s_entryPrevOcr;
        *settingsFolder += s_entryNextOcr;
        *settingsFolder += s_entryExitSession;
        *settingsFolder += entryReset;

        menu += settingsFolder;
    }

    int main(void)
    {
        OCR_Initialize();
        Process::SetProcessEventCallback(OnProcessEvent);

        PluginMenu *menu = new PluginMenu("Japanese OCR & Translation", 1, 0, 0,
            "Standalone Japanese OCR & AI Translation Client for Nintendo 3DS.\n\n"
            "Hotkeys:\n"
            "- In-Game: Press R + Start + Select to activate OCR.\n"
            "- Menu: Press Select to open this menu.");

        menu->SynchronizeWithFrame(true);

        // Frame callback runs on every frame to detect R + Start + Select hotkey
        menu->Callback(OCR_UpdateFrame);

        InitMenu(*menu);

        menu->Run();

        OCR_Cleanup();
        return 0;
    }
}
