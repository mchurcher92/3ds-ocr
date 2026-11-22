# Third-Party Licenses & Data Attributions

This project uses and optionally downloads the following openly licensed dictionary datasets, fonts, and libraries. Proprietary dictionaries (such as Babylon, Daijirin, Kenkyusha, etc.) are **not** bundled or downloaded by this project; users may optionally supply their own Yomitan-format `.zip` dictionaries locally.

---

## 1. JMdict / KANJIDIC2 / JMnedict / KRADFILE (via `jamdict-data`)
- **Copyright**: Electronic Dictionary Research and Development Group (EDRDG)
- **License**: Creative Commons Attribution-ShareAlike 4.0 International (**CC-BY-SA 4.0**)
- **Source**: https://www.edrdg.org/edrdg/licence.html / https://pypi.org/project/jamdict-data/
- **Usage**: Core Japanese-English word definitions, kanji readings/meanings, radical decomposition, and proper names (`dictionaries/jamdict.db`).

## 2. Kanji Stroke Order Font (`KanjiStrokeOrders.ttf`)
- **Copyright**: Tim Ernst & the KanjiVG project (Ulrich Apel)
- **License**: Permissive BSD-like font license & Creative Commons Attribution-ShareAlike 3.0 (**CC-BY-SA 3.0**, KanjiVG stroke order data)
- **Source**: https://sites.google.com/site/nihilistorguk/ / https://kanjivg.tagaini.net/ / Debian `fonts-kanjistrokeorders`
- **Usage**: Rendering numbered kanji stroke-order diagrams in the 3DS Kanji Details popup (`dictionaries/KanjiStrokeOrders.ttf`).

## 3. Kangxi Radicals Metadata (`dictionaries/kangxi_radicals.json`)
- **Copyright**: Kanji alive (University of Chicago)
- **License**: Creative Commons Attribution 4.0 International (**CC-BY 4.0**)
- **Source**: https://github.com/kanjialive/kanji-data-media
- **Usage**: Japanese readings and English meanings for all 214 Kangxi radicals.

## 4. Local OCR Models (Downloaded via Setup / Updater)

### PaddleOCR Japanese PP-OCRv4 (`ocr_models/japan_PP-OCRv4_rec.onnx`)
- **Copyright**: PaddleOCR Authors / Baidu
- **License**: **Apache License 2.0**
- **Source**: https://github.com/PaddlePaddle/PaddleOCR (ONNX export via HuggingFace)

### NDLOCR-Lite PARSeq (`ocr_models/ndlocr_parseq_16x384.onnx`)
- **Copyright**: National Diet Library of Japan (NDL Lab)
- **License**: Creative Commons Attribution 4.0 International (**CC-BY 4.0**)
- **Source**: https://github.com/ndl-lab/ndlocr-lite

### YomiToku PARSeq-Small (`ocr_models/yomitoku_parseq_small.safetensors`) *(Optional / Opt-In)*
- **Copyright**: Kotaro Kinoshita (YomiToku)
- **License**: Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (**CC-BY-NC-SA 4.0**) — *personal / non-commercial use only; commercial use requires permission from the author.*
- **Source**: https://huggingface.co/KotaroKinoshita/yomitoku-text-recognizer-parseq-small-open-beta

### RapidOCR & Manga-OCR
- **RapidOCR**: Apache License 2.0 (https://github.com/RapidAI/RapidOCR)
- **Manga-OCR**: Apache License 2.0 (https://github.com/kha-white/manga-ocr)

## 5. CTRPluginFramework & `quirc`
- **CTRPluginFramework**: Included under its respective license in `CTRPluginFramework/LICENSE.txt`.
- **quirc**: Copyright (C) 2010-2012 Daniel Beer (ISC License), used in the 3DS plugin for QR code pairing.
