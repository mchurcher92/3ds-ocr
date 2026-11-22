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

## 4. CTRPluginFramework & `quirc`
- **CTRPluginFramework**: Included under its respective license in `CTRPluginFramework/LICENSE.txt`.
- **quirc**: Copyright (C) 2010-2012 Daniel Beer (ISC License), used in the 3DS plugin for QR code pairing.
