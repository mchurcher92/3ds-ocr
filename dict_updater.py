"""
dict_updater.py - Open-licensed dictionary & font downloader / updater.

Only fetches openly licensed, freely redistributable resources from official
upstream sources (proprietary dictionaries such as Babylon or commercial J-J
dictionaries are intentionally excluded; users may place their own Yomitan
`.zip` dictionaries into `dictionaries/`).

Managed open-licensed assets:
  1. JMdict + KANJIDIC2 + JMnedict (`dictionaries/jamdict.db`)
     - Source: PyPI `jamdict-data` (EDRDG)
     - License: CC-BY-SA 4.0
  2. Kanji Stroke Order Font (`dictionaries/KanjiStrokeOrders.ttf`)
     - Source: Debian DFSG mirror (`fonts-kanjistrokeorders`, KanjiVG / Tim Ernst)
     - License: BSD-like / CC-BY-SA 3.0
  3. 214 Kangxi Radicals (`dictionaries/kangxi_radicals.json`)
     - Source: Kanji alive (`japanese-radicals.csv`)
     - License: CC-BY 4.0
"""

from __future__ import annotations

import csv
import hashlib
import io
import json
import lzma
import os
import re
import sys
import tarfile
import threading
import time
import unicodedata
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional, Tuple

USER_AGENT = "3DS-OCR-Dictionary-Updater/1.0"
MANIFEST_FILENAME = ".dict_manifest.json"
DOWNLOAD_TIMEOUT_SEC = 30.0
CHUNK_SIZE = 65536


@dataclass(frozen=True)
class DictAssetSpec:
    asset_id: str
    title: str
    filename: str
    license_label: str
    attribution: str
    source_label: str
    approx_size: str
    required_for_first_run: bool = False
    description: str = ""
    category: str = "dictionary"      # "dictionary" or "ocr_model"
    target_subdir: str = "dictionaries"


OPEN_DICT_ASSETS: Tuple[DictAssetSpec, ...] = (
    DictAssetSpec(
        asset_id="jamdict",
        title="JMdict + KANJIDIC2 + JMnedict",
        filename="jamdict.db",
        license_label="CC-BY-SA 4.0 (EDRDG)",
        attribution="Electronic Dictionary Research and Development Group (EDRDG)",
        source_label="PyPI jamdict-data",
        approx_size="~54 MB download (~325 MB unpacked)",
        required_for_first_run=True,
        description="Core Japanese-English word dictionary, kanji readings/meanings, and proper names.",
        category="dictionary",
        target_subdir="dictionaries",
    ),
    DictAssetSpec(
        asset_id="kanji_stroke_font",
        title="Kanji Stroke Order Font",
        filename="KanjiStrokeOrders.ttf",
        license_label="BSD-like / CC-BY-SA 3.0 (KanjiVG)",
        attribution="Tim Ernst & KanjiVG project",
        source_label="Debian DFSG mirror",
        approx_size="~9 MB download (~18 MB unpacked)",
        required_for_first_run=True,
        description="Renders numbered stroke-order diagrams in the 3DS Kanji Details popup.",
        category="dictionary",
        target_subdir="dictionaries",
    ),
    DictAssetSpec(
        asset_id="kangxi_radicals",
        title="214 Kangxi Radicals Metadata",
        filename="kangxi_radicals.json",
        license_label="CC-BY 4.0 (Kanji alive)",
        attribution="Kanji alive (University of Chicago)",
        source_label="Kanji alive GitHub",
        approx_size="~18 KB",
        required_for_first_run=False,
        description="Japanese readings and English meanings for all 214 Kangxi radicals.",
        category="dictionary",
        target_subdir="dictionaries",
    ),
)

OPEN_OCR_MODEL_ASSETS: Tuple[DictAssetSpec, ...] = (
    DictAssetSpec(
        asset_id="paddle_jp_v4",
        title="PaddleOCR Japanese PP-OCRv4 (ONNX)",
        filename="japan_PP-OCRv4_rec.onnx",
        license_label="Apache 2.0 (Baidu PaddleOCR)",
        attribution="PaddleOCR / Baidu",
        source_label="HuggingFace (pinned v4)",
        approx_size="~9.7 MB",
        required_for_first_run=True,
        description="Fast Japanese line recognition engine for local ensemble OCR.",
        category="ocr_model",
        target_subdir="ocr_models",
    ),
    DictAssetSpec(
        asset_id="ndlocr_lite",
        title="NDLOCR-Lite PARSeq 16x384 (ONNX)",
        filename="ndlocr_parseq_16x384.onnx",
        license_label="CC-BY 4.0 (National Diet Library)",
        attribution="National Diet Library of Japan (NDL Lab)",
        source_label="GitHub ndl-lab/ndlocr-lite (v1.1.1)",
        approx_size="~36.9 MB",
        required_for_first_run=True,
        description="High-accuracy Japanese PARSeq recognizer from Japan's National Diet Library.",
        category="ocr_model",
        target_subdir="ocr_models",
    ),
    DictAssetSpec(
        asset_id="yomitoku_parseq_small",
        title="YomiToku PARSeq-Small (Safetensors)",
        filename="yomitoku_parseq_small.safetensors",
        license_label="CC-BY-NC-SA 4.0 (Non-Commercial)",
        attribution="Kotaro Kinoshita (YomiToku)",
        source_label="HuggingFace (official open-beta)",
        approx_size="~97.3 MB",
        required_for_first_run=False,
        description="Optional high-accuracy Japanese recognizer (non-commercial personal use only).",
        category="ocr_model",
        target_subdir="ocr_models",
    ),
)

ALL_ASSETS: Tuple[DictAssetSpec, ...] = OPEN_DICT_ASSETS + OPEN_OCR_MODEL_ASSETS
ASSETS_BY_ID: Dict[str, DictAssetSpec] = {a.asset_id: a for a in ALL_ASSETS}


PINNED_OCR_MODEL_SOURCES: Dict[str, Tuple[str, str, str]] = {
    # asset_id -> (https_url, expected_sha256, version_label)
    "paddle_jp_v4": (
        "https://huggingface.co/cycloneboy/japan_PP-OCRv4_rec_infer/resolve/main/model.onnx",
        "bb0f23444c059d27a615c1a426414240bbd02d3658900d0455e007dc47d3e4a4",
        "PP-OCRv4",
    ),
    "ndlocr_lite": (
        "https://raw.githubusercontent.com/ndl-lab/ndlocr-lite/1.1.1/src/model/parseq-ndl-16x384-50-tiny-146epoch-tegaki2.onnx",
        "1a60e88c9ffeaefdfe286677146f39fdeb4d0e1acd94ccd974c8943f761d9a08",
        "v1.1.1 (16x384)",
    ),
    "yomitoku_parseq_small": (
        "https://huggingface.co/KotaroKinoshita/yomitoku-text-recognizer-parseq-small-open-beta/resolve/main/model.safetensors",
        "a23f8e6870d1c23e999b66d86819b9fa5145a3bcfb141d61f13828bf9b50b2e4",
        "open-beta",
    ),
}


ProgressCallback = Callable[[int, int, str], None]


def get_dictionaries_dir(app_dir: str) -> str:
    return os.path.join(app_dir, "dictionaries")


def _get_manifest_path(app_dir: str) -> str:
    return os.path.join(get_dictionaries_dir(app_dir), MANIFEST_FILENAME)


def load_manifest(app_dir: str) -> Dict[str, Any]:
    path = _get_manifest_path(app_dir)
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
            if isinstance(data, dict):
                return data
        except Exception:
            pass
    return {}


def save_manifest(app_dir: str, data: Dict[str, Any]) -> None:
    dict_dir = get_dictionaries_dir(app_dir)
    os.makedirs(dict_dir, exist_ok=True)
    path = _get_manifest_path(app_dir)
    tmp_path = f"{path}.{os.getpid()}.{threading.get_ident()}.tmp"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    _atomic_replace(tmp_path, path)


def _atomic_replace(src_tmp: str, dst: str) -> None:
    for attempt in range(6):
        try:
            os.replace(src_tmp, dst)
            return
        except PermissionError:
            if attempt == 5:
                try:
                    os.remove(src_tmp)
                except OSError:
                    pass
                raise
            time.sleep(0.1 * (attempt + 1))


def _locate_existing_jamdict_db(app_dir: str) -> Optional[str]:
    """Check if jamdict.db already exists in dictionaries/, app_dir, site-packages, or ~/.jamdict."""
    candidates = [
        os.path.join(get_dictionaries_dir(app_dir), "jamdict.db"),
        os.path.join(app_dir, "jamdict.db"),
        os.path.join(sys.prefix, "lib", "site-packages", "jamdict_data", "jamdict.db"),
        os.path.expanduser("~/.jamdict/data/jamdict.db"),
    ]
    try:
        import site

        for sp in site.getsitepackages() + [site.getusersitepackages()]:
            candidates.append(os.path.join(sp, "jamdict_data", "jamdict.db"))
    except Exception:
        pass
    for c in candidates:
        if c and os.path.isfile(c) and os.path.getsize(c) > 1024 * 1024:
            return c
    return None


def get_asset_status(app_dir: str, asset_id: str) -> Dict[str, Any]:
    spec = ASSETS_BY_ID[asset_id]
    target_dir = os.path.join(app_dir, spec.target_subdir)
    local_path = os.path.join(target_dir, spec.filename)
    manifest = load_manifest(app_dir)
    meta = manifest.get(asset_id, {}) if isinstance(manifest.get(asset_id), dict) else {}

    installed_path: Optional[str] = None
    if os.path.isfile(local_path) and os.path.getsize(local_path) > 0:
        installed_path = local_path
    elif asset_id == "jamdict":
        installed_path = _locate_existing_jamdict_db(app_dir)

    size_bytes = os.path.getsize(installed_path) if installed_path else 0
    return {
        "asset_id": asset_id,
        "installed": installed_path is not None,
        "path": installed_path or local_path,
        "size_bytes": size_bytes,
        "version": str(meta.get("version", "") or ("Bundled/External" if installed_path else "")),
        "updated_at": str(meta.get("updated_at", "") or ""),
    }


def get_missing_first_run_assets(app_dir: str) -> List[str]:
    """Return asset_ids that are recommended for first-run setup and currently missing."""
    missing: List[str] = []
    for spec in ALL_ASSETS:
        if not spec.required_for_first_run:
            continue
        if not get_asset_status(app_dir, spec.asset_id)["installed"]:
            missing.append(spec.asset_id)
    return missing


def _require_https(url: str) -> None:
    if not isinstance(url, str) or not url.startswith("https://"):
        raise ValueError(f"Refusing non-HTTPS download URL: {url!r}")


def _http_get_bytes(url: str, timeout: float = DOWNLOAD_TIMEOUT_SEC) -> bytes:
    _require_https(url)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def _download_to_file(
    url: str,
    dest_path: str,
    progress_cb: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
    label: str = "Downloading...",
    expected_sha256: Optional[str] = None,
) -> None:
    _require_https(url)
    req = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    hasher = hashlib.sha256() if expected_sha256 else None
    downloaded = 0

    with urllib.request.urlopen(req, timeout=DOWNLOAD_TIMEOUT_SEC) as resp:
        total_hdr = resp.headers.get("Content-Length")
        total = int(total_hdr) if (total_hdr and total_hdr.isdigit()) else 0
        with open(dest_path, "wb") as out:
            while True:
                if cancel_event is not None and cancel_event.is_set():
                    raise RuntimeError("Download cancelled by user.")
                chunk = resp.read(CHUNK_SIZE)
                if not chunk:
                    break
                out.write(chunk)
                if hasher is not None:
                    hasher.update(chunk)
                downloaded += len(chunk)
                if progress_cb is not None:
                    progress_cb(downloaded, total, label)
            out.flush()
            os.fsync(out.fileno())

    if expected_sha256 and hasher is not None:
        actual = hasher.hexdigest().lower()
        if actual != expected_sha256.lower():
            raise RuntimeError(
                f"SHA-256 verification failed (expected {expected_sha256[:12]}..., got {actual[:12]}...)"
            )


def _extract_single_tar_member(
    archive_path: str,
    predicate: Callable[[str], bool],
    dest_path: str,
    progress_cb: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
    label: str = "Extracting...",
) -> None:
    """Safely extract a single matched file from a tar archive without path traversal."""
    tmp_out = f"{dest_path}.{os.getpid()}.{threading.get_ident()}.tmp"
    try:
        with tarfile.open(archive_path, "r:*") as tf:
            target_member = None
            for member in tf.getmembers():
                if not member.isfile():
                    continue
                if predicate(member.name):
                    target_member = member
                    break
            if target_member is None:
                raise RuntimeError(f"Expected file not found inside archive: {os.path.basename(archive_path)}")

            src_stream = tf.extractfile(target_member)
            if src_stream is None:
                raise RuntimeError("Failed to open archive member stream.")

            total = int(target_member.size or 0)
            is_xz = target_member.name.lower().endswith(".xz")
            decompressor = lzma.LZMADecompressor() if is_xz else None
            processed = 0
            with src_stream, open(tmp_out, "wb") as out:
                while True:
                    if cancel_event is not None and cancel_event.is_set():
                        raise RuntimeError("Extraction cancelled by user.")
                    chunk = src_stream.read(CHUNK_SIZE)
                    if not chunk:
                        break
                    processed += len(chunk)
                    if decompressor is not None:
                        data_out = decompressor.decompress(chunk)
                        if data_out:
                            out.write(data_out)
                    else:
                        out.write(chunk)
                    if progress_cb is not None:
                        progress_cb(processed, total, label)
                out.flush()
                os.fsync(out.fileno())

        _atomic_replace(tmp_out, dest_path)
    except Exception:
        try:
            if os.path.exists(tmp_out):
                os.remove(tmp_out)
        except OSError:
            pass
        raise


def _update_jamdict(
    app_dir: str,
    progress_cb: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
) -> str:
    if progress_cb:
        progress_cb(0, 0, "Checking latest jamdict-data release on PyPI...")
    meta = json.loads(_http_get_bytes("https://pypi.org/pypi/jamdict-data/json").decode("utf-8"))
    version = str(meta.get("info", {}).get("version", "unknown"))
    urls = meta.get("urls", [])
    chosen = None
    for entry in urls:
        if entry.get("packagetype") in ("sdist", "bdist_wheel") and str(entry.get("url", "")).startswith("https://"):
            if str(entry.get("filename", "")).endswith((".tar.gz", ".tar.xz", ".tgz")):
                chosen = entry
                break
    if not chosen:
        raise RuntimeError("Could not find a downloadable jamdict-data tarball on PyPI.")

    dl_url = str(chosen["url"])
    sha256 = str((chosen.get("digests") or {}).get("sha256", "") or "") or None
    dict_dir = get_dictionaries_dir(app_dir)
    os.makedirs(dict_dir, exist_ok=True)
    archive_tmp = os.path.join(dict_dir, f"jamdict_data.{os.getpid()}.{threading.get_ident()}.tmp")
    dest_db = os.path.join(dict_dir, "jamdict.db")

    try:
        _download_to_file(
            dl_url,
            archive_tmp,
            progress_cb=progress_cb,
            cancel_event=cancel_event,
            label=f"Downloading jamdict-data v{version}...",
            expected_sha256=sha256,
        )
        _extract_single_tar_member(
            archive_tmp,
            predicate=lambda name: os.path.basename(name) in ("jamdict.db", "jamdict.db.xz"),
            dest_path=dest_db,
            progress_cb=progress_cb,
            cancel_event=cancel_event,
            label=f"Extracting jamdict.db (v{version})...",
        )
    finally:
        try:
            if os.path.exists(archive_tmp):
                os.remove(archive_tmp)
        except OSError:
            pass

    return version


def _update_kanji_stroke_font(
    app_dir: str,
    progress_cb: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
) -> str:
    base_url = "https://deb.debian.org/debian/pool/main/f/fonts-kanjistrokeorders/"
    if progress_cb:
        progress_cb(0, 0, "Checking latest KanjiStrokeOrders release...")
    html = _http_get_bytes(base_url).decode("utf-8", errors="ignore")
    matches = sorted(set(re.findall(r'href="(fonts-kanjistrokeorders_[^"]+\.orig\.tar\.[^"]+)"', html)))
    if not matches:
        raise RuntimeError("Could not locate KanjiStrokeOrders tarball on Debian mirror.")
    filename = matches[-1]
    ver_m = re.search(r"fonts-kanjistrokeorders_([0-9][0-9.\-+a-zA-Z~]*)\.orig\.tar", filename)
    version = ver_m.group(1) if ver_m else filename
    dl_url = base_url + filename

    dict_dir = get_dictionaries_dir(app_dir)
    os.makedirs(dict_dir, exist_ok=True)
    archive_tmp = os.path.join(dict_dir, f"kso_font.{os.getpid()}.{threading.get_ident()}.tmp")
    dest_ttf = os.path.join(dict_dir, "KanjiStrokeOrders.ttf")

    try:
        _download_to_file(
            dl_url,
            archive_tmp,
            progress_cb=progress_cb,
            cancel_event=cancel_event,
            label=f"Downloading KanjiStrokeOrders ({version})...",
        )
        _extract_single_tar_member(
            archive_tmp,
            predicate=lambda name: name.lower().endswith(".ttf") and "kanjistrokeorders" in os.path.basename(name).lower(),
            dest_path=dest_ttf,
            progress_cb=progress_cb,
            cancel_event=cancel_event,
            label="Extracting KanjiStrokeOrders.ttf...",
        )
    finally:
        try:
            if os.path.exists(archive_tmp):
                os.remove(archive_tmp)
        except OSError:
            pass

    return version


def _update_kangxi_radicals(
    app_dir: str,
    progress_cb: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
) -> str:
    url = "https://raw.githubusercontent.com/kanjialive/kanji-data-media/master/language-data/japanese-radicals.csv"
    if progress_cb:
        progress_cb(0, 0, "Downloading Kanji alive Kangxi radicals CSV...")
    if cancel_event is not None and cancel_event.is_set():
        raise RuntimeError("Download cancelled by user.")
    raw = _http_get_bytes(url).decode("utf-8-sig")

    kangxi_nfkc_to_num: Dict[str, str] = {}
    for i in range(214):
        raw_ch = chr(0x2F00 + i)
        kangxi_nfkc_to_num[raw_ch] = str(i + 1)
        kangxi_nfkc_to_num[unicodedata.normalize("NFKC", raw_ch)] = str(i + 1)

    radicals: Dict[str, List[str]] = {}
    reader = csv.DictReader(io.StringIO(raw))
    for row in reader:
        rad_glyph = str(row.get("Radical", "") or "").strip()
        if not rad_glyph:
            continue
        ch = rad_glyph[0]
        num_str = kangxi_nfkc_to_num.get(ch) or kangxi_nfkc_to_num.get(unicodedata.normalize("NFKC", ch))
        if not num_str:
            continue
        reading = str(row.get("Reading", "") or "").strip()
        meaning = str(row.get("Meaning", "") or "").strip()
        if num_str == "149":
            reading = "げん・ごんべん"
        if num_str not in radicals:
            radicals[num_str] = [reading, meaning]
        elif reading and reading not in radicals[num_str][0]:
            radicals[num_str][0] = f"{radicals[num_str][0]}・{reading}" if radicals[num_str][0] else reading

    # Variant glyphs in Kanji alive CSV that don't normalize directly to 0x2F00..0x2FD5
    fallback_radicals = {
        "43": ["だいのまげあし", "lame leg"],
        "52": ["いとがしら", "young, slight"],
        "92": ["きば・きばへん", "fang, canine tooth"],
        "168": ["ながい", "long"],
    }
    for k, v in fallback_radicals.items():
        radicals.setdefault(k, list(v))

    if len(radicals) < 200:
        raise RuntimeError(f"Unexpected Kangxi radicals CSV format (parsed only {len(radicals)} entries).")

    dict_dir = get_dictionaries_dir(app_dir)
    os.makedirs(dict_dir, exist_ok=True)
    dest_json = os.path.join(dict_dir, "kangxi_radicals.json")
    tmp_path = f"{dest_json}.{os.getpid()}.{threading.get_ident()}.tmp"
    ordered = {str(k): radicals[str(k)] for k in sorted((int(x) for x in radicals.keys()))}
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(ordered, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    _atomic_replace(tmp_path, dest_json)
    if progress_cb:
        progress_cb(1, 1, f"Saved {len(ordered)} Kangxi radicals.")
    return f"{len(ordered)} radicals"


def _update_pinned_ocr_model(
    app_dir: str,
    asset_id: str,
    progress_cb: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
) -> str:
    spec = ASSETS_BY_ID[asset_id]
    url, expected_sha256, version = PINNED_OCR_MODEL_SOURCES[asset_id]
    target_dir = os.path.join(app_dir, spec.target_subdir)
    os.makedirs(target_dir, exist_ok=True)
    dest_path = os.path.join(target_dir, spec.filename)
    tmp_path = f"{dest_path}.{os.getpid()}.{threading.get_ident()}.tmp"

    try:
        _download_to_file(
            url,
            tmp_path,
            progress_cb=progress_cb,
            cancel_event=cancel_event,
            label=f"Downloading {spec.title} ({version})...",
            expected_sha256=expected_sha256,
        )
        _atomic_replace(tmp_path, dest_path)
    except Exception:
        try:
            if os.path.exists(tmp_path):
                os.remove(tmp_path)
        except OSError:
            pass
        raise

    return version


def download_or_update_asset(
    app_dir: str,
    asset_id: str,
    progress_cb: Optional[ProgressCallback] = None,
    cancel_event: Optional[threading.Event] = None,
) -> Dict[str, Any]:
    """Download/update a single open-licensed dictionary or OCR model asset and update manifest."""
    if asset_id == "jamdict":
        version = _update_jamdict(app_dir, progress_cb=progress_cb, cancel_event=cancel_event)
    elif asset_id == "kanji_stroke_font":
        version = _update_kanji_stroke_font(app_dir, progress_cb=progress_cb, cancel_event=cancel_event)
    elif asset_id == "kangxi_radicals":
        version = _update_kangxi_radicals(app_dir, progress_cb=progress_cb, cancel_event=cancel_event)
    elif asset_id in PINNED_OCR_MODEL_SOURCES:
        version = _update_pinned_ocr_model(
            app_dir, asset_id, progress_cb=progress_cb, cancel_event=cancel_event
        )
    else:
        raise ValueError(f"Unknown asset_id: {asset_id!r}")

    manifest = load_manifest(app_dir)
    manifest[asset_id] = {
        "version": version,
        "updated_at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    save_manifest(app_dir, manifest)
    return get_asset_status(app_dir, asset_id)
