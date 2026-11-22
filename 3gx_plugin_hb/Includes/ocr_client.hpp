#ifndef OCR_CLIENT_HPP
#define OCR_CLIENT_HPP

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <string>

namespace CTRPluginFramework
{
    // Network & Config Paths
    #define OCR_CONFIG_SD_PATH      "/luma/ocr_config.txt"
    #define OCR_PSK_SD_PATH         "/luma/ocr_psk.txt"
    #define OCR_KEYS_SD_PATH        "/luma/ocr_keys.txt"
    #define OCR_ANKI_SD_PATH        "/luma/ocr_anki.txt"
    #define OCR_DEVICE_ID_SD_PATH   "/luma/ocr_device_id.txt"
    #define OCR_PSK_MAX_LEN         128
    #define OCR_ANKI_MAX_LEN        256

    // Screen Dimensions & Formats
    #define SCREEN_ID_TOP           0
    #define SCREEN_ID_BOTTOM        1
    #define TOP_WIDTH               400
    #define TOP_HEIGHT              240
    #define BOT_WIDTH               320
    #define BOT_HEIGHT              240
    #define POPUP_WIDTH             320
    #define POPUP_HEIGHT            240
    #define KANJI_TALL_HEIGHT       400
    #define BPP_RGB24               3
    #define TOP_FB_RGB_SIZE         (TOP_WIDTH * TOP_HEIGHT * BPP_RGB24)
    #define BOT_FB_RGB_SIZE         (BOT_WIDTH * BOT_HEIGHT * BPP_RGB24)
    #define POPUP_RGB_SIZE          (POPUP_WIDTH * POPUP_HEIGHT * BPP_RGB24)
    #define KANJI_TALL_RGB_SIZE     (POPUP_WIDTH * KANJI_TALL_HEIGHT * BPP_RGB24)
    #define KANJI_TALL_PAYLOAD_SIZE (KANJI_TALL_RGB_SIZE + 2)  // +2 bytes: big-endian uint16 content_height

    // Protocol Navigation Commands
    #define CMD_PREV_ENTRY          0x10
    #define CMD_NEXT_ENTRY          0x11
    #define CMD_SCROLL_UP           0x12
    #define CMD_SCROLL_DOWN         0x13
    #define CMD_POS_LEFT            0x14
    #define CMD_POS_RIGHT           0x15
    #define CMD_ADD_ANKI            0x16
    #define CMD_SET_MODE            0x17
    #define CMD_SET_KEY_LABELS      0x18
    #define CMD_PREV_OCR            0x19
    #define CMD_NEXT_OCR            0x1A
    #define CMD_SET_CLIENT_CAPS     0x1B
    #define CMD_ADD_ANKI_TOP        0x1C
    #define CMD_ADD_ANKI_BOT        0x1D
    #define CMD_TRANSLATE_TOP       0x20
    #define CMD_TRANSLATE_BOT       0x21
    #define CMD_LOOKUP_KANJI        0x22
    #define CMD_ADD_ANKI_KANJI      0x23
    #define CMD_SET_ANKI_CREDS      0x24
    #define CMD_SET_DEVICE_INFO     0x25
    #define CMD_CLICK_KANJI_WIN     0x26

    // Server Response Flags (OR'd onto target_screen_id in 5-byte popup header)
    #define POPUP_BUNDLE_FLAG        0x80
    #define POPUP_STATUS_FLAG        0x40
    #define POPUP_SCREEN_BUNDLE_FLAG 0x20
    #define POPUP_KANJI_FLAG         0x10
    #define POPUP_ANKI_LOGIN_PROMPT_FLAG 0x08

    // Keybindings Structure
    struct OcrKeyBindings {
        u32 activation;   // Default: Key::R | Key::Start | Key::Select
        u32 lookup;       // Default: Key::A
        u32 modeToggle;   // Default: Key::Select
        u32 switchScreen; // Default: Key::Y
        u32 addAnki;      // Default: Key::X
        u32 prevEntry;    // Default: Key::L
        u32 nextEntry;    // Default: Key::R
        u32 exitSession;  // Default: Key::B
        u32 prevOcr;      // Default: Key::ZL
        u32 nextOcr;      // Default: Key::ZR
    };

    extern OcrKeyBindings g_ocrKeys;

    // Cursor Configuration
    #define CURSOR_SIZE             7
    #define CURSOR_HALF             (CURSOR_SIZE / 2)
    #define CPAD_DEADZONE           25
    #define CPAD_SPEED_DIV          30

    // Lifecycle & Interface Functions
    void        OCR_Initialize(void);
    void        OCR_Cleanup(void);
    void        OCR_SuspendNetwork(bool closeSoc = false, bool logEvent = true);
    void        OCR_UnlockNetwork(bool logEvent = true);
    void        OCR_RelockNetworkDeferred(void);
    bool        OCR_TestServerConnection(std::string &out_msg);
    void        OCR_LaunchSession(void);
    void        OCR_UpdateFrame(void);
    bool        OCR_ScanQrCode(void);
    bool        OCR_OpenAnkiCredentialsDialog(void);
    std::string OCR_GetAnkiCredentialsLabel(void);
    void        OCR_InitKeyBindings(void);
    void        OCR_SaveKeyBindings(void);
    void        OCR_ResetKeyBindings(void);
    std::string OCR_KeyToString(u32 keys);
    uint8_t     OCR_KeyToCharCode(u32 k);
    std::string OCR_GetInstructionsText(void);
}

#endif // OCR_CLIENT_HPP
