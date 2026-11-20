# Nintendo 3DS Japanese OCR & Translation

Real-time dual-screen Japanese OCR, dictionary lookup, AI/neural translation, and Anki flashcard creation for Nintendo 3DS, using a lightweight Luma3DS `.3gx` plugin paired with a Python PC server.

## Repository Layout

- `ocr_launcher.py` — Desktop GUI launcher (QR code pairing, live OCR preview, settings, logs).
- `ocr_server.py` — Socket/OCR/translation/Anki backend server.
- `anki_dialog.py` & `ui_theme.py` — Configuration dialogs and dark-mode UI helpers.
- `3gx_plugin/` — Full CTRPluginFramework `.3gx` plugin source (`build_plugin.ps1` / `build_plugin.bat`).
- `3gx_plugin_slim/` — Headless slim `.3gx` plugin for Old 3DS / extended memory games (`build_slim_plugin.ps1`).
- `3gx_plugin_hb/` & `CTRPluginFramework_Hb/` — Homebrew-compatible `.3gx` plugin build (`build_hb_plugin.ps1`).
- `custom_dict.json` & `dictionaries/` — Custom proper-noun glossary and JMdict lookup support.
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

> A pre-shared encryption key (`ocr_psk.txt`) is generated automatically on first launch.

### Run
Start the desktop launcher:
```bash
python ocr_launcher.py
```
*(Or run the backend directly with `python ocr_server.py`.)*

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
