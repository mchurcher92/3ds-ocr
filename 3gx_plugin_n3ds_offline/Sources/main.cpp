#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include "ocr_client.hpp"

extern "C" bool HbBootLogIsEnabled(void);

namespace CTRPluginFramework
{
    void PatchProcess(FwkSettings &settings)
    {
        settings.CloseMenuWithB = true;
        settings.AllowActionReplay = false;
        settings.AllowSearchEngine = false;
        settings.WaitTimeToBoot = Seconds(5.f);
        settings.UseGameHidMemory = true;
    }

    static void OnProcessEvent(Process::Event event)
    {
        OCR_OnProcessEvent(event);
        if (event == Process::Event::EXIT) {
            OCR_Cleanup();
        } else if (event == Process::Event::HOME_ENTER) {
            // Keep ndm:u/ac:u open across HOME Menu like retail so 3DS doesn't tear down Wi-Fi
            OCR_SuspendNetwork(false);
        } else if (event == Process::Event::SLEEP_ENTER) {
            // Keep ndm:u/ac:u locked across sleep (closing soc:U, no pre-reply FS writes) so game cannot steal NDM into local-comm (e=2) on wake
            OCR_SuspendNetwork(true, false);
        } else if (event == Process::Event::SWAP_ENTER) {
            // Release all network handles before swapping/exiting process
            OCR_UnlockNetwork();
        } else if (event == Process::Event::HOME_EXIT ||
                   event == Process::Event::SLEEP_EXIT ||
                   event == Process::Event::SWAP_EXIT) {
            OCR_RelockNetworkDeferred();
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

    static MenuEntry *s_entryAnkiLogin = nullptr;
    static MenuEntry *s_entryAnkiMode = nullptr;
    static MenuEntry *s_entryAnkiConnect = nullptr;
    static MenuEntry *s_entryAnkiSync = nullptr;
    static MenuEntry *s_entryOfflineMode = nullptr;

    static void UpdateOfflineModeEntryName(void)
    {
        if (!s_entryOfflineMode) return;
        if (!OCR_IsNew3DS()) {
            s_entryOfflineMode->Name() = "OCR Mode: [Online Only (Old 3DS)]";
        } else {
            s_entryOfflineMode->Name() = OCR_IsOfflineModeEnabled()
                ? "OCR Mode: [Offline (N3DS On-Board)]"
                : "OCR Mode: [Online (PC Server)]";
        }
    }

    static void MenuEntry_ToggleOfflineMode(MenuEntry *entry)
    {
        if (!OCR_IsNew3DS()) {
            MessageBox("New 3DS Required",
                "On-board offline OCR requires New 3DS / New 2DS XL hardware (256 MB RAM, 804 MHz quad-core).\n\n"
                "This console is an Old 3DS/2DS, so only Online (PC Server) mode is available.")();
            return;
        }
        bool next = !OCR_IsOfflineModeEnabled();
        OCR_SetOfflineModeEnabled(next);
        UpdateOfflineModeEntryName();
        OSD::Notify(next ? "Switched to Offline N3DS Mode" : "Switched to Online Server Mode", Color::Green);
    }

    static void MenuEntry_TestOffline(MenuEntry *entry)
    {
        std::string resultMsg;
        bool ok = OCR_TestOfflineAssets(resultMsg);
        MessageBox(ok ? "Offline Assets Ready" : "Offline Asset Error", resultMsg)();
    }

    static void UpdateAnkiEntryName(void)
    {
        if (s_entryAnkiLogin)
            s_entryAnkiLogin->Name() = "AnkiWeb Login: [" + OCR_GetAnkiCredentialsLabel() + "]";
        if (s_entryAnkiMode)
            s_entryAnkiMode->Name() = "Anki Upload Mode: [" + OCR_GetAnkiModeLabel() + "]";
        if (s_entryAnkiConnect)
            s_entryAnkiConnect->Name() = "AnkiConnect Host: [" + OCR_GetAnkiConnectLabel() + "]";
        if (s_entryAnkiSync) {
            int q = OCR_GetQueuedAnkiCount();
            s_entryAnkiSync->Name() = "Sync Queued Anki Cards (" + std::to_string(q) + " waiting)";
        }
    }

    static void MenuEntry_AnkiLogin(MenuEntry *entry)
    {
        if (OCR_OpenAnkiCredentialsDialog()) {
            UpdateAnkiEntryName();
        }
    }

    static void MenuEntry_AnkiMode(MenuEntry *entry)
    {
        if (OCR_OpenAnkiModeDialog()) {
            UpdateAnkiEntryName();
        }
    }

    static void MenuEntry_AnkiConnect(MenuEntry *entry)
    {
        if (OCR_OpenAnkiConnectDialog()) {
            UpdateAnkiEntryName();
        }
    }

    static void MenuEntry_AnkiSync(MenuEntry *entry)
    {
        std::string summary;
        OCR_SyncQueuedAnkiCards(summary);
        UpdateAnkiEntryName();
        MessageBox("Anki Queue Sync", summary)();
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
        s_entryOfflineMode = new MenuEntry("OCR Mode", nullptr, MenuEntry_ToggleOfflineMode,
            "Toggle between on-board Offline OCR + Dictionary (New 3DS only) and Online PC Server mode.");
        UpdateOfflineModeEntryName();
        menu += s_entryOfflineMode;

        menu += new MenuEntry("Test Offline Assets & Benchmark", nullptr, MenuEntry_TestOffline,
            "Checks /luma/ocr_offline/ assets and runs a quick on-board OCR + JMdict benchmark.");

        menu += new MenuEntry("Scan Server QR Code", nullptr, MenuEntry_ScanQr,
            "Launch outer camera to scan the Server GUI QR code. Automatically saves new PSK and server IPs.");

        s_entryAnkiLogin = new MenuEntry("AnkiWeb Login", nullptr, MenuEntry_AnkiLogin,
            "Set your personal AnkiWeb email and password for this 3DS, or clear to use the server default.");
        s_entryAnkiMode = new MenuEntry("Anki Upload Mode", nullptr, MenuEntry_AnkiMode,
            "Choose direct upload mode: AnkiWeb Only, AnkiConnect Only, or either with fallback.");
        s_entryAnkiConnect = new MenuEntry("AnkiConnect Host", nullptr, MenuEntry_AnkiConnect,
            "Set PC LAN IP/port for AnkiConnect (default port 8765, or Auto to follow OCR Server IP).");
        s_entryAnkiSync = new MenuEntry("Sync Queued Anki Cards", nullptr, MenuEntry_AnkiSync,
            "Upload any queued cards from /luma/ocr_offline/anki_queue.jsonl using the active Anki upload mode.");
        UpdateAnkiEntryName();
        menu += s_entryAnkiLogin;
        menu += s_entryAnkiMode;
        menu += s_entryAnkiConnect;
        menu += s_entryAnkiSync;

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

        PluginMenu *menu = new PluginMenu("Japanese OCR & Translation (N3DS Offline)", 1, 0, 0,
            "New 3DS Offline + Online Japanese OCR & Dictionary Client.\n\n"
            "Hotkeys:\n"
            "- In-Game: Press R + Start + Select to activate OCR.\n"
            "- Menu: Press Select to open this menu.");

        menu->SynchronizeWithFrame(true);

        // Frame callback runs on every frame to detect R + Start + Select hotkey
        menu->Callback(OCR_UpdateFrame);

        InitMenu(*menu);

        if (HbBootLogIsEnabled()) {
            File logFile;
            if (File::Open(logFile, "/luma/hb_boot.log", File::WRITE | File::CREATE | File::APPEND | File::SYNC) == 0) {
                logFile.Write("[main] calling menu->Run()\n", 27);
            }
        }

        menu->Run();

        OCR_Cleanup();
        return 0;
    }
}
