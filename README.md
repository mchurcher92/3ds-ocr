# Nintendo 3DS Japanese OCR & Translation

Real-time dual-screen Japanese OCR, dictionary lookup, AI/neural translation, and Anki flashcard creation for Nintendo 3DS, using a lightweight Luma3DS `.3gx` plugin paired with a Python PC server.

## Repository Layout

- `ocr_launcher.py` — Desktop GUI launcher (QR code pairing, Paired 3DSes management, live OCR preview, settings, logs).
- `ocr_server.py` — Socket/OCR/translation/Anki backend server.
- `psk_store.py` — Thread-safe per-3DS PSK store and pairing registry (`ocr_psks.json`).
- `dict_updater.py` & `dict_updater_dialog.py` — First-run open-licensed dictionary downloader & in-app Dictionary Updater.
- `anki_dialog.py` & `ui_theme.py` — Configuration dialogs and dark-mode UI helpers.
- `3gx_plugin/` — Full CTRPluginFramework `.3gx` plugin source (`build_plugin.ps1` / `build_plugin.bat`).
- `3gx_plugin_slim/` — Headless slim `.3gx` plugin for Old 3DS / extended memory games (`build_slim_plugin.ps1`).
- `3gx_plugin_hb/` & `CTRPluginFramework_Hb/` — Homebrew-compatible `.3gx` plugin build (`build_hb_plugin.ps1`).
- `dictionaries/` — Custom proper-noun glossary (`custom_dict.json`), JLPT & Kangxi radical metadata, plus downloaded open-licensed dictionaries (`jamdict.db`, `KanjiStrokeOrders.ttf`) and optional user-supplied Yomitan `.zip` dictionaries.
- `THIRD_PARTY_LICENSES.md` — Attributions and licenses for open-licensed dictionary datasets and fonts.
- `ocr_models/` — Model charsets/configs (`NDLmoji.yaml`, `japan_dict.txt`, `yomitoku_charset.txt`).

---

## 1. PC Server Setup

### Prerequisites
- Python 3.10+ (with Tkinter, included in standard Windows/macOS installers)
- *(Optional)* Tesseract OCR, Ollama (local LLM translation), or Anki Desktop + AnkiConnect (`127.0.0.1:8765`)

### Install Dependencies
```bash
pip install -r requirements.txt
```

### Configuration (Optional)
Copy the example config files if you want to customize settings before launching, or configure them directly inside the GUI:
- `anki_config.example.json` → `anki_config.json`
- `anki_credentials.example.json` → `anki_credentials.json` *(only needed for direct AnkiWeb sync)*
- `gemini_credentials.example.json` → `gemini_credentials.json` *(or set `GEMINI_API_KEY` / `GEMINI_FALLBACK_API_KEY` environment variables)*

> Per-3DS pre-shared encryption keys are managed automatically in `ocr_psks.json` (any existing `ocr_psk.txt` key is migrated automatically on first launch).

### Run
Start the desktop launcher:
```bash
python ocr_launcher.py
```
*(Or run the backend directly with `python ocr_server.py`.)*

### Dictionaries & First-Run Setup
- **Automatic First-Run Setup**: On first launch, if `dictionaries/jamdict.db` or `dictionaries/KanjiStrokeOrders.ttf` is missing, the launcher opens the **First-Run Dictionary Setup** dialog to download the open-licensed core dictionaries (**JMdict + KANJIDIC2 + JMnedict** under CC-BY-SA 4.0 and **KanjiStrokeOrders.ttf** under BSD-like / CC-BY-SA 3.0) from their official upstream sources.
- **Built-In Dictionary Updater**: Click **Update Dictionaries...** in the launcher at any time to refresh `jamdict.db`, `KanjiStrokeOrders.ttf`, or `kangxi_radicals.json`.
- **Optional (Bring Your Own Yomitan Dictionaries)**: Proprietary dictionaries (such as Babylon, Daijirin, or Kenkyusha) are intentionally excluded from automatic downloads for licensing reasons. You can drop any Yomitan-format `.zip` dictionary you already own into `dictionaries/` and click **Reload Dictionaries**.
- See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) for dataset attributions and licenses.

### Optional: Build Standalone Launcher Executable
```bash
pyinstaller OCR_Server.spec
```
Output is written to `dist/OCR_Server.exe`.

### Optional: Extra Local OCR Models
The server runs out of the box with `manga-ocr` / `rapidocr-onnxruntime`. To also enable PaddleOCR Japanese v4, YomiToku PARSeq-Small, or NDLOCR-Lite, place their model weights in `ocr_models/`:
- `japan_PP-OCRv4_rec.onnx`
- `yomitoku_parseq_small.safetensors`
- `ndlocr_parseq_16x384.onnx`

---

## 2. Building the 3DS Plugins (`.3gx`)

### Prerequisites
- [Docker](https://www.docker.com/) running locally (uses the `pablomk7/libctrpf:0.8.0` builder image — no local `devkitARM` install needed).

### Build Commands (PowerShell)
- **Full Plugin** (outputs to `full/default.3gx` and `full/user_param.bin`):
  ```powershell
  .\build_plugin.ps1
  ```
- **Slim / Extended-Memory Mode Plugin** (outputs to `slim/default.3gx`, `slim/default_slim.3gx`, and `slim/user_param.bin`):
  ```powershell
  .\build_slim_plugin.ps1
  ```
- **Homebrew-Compatible Plugin** (rebuilds `CTRPluginFramework_Hb` and outputs to `homebrew/`):
  ```powershell
  .\build_hb_plugin.ps1
  ```

*(Optional)* Set `$env:GDRIVE_DIR` before running any build script to automatically copy compiled `.3gx` files to a sync directory.

### 3DS Installation
1. Enable **Game Patching** and **Plugin Loader** in Luma3DS (`Select` on boot).
2. Copy the built `default.3gx` to `/luma/plugins/default.3gx` on your 3DS SD card (or `/luma/plugins/<TitleID>/default.3gx` for a specific game).
3. Launch the desktop server (`ocr_launcher.py`), pair your 3DS by scanning the QR code in the plugin, and press **`R + Start + Select`** in-game to capture and translate text.
4. Move the cursor using the **Circle Pad** or **C-Stick** (New 3DS), D-Pad fine-stepping, or directly by tapping the touch screen.
5. **Word Frequency & JLPT**: Word cards display their frequency ranking (e.g. `#195`) and JLPT level (e.g. `N5`) in the top banner instead of raw screen coordinates.

### Using Kanji Lookup
When a word translation popup is open on-screen, you can inspect any kanji in it without losing your current translation:

1. **Open Kanji Details**: Move the cursor over any kanji character in the translation popup and press **`A`** (or tap it directly on the touchscreen). A **Kanji Details** window will open on the **opposite screen** containing:
   - Numbered stroke order diagram (`KanjiStrokeOrders.ttf`)
   - JLPT level, school grade, and frequency rank
   - On (`音`), Kun (`訓`), and Nanori (`名`) readings
   - Stroke count and radical
   - English meanings (from Kanjidic)
2. **Scroll**: Use **D-Pad Up / Down** to scroll through long reading lists and meanings (if all details fit on one screen, scrolling is disabled).
3. **Add to Anki**: Press **`X`** *(or your configured Anki keybind)* while the Kanji Details window is open to create an Anki flashcard for that kanji.
4. **Close**: Press **`B`** (or click/tap a non-kanji area of the translation popup) to close only the Kanji Details window and return to your word popup. Press **`B`** again to close the word popup.

---

## 3. Multi-User Mode & Per-3DS Pairing

Multiple 3DS consoles can connect concurrently to the same server, each with its own encryption key and AnkiWeb account.

### Per-3DS PSKs & QR Pairing
- Each 3DS receives its own 256-bit pre-shared key stored in `ocr_psks.json`.
- When a 3DS scans the QR code in the launcher GUI, it immediately pings the server over the encrypted channel with its System Settings nickname, hardware model, and hardware-unique console ID.
- The server records the console in `ocr_psks.json` and automatically rotates the QR code to a fresh pending key for the next 3DS. *(If the server is offline during the QR scan, the 3DS registers itself automatically on its next OCR session.)*

### Managing Paired 3DSes
- Open the **Paired 3DSes** tab in `ocr_launcher.py` to view all paired consoles (3DS name, model, console ID, masked PSK, paired time, and last-seen time).
- You can **Rename** a console or **Invalidate PSK** to revoke a single 3DS's key—invalidating a PSK immediately rejects new handshakes and terminates any active session using that key without affecting other paired 3DSes.

### Server Mode: Single 3DS vs. Multiple 3DS
In the launcher's **Dictionaries & Anki Sync** card, switch **Server Mode** (`multi_user_mode` in `anki_config.json`):
- **Single 3DS** *(default)*: Uses the server's configured Anki Desktop (AnkiConnect) or AnkiWeb login unless the connecting 3DS has set its own AnkiWeb credentials.
- **Multiple 3DS**: Ignores the server's Anki login completely. Each connecting 3DS must use its own AnkiWeb credentials (synced through isolated per-account mirrors under `anki_sync/users/<hash>/` so concurrent sessions never cross accounts). If a 3DS without saved credentials tries to add a card, the server prompts it to open its AnkiWeb login dialog.

### Setting AnkiWeb Login on the 3DS
- **Full / Homebrew builds**: Press **`Select`** to open the Plugin Menu → **`AnkiWeb Login: [...]`**.
- **Slim build**: Press **`Start + Select + L`** (default `Slim Menu` hotkey) → **`AnkiWeb Login`**.
- **Mid-session (all builds)**: Press **`Start + Select + L`** *(or your configured `Slim Menu` hotkey)* while an OCR session is open to set, update, or clear credentials live; changes are immediately pushed to the active server session.

Credentials are saved on the 3DS SD card at `/luma/ocr_anki.txt` and transmitted over the X25519-encrypted session stream. Choosing **Clear (Use Server Default)** removes the per-3DS login.
