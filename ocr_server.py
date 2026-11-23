#!/usr/bin/env python3
"""
ocr_server.py - Nintendo 3DS Rosalina Dual-Screen OCR & Japanese Dictionary Server
Handles:
  1. SHA-256 challenge-response authentication (hmac.compare_digest, 2s timeout)
  2. Multi-threaded WAN-hardened socket handling with TCP_NODELAY & transfer timeouts
  3. Dual-screen raw RGB24 framebuffer reception (400x240 Top, 320x240 Bottom)
  4. Full-screen layout-aware OCR with multi-line paragraph stitching (handles line wraps,
     long multi-line conjugations, horizontal & vertical Japanese text, and mid-word clicks)
  5. Full-sentence morphological analysis (Fugashi/MeCab) + JMdict lookup (Jamdict)
  6. Dynamic 320x240 TTF dictionary popup rendering onto the opposite 3DS screen
"""

from dataclasses import dataclass, field
from contextlib import contextmanager
import base64
import hashlib
import hmac
import html
import http.client
import http.server
import io
import json
import logging
import math
import os
import re
import secrets
import socket
import sqlite3
import struct
import sys
import textwrap
import threading
import time
import unicodedata
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib
import functools
import pickle
from collections import deque
from typing import Any, Callable, Dict, List, Optional, Tuple, Union
import numpy as np
import torch
from PIL import Image, ImageDraw, ImageFont, ImageOps
import psk_store

# Server & Protocol Configuration
HOST = "0.0.0.0"
PORT = 8888

if getattr(sys, "frozen", False):
    APP_DIR = os.path.dirname(os.path.abspath(sys.executable))
else:
    APP_DIR = os.path.dirname(os.path.abspath(__file__))

DICTIONARIES_DIR = os.environ.get("OCR_DICTIONARIES_DIR", os.path.join(APP_DIR, "dictionaries"))
WORD_META_DB_PATH = os.path.join(DICTIONARIES_DIR, "word_meta.db")
JLPT_KANJI_PATH = os.path.join(DICTIONARIES_DIR, "jlpt_kanji.json")
KANJI_STROKE_FONT_PATH = os.path.join(DICTIONARIES_DIR, "KanjiStrokeOrders.ttf")
OCR_PSK_PATH = os.path.join(APP_DIR, "ocr_psk.txt")
ANKI_CONFIG_PATH = os.path.join(APP_DIR, "anki_config.json")
ANKI_CREDENTIALS_PATH = os.path.join(APP_DIR, "anki_credentials.json")
GEMINI_CREDENTIALS_PATH = os.path.join(APP_DIR, "gemini_credentials.json")
ANKI_SYNC_DIR = os.path.join(APP_DIR, "anki_sync")
TITLE_CACHE_PATH = os.path.join(ANKI_SYNC_DIR, "title_cache.json")

# Monkey-patch puchikarui ExecutionContext to open SQLite in read-only URI mode on Windows
# to prevent "sqlite3.OperationalError: unable to open database file" caused by journal
# lock conflicts or permission restrictions in Python site-packages.
try:
    import puchikarui
    from pathlib import Path

    _orig_puchikarui_exec_init = puchikarui.ExecutionContext.__init__

    def _safe_puchikarui_exec_init(self, path, schema, auto_commit=True):
        path_str = str(path) if path is not None else ""
        if path_str and path_str != ":memory:" and os.path.isfile(path_str):
            try:
                uri = Path(os.path.abspath(path_str)).as_uri() + "?mode=ro"
                self.conn = sqlite3.connect(uri, uri=True, check_same_thread=False)
                self.conn.row_factory = sqlite3.Row
                self.cur = self.conn.cursor()
                self.schema = schema
                self.auto_commit = auto_commit
                return
            except Exception:
                pass
        _orig_puchikarui_exec_init(self, path, schema, auto_commit=auto_commit)

    puchikarui.ExecutionContext.__init__ = _safe_puchikarui_exec_init
except ImportError:
    pass


def find_ocr_models_dir() -> str:
    """Locate the local `ocr_models/` directory containing PaddleOCR, YomiToku, and NDLOCR weights."""
    candidates = [
        os.path.join(APP_DIR, "ocr_models"),
        os.path.join(getattr(sys, "_MEIPASS", ""), "ocr_models"),
        os.environ.get("OCR_MODELS_DIR", ""),
    ]
    for p in candidates:
        if p and os.path.isdir(p):
            return os.path.abspath(p)
    return os.path.join(APP_DIR, "ocr_models")


def find_jamdict_db_path() -> Optional[str]:
    """Locate jamdict.db across dictionaries directory, app directory, PyInstaller _MEIPASS, site-packages, or home dir."""
    candidates = [
        os.path.join(DICTIONARIES_DIR, "jamdict.db"),
        os.path.join(APP_DIR, "jamdict.db"),
        os.path.join(getattr(sys, "_MEIPASS", ""), "jamdict_data", "jamdict.db"),
        os.path.join(getattr(sys, "_MEIPASS", ""), "jamdict", "data", "jamdict.db"),
        os.environ.get("JAMDICT_DB_PATH", ""),
        os.path.join(sys.prefix, "lib", "site-packages", "jamdict_data", "jamdict.db"),
        os.path.expanduser("~/.jamdict/data/jamdict.db"),
    ]
    # Check current user's local python installations on Windows dynamically
    local_appdata = os.environ.get("LOCALAPPDATA", "")
    if local_appdata:
        py_ver = f"Python{sys.version_info.major}{sys.version_info.minor}"
        candidates.append(os.path.join(local_appdata, "Programs", "Python", py_ver, "lib", "site-packages", "jamdict_data", "jamdict.db"))

    try:
        import site
        for sp in site.getsitepackages():
            candidates.append(os.path.join(sp, "jamdict_data", "jamdict.db"))
    except Exception:
        pass
    try:
        import jamdict_data
        if hasattr(jamdict_data, "JAMDICT_DB_PATH"):
            candidates.insert(1, jamdict_data.JAMDICT_DB_PATH)
    except Exception:
        pass

    for p in candidates:
        if p and os.path.isfile(p):
            return os.path.abspath(p)
    return None


def secure_write_psk(new_psk: str) -> None:
    """Write new PSK to ocr_psk.txt and lock down NTFS / POSIX permissions."""
    tmp_path = OCR_PSK_PATH + ".tmp"
    with open(tmp_path, "w", encoding="utf-8") as f:
        f.write(new_psk + "\n")

    if sys.platform == "win32":
        try:
            username = os.environ.get("USERNAME", "")
            if username and re.match(r"^[\w\-. ]+$", username):
                subprocess.run(
                    ["icacls", tmp_path, "/inheritance:r", "/grant:r", f"{username}:(R,W)"],
                    capture_output=True,
                    check=False,
                )
        except Exception:
            pass
    else:
        try:
            os.chmod(tmp_path, 0o600)
        except Exception:
            pass

    try:
        os.replace(tmp_path, OCR_PSK_PATH)
    except Exception:
        with open(OCR_PSK_PATH, "w", encoding="utf-8") as f:
            f.write(new_psk + "\n")


def load_ocr_psk() -> bytes:
    """Load OCR Pre-Shared Key dynamically from environment or protected file."""
    env_psk = os.environ.get("OCR_PSK", "").strip()
    if env_psk:
        return env_psk.encode("utf-8")
    if os.path.exists(OCR_PSK_PATH):
        try:
            with open(OCR_PSK_PATH, "r", encoding="utf-8") as f:
                content = f.read().strip()
                if content:
                    return content.encode("utf-8")
        except Exception as exc:
            logger.warning("Could not read %s: %s", OCR_PSK_PATH, exc)
    new_psk = secrets.token_urlsafe(32)
    try:
        secure_write_psk(new_psk)
        logger.info("Generated new random OCR PSK and saved with restricted permissions to %s", OCR_PSK_PATH)
    except Exception as exc:
        logger.warning("Could not persist generated PSK to %s: %s", OCR_PSK_PATH, exc)
    return new_psk.encode("utf-8")


SHA256_SIZE = 32

# WAN Timeouts & Anti-Exhaustion Hardening Limits
AUTH_TIMEOUT_SEC = 5.0
FB_TRANSFER_TIMEOUT_SEC = 20.0
IDLE_SESSION_TIMEOUT_SEC = 600.0
MAX_CONCURRENT_CONNECTIONS = 16
MAX_PREAUTH_CONNECTIONS = 32
MAX_PREAUTH_PER_IP = 4
MAX_AUTH_FAILURES_PER_IP = 3
AUTH_BAN_DURATION_SEC = 600.0
SCANNER_BAN_DURATION_SEC = 3600.0
MIN_OCR_INTERVAL_SEC = 0.45
MAX_ANKI_CRED_FIELD_LEN = 256
MAX_DEVICE_INFO_LEN = 512
PAIRING_PING_MAGIC = b"PAIRPING"

# 3DS Screen Dimensions & Formats
SCREEN_ID_TOP = 0
SCREEN_ID_BOTTOM = 1

# Navigation & Mode Command Opcodes (sent in byte 0 of the 5-byte client packet)
CMD_PREV_ENTRY = 0x10     # L button: Previous dictionary match
CMD_NEXT_ENTRY = 0x11     # R button: Next dictionary match
CMD_SCROLL_UP = 0x12      # D-Pad Up: Scroll definition lines up
CMD_SCROLL_DOWN = 0x13    # D-Pad Down: Scroll definition lines down
CMD_POS_LEFT = 0x14       # D-Pad Left: Scroll grammar note / POS header left
CMD_POS_RIGHT = 0x15      # D-Pad Right: Scroll grammar note / POS header right
CMD_ADD_ANKI = 0x16       # X button: Add current dictionary match to game's Anki deck
CMD_SET_MODE = 0x17       # Select button: Toggle Dictionary Lookup (0) vs Selection Translation (1)
CMD_SET_KEY_LABELS = 0x18 # Sync custom button labels for popup footer
CMD_PREV_OCR = 0x19       # ZL button: Cycle to previous differing OCR engine output
CMD_NEXT_OCR = 0x1A       # ZR button: Cycle to next differing OCR engine output
CMD_SET_CLIENT_CAPS = 0x1B # Client capability handshake (bit 0 = POPUP_BUNDLE, bit 1 = SCREEN_BUNDLE)
CMD_ADD_ANKI_TOP = 0x1C    # X button on cached Top screen spot: x=spot_id, y=(v_idx<<8)|e_idx
CMD_ADD_ANKI_BOT = 0x1D    # X button on cached Bottom screen spot: x=spot_id, y=(v_idx<<8)|e_idx
CMD_TRANSLATE_TOP = 0x20  # 2-point selection box on Top Screen (2x 5-byte frames: x1,y1 then x2,y2)
CMD_TRANSLATE_BOT = 0x21  # 2-point selection box on Bottom Screen (2x 5-byte frames: x1,y1 then x2,y2)
CMD_LOOKUP_KANJI = 0x22   # Kanji click in translation overlay: [0x22, cp_high16, cp_low16]
CMD_ADD_ANKI_KANJI = 0x23 # Add currently viewed kanji to Anki deck
CMD_SET_ANKI_CREDS = 0x24 # Client AnkiWeb credentials: [0x24, u_len_hi, u_len_lo, p_len_hi, p_len_lo] + (user||pass)
CMD_SET_DEVICE_INFO = 0x25 # Client device identity: [0x25, len_hi, len_lo, 0, 0] + (id\nname\nmodel)
CMD_CLICK_KANJI_WIN = 0x26 # Click/tap inside Kanji Details window: [0x26, popX_hi, popX_lo, tallY_hi, tallY_lo]
POPUP_BUNDLE_FLAG = 0x80
POPUP_STATUS_FLAG = 0x40
POPUP_SCREEN_BUNDLE_FLAG = 0x20
POPUP_KANJI_FLAG = 0x10
POPUP_ANKI_LOGIN_PROMPT_FLAG = 0x08

OCR_ENGINE_NAMES: Tuple[str, ...] = ("Manga", "Paddle", "Yomi", "NDL")

def _char_code_to_key_str(code: int) -> str:
    try:
        ch = chr(code)
    except Exception:
        return "A"
    mapping = {
        'S': "SEL",
        'T': "STA",
        'A': "A",
        'B': "B",
        'X': "X",
        'Y': "Y",
        'L': "L",
        'R': "R",
        '1': "ZL",
        '2': "ZR",
    }
    return mapping.get(ch, ch)

GLOSS_VISIBLE_LINES = 7
GLOSS_SCROLL_STEP = 2
POS_SCROLL_STEP = 6
TOP_DIM = (400, 240)
BOT_DIM = (320, 240)
POPUP_DIM = (320, 240)
KANJI_TALL_DIM = (320, 400)   # Full tall canvas sent to 3DS; plugin crops to 240px and scrolls locally
BPP = 3

TOP_FB_BYTES = TOP_DIM[0] * TOP_DIM[1] * BPP        # 288,000 bytes
BOT_FB_BYTES = BOT_DIM[0] * BOT_DIM[1] * BPP        # 230,400 bytes
POPUP_FB_BYTES = POPUP_DIM[0] * POPUP_DIM[1] * BPP  # 230,400 bytes

OCR_UPSCALE_FACTOR = 3
MAX_CLICK_DISTANCE_PX = 26.0  # Max cursor distance from a character box to count as a hit

# Packet struct definitions (Network Big-Endian)
TITLE_ID_FMT = "!Q"         # [Title_ID: uint64] (8 bytes)
TITLE_ID_SIZE = struct.calcsize(TITLE_ID_FMT)
CLICK_PACKET_FMT = "!BHH"   # [Screen_ID: uint8, X: uint16, Y: uint16] (5 bytes)
CLICK_PACKET_SIZE = struct.calcsize(CLICK_PACKET_FMT)
POPUP_HEADER_FMT = "!BI"    # [Target_Screen_ID: uint8, Image_Size: uint32] (5 bytes)

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
)
logger = logging.getLogger("3ds_ocr_server")
try:
    _file_handler = logging.FileHandler(os.path.join(APP_DIR, "ocr_server.log"), mode="a", encoding="utf-8")
    _file_handler.setFormatter(logging.Formatter("%(asctime)s [%(levelname)s] %(message)s"))
    logger.addHandler(_file_handler)
except Exception:
    pass

# Regex matching Japanese characters (Kanji, Hiragana, Katakana, prolonged sound mark, iteration marks)
JP_CHAR_RE = re.compile(r"[\u3005\u3040-\u309F\u30A0-\u30FF\u30FC\u4E00-\u9FFF]+")


def katakana_to_hiragana(s: str) -> str:
    """Convert Japanese katakana characters to hiragana."""
    return "".join(
        chr(ord(c) - 0x60) if 0x30A1 <= ord(c) <= 0x30F6 else c
        for c in s
    )


def is_noise_chunk(text: str) -> bool:
    """
    Check if a recognized OCR text chunk is noise/hallucination rather than genuine dialogue.
    Returns True for:
      - Empty strings
      - Strings consisting purely of whitespace, punctuation, quotes, or symbols (e.g. '"', ':', '...', '---', '::')
      - Non-Japanese alphanumeric noise (e.g. ':1', '1', '5a', '1a', '2b', 'v', 'x', isolated 1-4 digits)
    Returns False for:
      - Any string containing Japanese characters (Kana/Kanji)
      - Legitimate game UI / acronyms (e.g. 'HP', 'EXP', 'PP', 'LV', 'Lv', 'MAX', 'START', 'OK', 'NO')
    """
    if not text:
        return True
    if JP_CHAR_RE.search(text):
        return False
    # Strip all whitespace, punctuation, quotes, brackets, and symbols
    inner = re.sub(r"[\s\W_]+", "", text)
    if not inner:
        return True
    # Short alphanumeric noise patterns:
    # 1. Any mixed letters & digits (e.g. '5a', '1a', '2b', 'a1')
    # 2. Pure digits without Japanese counters (e.g. '1', '12', '222', '777')
    # 3. Short lowercase letter fragments (1-3 chars, e.g. 'v', 'x', 'a', 'abc')
    if re.match(r"^(?:\d+[a-zA-Z]|[a-zA-Z]+\d|[a-z]{1,3}|\d{1,4})$", inner):
        return True
    return False



@dataclass
class DetectedLine:
    """A single detected text line on the 3DS screen (in native 3DS pixel coordinates)."""
    bbox: Tuple[int, int, int, int]  # (x1, y1, x2, y2)
    text: str
    is_vertical: bool
    has_leading_punct: bool = False
    has_trailing_punct: bool = False
    raw_text: str = ""
    engine_texts: Dict[str, str] = field(default_factory=dict)
    engine_raw_texts: Dict[str, str] = field(default_factory=dict)
    alt_texts: List[str] = field(default_factory=list)
    ocr_source: str = "local"

    @property
    def width(self) -> int:
        return max(1, self.bbox[2] - self.bbox[0])

    @property
    def height(self) -> int:
        return max(1, self.bbox[3] - self.bbox[1])

    @property
    def center(self) -> Tuple[float, float]:
        return ((self.bbox[0] + self.bbox[2]) / 2.0, (self.bbox[1] + self.bbox[3]) / 2.0)


@dataclass
class CharSpan:
    """Maps a single character in a stitched multi-line paragraph to its 3DS screen bounding box."""
    char: str
    bbox: Tuple[float, float, float, float]  # (x1, y1, x2, y2) in native 3DS coordinates
    alt_texts: List[str] = field(default_factory=list)

    def distance_to(self, x: float, y: float) -> float:
        """Euclidean distance from point (x, y) to this character's bounding box (0 if inside)."""
        x1, y1, x2, y2 = self.bbox
        dx = max(x1 - x, 0.0, x - x2)
        dy = max(y1 - y, 0.0, y - y2)
        # On a 3DS touchscreen/cursor, allow horizontal leeway along the line
        return math.hypot(dx * 0.65, dy)


@dataclass
class MorphemeSpan:
    """
    A word/conjugation span inside a stitched multi-line paragraph.
    Groups content words with trailing auxiliary verbs/particles so long multi-line
    conjugations map back to the root word's start index and dictionary lemma.
    """
    start_idx: int
    end_idx: int
    surface: str
    lemma: str
    reading: str
    kanji_lemma: str = ""
    pos1: str = ""
    conjugation_steps: List[str] = field(default_factory=list)


@dataclass
class StitchedParagraph:
    """
    A multi-line paragraph where wrapped lines have been concatenated into a continuous
    logical string (`full_text`), preserving per-character screen coordinates (`chars`)
    and full-sentence morphological spans (`morphemes`).
    """
    bbox: Tuple[int, int, int, int]
    full_text: str
    chars: List[CharSpan]
    morphemes: List[MorphemeSpan]
    alt_phrases: Dict[int, List[str]] = field(default_factory=dict)
    ocr_source: str = "local"


@dataclass
class DictionaryResult:
    """Structured result from OCR + multi-line morphological analysis + JMdict lookup."""
    matched_surface: str
    context_text: str
    headword: str
    reading: str
    pos: str
    glosses: List[str] = field(default_factory=list)
    word_bboxes: List[Tuple[float, float, float, float]] = field(default_factory=list)
    found: bool = False
    ocr_disagreement: bool = False
    ocr_variant_idx: int = 0
    ocr_total_variants: int = 1
    ocr_engines: List[str] = field(default_factory=list)
    ocr_summary_line: str = ""
    rank_jlpt: str = ""
    _wrapped_lines: Optional[List[str]] = field(default=None, repr=False, compare=False)


@dataclass
class OcrVariant:
    """Represents one distinct OCR output (and its translations/dictionary entries) across the 4 OCR engines."""
    ocr_text: str
    engines: List[str]
    results: List[DictionaryResult]


@dataclass
class DeinflectionCandidate:
    root: str
    steps: List[str]
    required_pos: set[str]


@dataclass
class HybridBatchHandle:
    """Tracks a parallel Local + AI hybrid OCR pass across one or more screens."""
    gemini_ready_evt: threading.Event = field(default_factory=threading.Event)
    local_all_done_evt: threading.Event = field(default_factory=threading.Event)
    any_batch_ready_evt: threading.Event = field(default_factory=threading.Event)
    gemini_succeeded: bool = False


class JapaneseDeinflector:
    """
    Yomitan-compatible recursive Japanese deinflection engine.
    Transforms conjugated verb, adjective, auxiliary, and suru forms back to their
    dictionary lemmas (e.g. 'いやしてくれる' -> '癒す' [-te kureru, te-form],
    '回復した' -> '回復' [suru, past], '勝負しよう' -> '勝負' [suru, volitional]).
    """

    def __init__(self) -> None:
        self.rules: List[Tuple[str, str, List[str], set[str]]] = [
            # Auxiliary chains (-te / -de attachments)
            ("ても", "て", ["even if (-temo)"], {"v_te"}),
            ("でも", "で", ["even if (-temo)"], {"v_de"}),
            ("ては", "て", ["-te wa"], {"v_te"}),
            ("では", "で", ["-de wa"], {"v_de"}),
            ("なくても", "ない", ["even if (-temo)"], {"adj-i"}),
            ("くても", "い", ["even if (-temo)"], {"adj-i"}),

            ("てくれる", "て", ["-te kureru"], {"v_te"}),
            ("てくれた", "て", ["-te kureru", "past"], {"v_te"}),
            ("てくれない", "て", ["-te kureru", "negative"], {"v_te"}),
            ("てくれ", "て", ["-te kureru", "imperative"], {"v_te"}),
            ("でくれる", "で", ["-te kureru"], {"v_de"}),
            ("でくれた", "で", ["-te kureru", "past"], {"v_de"}),
            ("でくれない", "で", ["-te kureru", "negative"], {"v_de"}),
            ("でくれ", "で", ["-te kureru", "imperative"], {"v_de"}),

            ("ている", "て", ["continuous"], {"v_te"}),
            ("ていた", "て", ["continuous", "past"], {"v_te"}),
            ("ていない", "て", ["continuous", "negative"], {"v_te"}),
            ("ていなかった", "て", ["continuous", "negative past"], {"v_te"}),
            ("ています", "て", ["continuous", "polite"], {"v_te"}),
            ("ていました", "て", ["continuous", "polite past"], {"v_te"}),
            ("ていません", "て", ["continuous", "polite negative"], {"v_te"}),
            ("ておる", "て", ["continuous (humble)"], {"v_te"}),
            ("ており", "て", ["continuous (humble)"], {"v_te"}),
            ("ております", "て", ["continuous (humble)", "polite"], {"v_te"}),
            ("ておりました", "て", ["continuous (humble)", "polite past"], {"v_te"}),
            ("ておりません", "て", ["continuous (humble)", "polite negative"], {"v_te"}),
            ("てる", "て", ["continuous"], {"v_te"}),
            ("てた", "て", ["continuous", "past"], {"v_te"}),
            ("てない", "て", ["continuous", "negative"], {"v_te"}),
            ("てなかった", "て", ["continuous", "negative past"], {"v_te"}),
            ("でいる", "で", ["continuous"], {"v_de"}),
            ("でいた", "で", ["continuous", "past"], {"v_de"}),
            ("でいない", "で", ["continuous", "negative"], {"v_de"}),
            ("でいなかった", "で", ["continuous", "negative past"], {"v_de"}),
            ("でいます", "で", ["continuous", "polite"], {"v_de"}),
            ("でいました", "で", ["continuous", "polite past"], {"v_de"}),
            ("でおる", "で", ["continuous (humble)"], {"v_de"}),
            ("でおり", "で", ["continuous (humble)"], {"v_de"}),
            ("でおります", "で", ["continuous (humble)", "polite"], {"v_de"}),
            ("でおりました", "で", ["continuous (humble)", "polite past"], {"v_de"}),
            ("でる", "で", ["continuous"], {"v_de"}),
            ("でた", "で", ["continuous", "past"], {"v_de"}),
            ("でない", "で", ["continuous", "negative"], {"v_de"}),

            ("てくださる", "て", ["-te kudasaru"], {"v_te"}),
            ("てくださり", "て", ["-te kudasaru"], {"v_te"}),
            ("てくださった", "て", ["-te kudasaru", "past"], {"v_te"}),
            ("てください", "て", ["polite request (-te kudasai)"], {"v_te"}),
            ("でくださる", "で", ["-te kudasaru"], {"v_de"}),
            ("でくださり", "で", ["-te kudasaru"], {"v_de"}),
            ("でくださった", "で", ["-te kudasaru", "past"], {"v_de"}),
            ("でください", "で", ["polite request (-te kudasai)"], {"v_de"}),

            ("ていく", "て", ["-te iku"], {"v_te"}),
            ("ていった", "て", ["-te iku", "past"], {"v_te"}),
            ("ていかない", "て", ["-te iku", "negative"], {"v_te"}),
            ("てく", "て", ["-te iku"], {"v_te"}),
            ("てった", "て", ["-te iku", "past"], {"v_te"}),
            ("でいく", "で", ["-te iku"], {"v_de"}),
            ("でいった", "で", ["-te iku", "past"], {"v_de"}),
            ("でく", "で", ["-te iku"], {"v_de"}),

            ("てくる", "て", ["-te kuru"], {"v_te"}),
            ("てきた", "て", ["-te kuru", "past"], {"v_te"}),
            ("てこない", "て", ["-te kuru", "negative"], {"v_te"}),
            ("でくる", "で", ["-te kuru"], {"v_de"}),
            ("できた", "で", ["-te kuru", "past"], {"v_de"}),

            ("てしまう", "て", ["completion (-te shimau)"], {"v_te"}),
            ("てしまった", "て", ["completion (-te shimau)", "past"], {"v_te"}),
            ("ちゃう", "て", ["completion (-te shimau)"], {"v_te"}),
            ("ちゃった", "て", ["completion (-te shimau)", "past"], {"v_te"}),
            ("でしまう", "で", ["completion (-te shimau)"], {"v_de"}),
            ("でしまった", "で", ["completion (-te shimau)", "past"], {"v_de"}),
            ("じゃう", "で", ["completion (-te shimau)"], {"v_de"}),
            ("じゃった", "で", ["completion (-te shimau)", "past"], {"v_de"}),

            ("ておく", "て", ["preparatory (-te oku)"], {"v_te"}),
            ("ておいた", "て", ["preparatory (-te oku)", "past"], {"v_te"}),
            ("とく", "て", ["preparatory (-te oku)"], {"v_te"}),
            ("といた", "て", ["preparatory (-te oku)", "past"], {"v_te"}),
            ("でおく", "で", ["preparatory (-te oku)"], {"v_de"}),
            ("どく", "で", ["preparatory (-te oku)"], {"v_de"}),

            ("てみる", "て", ["-te miru"], {"v_te"}),
            ("てみた", "て", ["-te miru", "past"], {"v_te"}),
            ("でみる", "で", ["-te miru"], {"v_de"}),
            ("でみた", "で", ["-te miru", "past"], {"v_de"}),

            ("てあげる", "て", ["-te ageru"], {"v_te"}),
            ("てあげた", "て", ["-te ageru", "past"], {"v_te"}),
            ("であげる", "で", ["-te ageru"], {"v_de"}),
            ("であげた", "で", ["-te ageru", "past"], {"v_de"}),

            ("てもらう", "て", ["-te morau"], {"v_te"}),
            ("てもらった", "て", ["-te morau", "past"], {"v_te"}),
            ("でもらう", "で", ["-te morau"], {"v_de"}),
            ("でもらった", "で", ["-te morau", "past"], {"v_de"}),

            ("てほしい", "て", ["desiderative"], {"v_te"}),
            ("てほしかった", "て", ["desiderative", "past"], {"v_te"}),
            ("でほしい", "で", ["desiderative"], {"v_de"}),

            # Te-form / De-form to base
            ("して", "す", ["te-form"], {"v5s"}),
            ("して", "", ["suru", "te-form"], {"vs"}),
            ("いて", "く", ["te-form"], {"v5k"}),
            ("いで", "ぐ", ["te-form"], {"v5g"}),
            ("って", "う", ["te-form"], {"v5u"}),
            ("って", "つ", ["te-form"], {"v5t"}),
            ("って", "る", ["te-form"], {"v5r"}),
            ("って", "く", ["te-form"], {"v5k-s"}),
            ("んで", "む", ["te-form"], {"v5m"}),
            ("んで", "ぶ", ["te-form"], {"v5b"}),
            ("んで", "ぬ", ["te-form"], {"v5n"}),
            ("て", "る", ["te-form"], {"v1"}),
            ("くて", "い", ["te-form"], {"adj-i"}),

            # Past / Ta-form to base
            ("した", "す", ["past"], {"v5s"}),
            ("した", "", ["suru", "past"], {"vs"}),
            ("いた", "く", ["past"], {"v5k"}),
            ("いだ", "ぐ", ["past"], {"v5g"}),
            ("った", "う", ["past"], {"v5u"}),
            ("った", "つ", ["past"], {"v5t"}),
            ("った", "る", ["past"], {"v5r"}),
            ("った", "く", ["past"], {"v5k-s"}),
            ("んだ", "む", ["past"], {"v5m"}),
            ("んだ", "ぶ", ["past"], {"v5b"}),
            ("んだ", "ぬ", ["past"], {"v5n"}),
            ("た", "る", ["past"], {"v1"}),
            ("かった", "い", ["past"], {"adj-i"}),

            # Standalone irregular Suru verb forms (maps to 'する' with POS vs-i)
            ("しても", "する", ["te-form", "even if (-temo)"], {"vs-i"}),
            ("しなくても", "する", ["negative", "even if (-temo)"], {"vs-i"}),
            ("して", "する", ["te-form"], {"vs-i"}),
            ("した", "する", ["past"], {"vs-i"}),
            ("しない", "する", ["negative"], {"vs-i"}),
            ("しなかった", "する", ["negative past"], {"vs-i"}),
            ("します", "する", ["polite"], {"vs-i"}),
            ("しました", "する", ["polite past"], {"vs-i"}),
            ("しません", "する", ["polite negative"], {"vs-i"}),
            ("しませんでした", "する", ["polite neg past"], {"vs-i"}),
            ("しよう", "する", ["volitional"], {"vs-i"}),
            ("すれば", "する", ["conditional (-eba)"], {"vs-i"}),
            ("したら", "する", ["conditional (-tara)"], {"vs-i"}),
            ("しろ", "する", ["imperative"], {"vs-i"}),
            ("せよ", "する", ["imperative"], {"vs-i"}),
            ("される", "する", ["passive"], {"vs-i"}),
            ("された", "する", ["passive past"], {"vs-i"}),
            ("されない", "する", ["passive negative"], {"vs-i"}),
            ("されなかった", "する", ["passive neg past"], {"vs-i"}),
            ("させる", "する", ["causative"], {"vs-i"}),
            ("させた", "する", ["causative past"], {"vs-i"}),
            ("させない", "する", ["causative negative"], {"vs-i"}),
            ("させられる", "する", ["causative-passive"], {"vs-i"}),
            ("させられた", "する", ["causative-passive past"], {"vs-i"}),
            ("したい", "する", ["desiderative (-tai)"], {"vs-i"}),
            ("したかった", "する", ["desiderative past"], {"vs-i"}),
            ("したくない", "する", ["desiderative neg"], {"vs-i"}),
            ("しそう", "する", ["seeming (-sou)"], {"vs-i"}),
            ("しすぎる", "する", ["excessive (-sugiru)"], {"vs-i"}),
            ("しすぎた", "する", ["excessive past"], {"vs-i"}),

            # Suru verb forms (root noun marked with POS vs)
            ("しても", "", ["suru", "te-form", "even if (-temo)"], {"vs"}),
            ("しなくても", "", ["suru", "negative", "even if (-temo)"], {"vs"}),
            ("する", "", ["suru"], {"vs"}),
            ("しない", "", ["suru", "negative"], {"vs"}),
            ("しなかった", "", ["suru", "negative past"], {"vs"}),
            ("します", "", ["suru", "polite"], {"vs"}),
            ("しました", "", ["suru", "polite past"], {"vs"}),
            ("しません", "", ["suru", "polite negative"], {"vs"}),
            ("しませんでした", "", ["suru", "polite neg past"], {"vs"}),
            ("しよう", "", ["suru", "volitional"], {"vs"}),
            ("すれば", "", ["suru", "conditional (-eba)"], {"vs"}),
            ("したら", "", ["suru", "conditional (-tara)"], {"vs"}),
            ("しろ", "", ["suru", "imperative"], {"vs"}),
            ("せよ", "", ["suru", "imperative"], {"vs"}),
            ("できる", "", ["suru", "potential"], {"vs"}),
            ("できた", "", ["suru", "potential past"], {"vs"}),
            ("できない", "", ["suru", "potential negative"], {"vs"}),
            ("できなかった", "", ["suru", "potential neg past"], {"vs"}),
            ("できます", "", ["suru", "potential polite"], {"vs"}),
            ("できました", "", ["suru", "potential polite past"], {"vs"}),
            ("できません", "", ["suru", "potential polite neg"], {"vs"}),
            ("される", "", ["suru", "passive"], {"vs"}),
            ("された", "", ["suru", "passive past"], {"vs"}),
            ("されない", "", ["suru", "passive negative"], {"vs"}),
            ("されなかった", "", ["suru", "passive neg past"], {"vs"}),
            ("されます", "", ["suru", "passive polite"], {"vs"}),
            ("されました", "", ["suru", "passive polite past"], {"vs"}),
            ("させる", "", ["suru", "causative"], {"vs"}),
            ("させた", "", ["suru", "causative past"], {"vs"}),
            ("させない", "", ["suru", "causative negative"], {"vs"}),
            ("させられる", "", ["suru", "causative-passive"], {"vs"}),
            ("させられた", "", ["suru", "causative-passive past"], {"vs"}),
            ("したい", "", ["suru", "desiderative (-tai)"], {"vs"}),
            ("したかった", "", ["suru", "desiderative past"], {"vs"}),
            ("したくない", "", ["suru", "desiderative neg"], {"vs"}),
            ("しそう", "", ["suru", "seeming (-sou)"], {"vs"}),
            ("しすぎる", "", ["suru", "excessive (-sugiru)"], {"vs"}),
            ("しすぎた", "", ["suru", "excessive past"], {"vs"}),

            # Negative forms
            ("さない", "す", ["negative"], {"v5s"}),
            ("さなかった", "す", ["negative past"], {"v5s"}),
            ("かない", "く", ["negative"], {"v5k"}),
            ("かなかった", "く", ["negative past"], {"v5k"}),
            ("がない", "ぐ", ["negative"], {"v5g"}),
            ("がなかった", "ぐ", ["negative past"], {"v5g"}),
            ("たない", "つ", ["negative"], {"v5t"}),
            ("たなかった", "つ", ["negative past"], {"v5t"}),
            ("わない", "う", ["negative"], {"v5u"}),
            ("わなかった", "う", ["negative past"], {"v5u"}),
            ("らない", "る", ["negative"], {"v5r"}),
            ("らなかった", "る", ["negative past"], {"v5r"}),
            ("まない", "む", ["negative"], {"v5m"}),
            ("まなかった", "む", ["negative past"], {"v5m"}),
            ("ばない", "ぶ", ["negative"], {"v5b"}),
            ("ばなかった", "ぶ", ["negative past"], {"v5b"}),
            ("ない", "る", ["negative"], {"v1"}),
            ("なかった", "る", ["negative past"], {"v1"}),
            ("くない", "い", ["negative"], {"adj-i"}),
            ("くなかった", "い", ["negative past"], {"adj-i"}),

            # Polite forms
            ("します", "す", ["polite"], {"v5s"}),
            ("しました", "す", ["polite past"], {"v5s"}),
            ("しません", "す", ["polite negative"], {"v5s"}),
            ("しませんでした", "す", ["polite neg past"], {"v5s"}),
            ("きます", "く", ["polite"], {"v5k"}),
            ("きました", "く", ["polite past"], {"v5k"}),
            ("きません", "く", ["polite negative"], {"v5k"}),
            ("ぎます", "ぐ", ["polite"], {"v5g"}),
            ("ぎました", "ぐ", ["polite past"], {"v5g"}),
            ("ぎません", "ぐ", ["polite negative"], {"v5g"}),
            ("ちます", "つ", ["polite"], {"v5t"}),
            ("ちました", "つ", ["polite past"], {"v5t"}),
            ("ちません", "つ", ["polite negative"], {"v5t"}),
            ("います", "う", ["polite"], {"v5u"}),
            ("いました", "う", ["polite past"], {"v5u"}),
            ("いません", "う", ["polite negative"], {"v5u"}),
            ("ります", "る", ["polite"], {"v5r"}),
            ("りました", "る", ["polite past"], {"v5r"}),
            ("りません", "る", ["polite negative"], {"v5r"}),
            ("みます", "む", ["polite"], {"v5m"}),
            ("みました", "む", ["polite past"], {"v5m"}),
            ("みません", "む", ["polite negative"], {"v5m"}),
            ("びます", "ぶ", ["polite"], {"v5b"}),
            ("びました", "ぶ", ["polite past"], {"v5b"}),
            ("びません", "ぶ", ["polite negative"], {"v5b"}),
            ("ます", "る", ["polite"], {"v1"}),
            ("ました", "る", ["polite past"], {"v1"}),
            ("ません", "る", ["polite negative"], {"v1"}),
            ("ませんでした", "る", ["polite neg past"], {"v1"}),

            # Volitional forms
            ("そう", "す", ["volitional"], {"v5s"}),
            ("こう", "く", ["volitional"], {"v5k"}),
            ("ごう", "ぐ", ["volitional"], {"v5g"}),
            ("とう", "つ", ["volitional"], {"v5t"}),
            ("おう", "う", ["volitional"], {"v5u"}),
            ("ろう", "る", ["volitional"], {"v5r"}),
            ("もう", "む", ["volitional"], {"v5m"}),
            ("ぼう", "ぶ", ["volitional"], {"v5b"}),
            ("よう", "る", ["volitional"], {"v1"}),

            # Causative / Passive / Potential
            ("させられる", "る", ["causative", "passive"], {"v1"}),
            ("させられた", "る", ["causative", "passive", "past"], {"v1"}),
            ("させる", "る", ["causative"], {"v1"}),
            ("させた", "る", ["causative", "past"], {"v1"}),
            ("られる", "る", ["passive/potential"], {"v1"}),
            ("られた", "る", ["passive/potential", "past"], {"v1"}),
            ("される", "す", ["passive"], {"v5s"}),
            ("かれる", "く", ["passive"], {"v5k"}),
            ("がれる", "ぐ", ["passive"], {"v5g"}),
            ("たれる", "つ", ["passive"], {"v5t"}),
            ("われる", "う", ["passive"], {"v5u"}),
            ("られる", "る", ["passive"], {"v5r"}),
            ("まれる", "む", ["passive"], {"v5m"}),
            ("ばれる", "ぶ", ["passive"], {"v5b"}),
            ("せる", "す", ["potential"], {"v5s"}),
            ("ける", "く", ["potential"], {"v5k"}),
            ("げる", "ぐ", ["potential"], {"v5g"}),
            ("てる", "つ", ["potential"], {"v5t"}),
            ("える", "う", ["potential"], {"v5u"}),
            ("れる", "る", ["potential"], {"v5r"}),
            ("める", "む", ["potential"], {"v5m"}),
            ("べる", "ぶ", ["potential"], {"v5b"}),

            # Godan causatives (-seru / -sasu)
            ("かせる", "く", ["causative"], {"v5k"}),
            ("がせる", "ぐ", ["causative"], {"v5g"}),
            ("たせる", "つ", ["causative"], {"v5t"}),
            ("ばせる", "ぶ", ["causative"], {"v5b"}),
            ("ませる", "む", ["causative"], {"v5m"}),
            ("らせる", "る", ["causative"], {"v5r"}),
            ("わせる", "う", ["causative"], {"v5u"}),
            ("させる", "す", ["causative"], {"v5s"}),
            ("かす", "く", ["causative"], {"v5k"}),
            ("たす", "つ", ["causative"], {"v5t"}),
            ("ばす", "ぶ", ["causative"], {"v5b"}),
            ("ます", "む", ["causative"], {"v5m"}),
            ("らす", "る", ["causative"], {"v5r"}),
            ("わす", "う", ["causative"], {"v5u"}),
            # Polite negative of adjectives / expressions ending in -nai (e.g. 欠かせない -> 欠かせません)
            ("ません", "ない", ["polite negative"], {"adj-i", "exp"}),

            # Desiderative (-tai)
            ("したい", "す", ["desiderative (-tai)"], {"v5s"}),
            ("きたい", "く", ["desiderative (-tai)"], {"v5k"}),
            ("ぎたい", "ぐ", ["desiderative (-tai)"], {"v5g"}),
            ("ちたい", "つ", ["desiderative (-tai)"], {"v5t"}),
            ("いたい", "う", ["desiderative (-tai)"], {"v5u"}),
            ("りたい", "る", ["desiderative (-tai)"], {"v5r"}),
            ("みたい", "む", ["desiderative (-tai)"], {"v5m"}),
            ("びたい", "ぶ", ["desiderative (-tai)"], {"v5b"}),
            ("たい", "る", ["desiderative (-tai)"], {"v1"}),
            ("たくない", "る", ["desiderative negative"], {"v1"}),
            ("たかった", "る", ["desiderative past"], {"v1"}),

            # Conditionals
            ("せば", "す", ["conditional (-eba)"], {"v5s"}),
            ("けば", "く", ["conditional (-eba)"], {"v5k"}),
            ("げば", "ぐ", ["conditional (-eba)"], {"v5g"}),
            ("てば", "つ", ["conditional (-eba)"], {"v5t"}),
            ("えば", "う", ["conditional (-eba)"], {"v5u"}),
            ("れば", "る", ["conditional (-eba)"], {"v5r"}),
            ("れば", "る", ["conditional (-eba)"], {"v1"}),
            ("めば", "む", ["conditional (-eba)"], {"v5m"}),
            ("べば", "ぶ", ["conditional (-eba)"], {"v5b"}),
            ("ければ", "い", ["conditional (-eba)"], {"adj-i"}),

            # Irregular Kuru
            ("こない", "くる", ["negative"], {"vk"}),
            ("こなかった", "くる", ["negative past"], {"vk"}),
            ("きます", "くる", ["polite"], {"vk"}),
            ("きました", "くる", ["polite past"], {"vk"}),
            ("きません", "くる", ["polite negative"], {"vk"}),
            ("きて", "くる", ["te-form"], {"vk"}),
            ("きた", "くる", ["past"], {"vk"}),
            ("こよう", "くる", ["volitional"], {"vk"}),
            ("くれば", "くる", ["conditional (-eba)"], {"vk"}),
            ("こられる", "くる", ["potential/passive"], {"vk"}),
            ("こさせる", "くる", ["causative"], {"vk"}),
            ("こい", "くる", ["imperative"], {"vk"}),
            ("来ない", "来る", ["negative"], {"vk"}),
            ("来なかった", "来る", ["negative past"], {"vk"}),
            ("来ます", "来る", ["polite"], {"vk"}),
            ("来ました", "来る", ["polite past"], {"vk"}),
            ("来ません", "来る", ["polite negative"], {"vk"}),
            ("来て", "来る", ["te-form"], {"vk"}),
            ("来た", "来る", ["past"], {"vk"}),
            ("来よう", "来る", ["volitional"], {"vk"}),
            ("来れば", "来る", ["conditional (-eba)"], {"vk"}),
            ("来られる", "来る", ["potential/passive"], {"vk"}),
            ("来させる", "来る", ["causative"], {"vk"}),
            ("来い", "来る", ["imperative"], {"vk"}),
        ]

    @functools.lru_cache(maxsize=4096)
    def _deinflect_cached(self, word: str) -> Tuple[DeinflectionCandidate, ...]:
        results: List[DeinflectionCandidate] = []
        seen = set()

        def add_res(term: str, steps: List[str], pos_set: set[str]) -> None:
            key = (term, tuple(steps))
            if term and key not in seen:
                seen.add(key)
                results.append(DeinflectionCandidate(root=term, steps=steps, required_pos=pos_set))

        queue: deque[Tuple[str, List[str]]] = deque([(word, [])])
        visited_words = {word}

        while queue:
            curr, steps = queue.popleft()
            if len(steps) >= 4:
                continue
            for suf, repl, rule_steps, pos_set in self.rules:
                if not curr.endswith(suf):
                    continue
                is_full_or_chain = bool(pos_set & {"vs-i", "vk", "v_te", "v_de"})
                if is_full_or_chain:
                    if len(curr) < len(suf):
                        continue
                else:
                    # Standard verb/adjective/vs-noun suffix rules require a non-empty stem before `suf`
                    if len(curr) <= len(suf):
                        continue
                new_word = curr[:-len(suf)] + repl
                combined_steps = steps + rule_steps
                if pos_set not in ({"v_te"}, {"v_de"}):
                    add_res(new_word, combined_steps, pos_set)
                if new_word and new_word not in visited_words and len(new_word) >= 2:
                    visited_words.add(new_word)
                    queue.append((new_word, combined_steps))

        return tuple(results)

    def deinflect(self, word: str) -> List[DeinflectionCandidate]:
        """
        Recursively strip inflections and return candidate roots with ordered conjugation steps.
        Cached via LRU for sub-millisecond repeated lookups.
        """
        return list(self._deinflect_cached(word))


class _YomiPARSeqSmallEngine:
    """
    Lightweight pure-PyTorch inference wrapper for YomiToku's PARSeq-Small Japanese recognizer
    (`yomitoku_parseq_small.safetensors` + `yomitoku_charset.txt`).
    Uses `resize_policy="downscale"` on `(32, 800)` canvas + 1-step cloze iterative refinement.
    """

    def __init__(self, weights_path: str, charset_path: str) -> None:
        import torch.nn as nn
        from torch.nn.modules import transformer
        from safetensors.torch import load_file

        self._torch = torch
        with open(charset_path, "r", encoding="utf-8") as f:
            charset_str = f.read()

        self.itos = ("[E]",) + tuple(charset_str) + ("[B]", "[P]")
        self.eos_id = 0
        self.bos_id = len(self.itos) - 2
        self.pad_id = len(self.itos) - 1
        num_tokens = len(self.itos)
        embed_dim = 384
        self.max_label_length = 100

        class _PatchEmbed(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.grid_size = (2, 50)
                self.proj = nn.Conv2d(3, embed_dim, kernel_size=(16, 16), stride=(16, 16))

        class _Attention(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.num_heads = 8
                self.head_dim = embed_dim // 8
                self.qkv = nn.Linear(embed_dim, embed_dim * 3, bias=True)
                self.proj = nn.Linear(embed_dim, embed_dim, bias=True)

            def forward(self, x):
                B, N, C = x.shape
                qkv = self.qkv(x).reshape(B, N, 3, self.num_heads, self.head_dim).permute(2, 0, 3, 1, 4)
                q, k, v = qkv.unbind(0)
                x = torch.nn.functional.scaled_dot_product_attention(q, k, v)
                return self.proj(x.transpose(1, 2).reshape(B, N, C))

        class _Mlp(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.fc1 = nn.Linear(embed_dim, embed_dim * 4)
                self.act = nn.GELU()
                self.fc2 = nn.Linear(embed_dim * 4, embed_dim)

            def forward(self, x):
                return self.fc2(self.act(self.fc1(x)))

        class _Block(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.norm1 = nn.LayerNorm(embed_dim, eps=1e-6)
                self.attn = _Attention()
                self.norm2 = nn.LayerNorm(embed_dim, eps=1e-6)
                self.mlp = _Mlp()

            def forward(self, x):
                x = x + self.attn(self.norm1(x))
                return x + self.mlp(self.norm2(x))

        class _Encoder(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.patch_embed = _PatchEmbed()
                self.pos_embed = nn.Parameter(torch.zeros(1, 100, embed_dim))
                self.blocks = nn.Sequential(*[_Block() for _ in range(9)])
                self.norm = nn.LayerNorm(embed_dim, eps=1e-6)

            def forward(self, x):
                x = self.patch_embed.proj(x)
                gh, gw = x.shape[-2:]
                x = x.flatten(2).transpose(1, 2)
                pos = self.pos_embed.reshape(1, 2, 50, -1)[:, :gh, :gw]
                x = x + pos.reshape(1, gh * gw, -1)
                return self.norm(self.blocks(x))

        class _DecoderLayer(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.self_attn = nn.MultiheadAttention(embed_dim, 8, batch_first=True)
                self.cross_attn = nn.MultiheadAttention(embed_dim, 8, batch_first=True)
                self.linear1 = nn.Linear(embed_dim, embed_dim * 4)
                self.linear2 = nn.Linear(embed_dim * 4, embed_dim)
                self.norm1 = nn.LayerNorm(embed_dim)
                self.norm2 = nn.LayerNorm(embed_dim)
                self.norm_q = nn.LayerNorm(embed_dim)
                self.norm_c = nn.LayerNorm(embed_dim)

            def forward_stream(self, tgt, tgt_norm, tgt_kv, memory, tgt_mask, tgt_key_padding_mask):
                tgt2, _ = self.self_attn(tgt_norm, tgt_kv, tgt_kv, attn_mask=tgt_mask, key_padding_mask=tgt_key_padding_mask)
                tgt = tgt + tgt2
                tgt2, _ = self.cross_attn(self.norm1(tgt), memory, memory)
                tgt = tgt + tgt2
                tgt2 = self.linear2(torch.nn.functional.gelu(self.linear1(self.norm2(tgt))))
                return tgt + tgt2

            def forward(self, query, content, memory, query_mask=None, content_mask=None, content_key_padding_mask=None, update_content=True):
                q_norm = self.norm_q(query)
                c_norm = self.norm_c(content)
                query = self.forward_stream(query, q_norm, c_norm, memory, query_mask, content_key_padding_mask)
                if update_content:
                    content = self.forward_stream(content, c_norm, c_norm, memory, content_mask, content_key_padding_mask)
                return query, content

        class _Decoder(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.layers = transformer._get_clones(_DecoderLayer(), 1)
                self.norm = nn.LayerNorm(embed_dim)

            def forward(self, query, content, memory, query_mask=None, content_mask=None, content_key_padding_mask=None):
                for i, mod in enumerate(self.layers):
                    last = i == len(self.layers) - 1
                    query, content = mod(query, content, memory, query_mask, content_mask, content_key_padding_mask, update_content=not last)
                return self.norm(query)

        class _TokenEmbedding(nn.Module):
            def __init__(self) -> None:
                super().__init__()
                self.embedding = nn.Embedding(num_tokens, embed_dim)

            def forward(self, tokens):
                return (embed_dim ** 0.5) * self.embedding(tokens)

        class _PARSeqModel(nn.Module):
            def __init__(self, max_len: int) -> None:
                super().__init__()
                self.encoder = _Encoder()
                self.decoder = _Decoder()
                self.head = nn.Linear(embed_dim, num_tokens - 2)
                self.text_embed = _TokenEmbedding()
                self.pos_queries = nn.Parameter(torch.zeros(1, max_len + 1, embed_dim))

            def decode_step(self, tgt, memory, tgt_mask=None, tgt_padding_mask=None, tgt_query=None, tgt_query_mask=None):
                N, L = tgt.shape
                null_ctx = self.text_embed(tgt[:, :1])
                tgt_emb = self.pos_queries[:, :L - 1] + self.text_embed(tgt[:, 1:])
                tgt_emb = torch.cat([null_ctx, tgt_emb], dim=1)
                if tgt_query is None:
                    tgt_query = self.pos_queries[:, :L].expand(N, -1, -1)
                return self.decoder(tgt_query, tgt_emb, memory, tgt_query_mask, tgt_mask, tgt_padding_mask)

        self.model = _PARSeqModel(self.max_label_length)
        sd = load_file(weights_path)
        self.model.load_state_dict(sd, strict=True)
        self.model.eval()

    def recognize(self, pil_img: Image.Image) -> str:
        torch = self._torch
        rgb = np.array(pil_img.convert("RGB"))
        h, w = rgb.shape[:2]
        if h > 2 * w:
            rgb = np.rot90(rgb, 1)
            h, w = rgb.shape[:2]
        scale = min(1.0, min(32.0 / max(1, h), 800.0 / max(1, w)))
        new_h = max(1, int(h * scale))
        new_w = max(1, int(w * scale))
        if (new_w, new_h) != (w, h):
            resized = np.array(Image.fromarray(rgb).resize((new_w, new_h), Image.Resampling.BILINEAR))
        else:
            resized = rgb
        canvas = np.zeros((32, 800, 3), dtype=np.uint8)
        canvas[:new_h, :new_w, :] = resized
        tensor = torch.from_numpy(canvas.transpose(2, 0, 1)).float() / 255.0
        tensor = (tensor - 0.5) / 0.5
        images = tensor.unsqueeze(0)

        with torch.inference_mode():
            memory = self.model.encoder(images)
            num_steps = self.max_label_length + 1
            pos_queries = self.model.pos_queries[:, :num_steps]
            tgt_mask = torch.triu(torch.ones((num_steps, num_steps), dtype=torch.bool), 1)
            query_mask = tgt_mask.clone()
            tgt_in = torch.full((1, num_steps), self.pad_id, dtype=torch.long)
            tgt_in[:, 0] = self.bos_id
            logits = []
            for i in range(num_steps):
                j = i + 1
                tgt_out = self.model.decode_step(
                    tgt_in[:, :j],
                    memory,
                    tgt_mask[:j, :j],
                    tgt_query=pos_queries[:, i:j],
                    tgt_query_mask=query_mask[i:j, :j],
                )
                p_i = self.model.head(tgt_out)
                logits.append(p_i)
                if j < num_steps:
                    nxt = int(p_i.argmax(-1).item())
                    tgt_in[0, j] = nxt
                    if nxt == self.eos_id:
                        break
            logits_cat = torch.cat(logits, dim=1)
            # 1-step cloze refinement
            q_mask_ref = torch.triu(torch.ones((num_steps, num_steps), dtype=torch.bool), 1)
            q_mask_ref[torch.triu(torch.ones((num_steps, num_steps), dtype=torch.bool), 2)] = False
            bos = torch.full((1, 1), self.bos_id, dtype=torch.long)
            tgt_in_ref = torch.cat([bos, logits_cat[:, :-1].argmax(-1)], dim=1)
            tgt_pad_mask = (tgt_in_ref == self.eos_id).int().cumsum(-1) > 0
            L_ref = tgt_in_ref.shape[1]
            tgt_out_ref = self.model.decode_step(
                tgt_in_ref,
                memory,
                tgt_mask[:L_ref, :L_ref],
                tgt_pad_mask,
                pos_queries[:, :L_ref],
                q_mask_ref[:L_ref, :L_ref],
            )
            logits_final = self.model.head(tgt_out_ref)
            ids = logits_final[0].argmax(-1).tolist()
            if self.eos_id in ids:
                ids = ids[:ids.index(self.eos_id)]
            return unicodedata.normalize("NFKC", "".join(self.itos[idx] for idx in ids))


class YomitanEntry:
    __slots__ = ("term", "reading", "headword", "pos", "glosses", "score", "dict_title")

    def __init__(
        self,
        term: str,
        reading: str,
        headword: str,
        pos: str,
        glosses: List[str],
        score: int,
        dict_title: str,
    ):
        self.term = term
        self.reading = reading
        self.headword = headword
        self.pos = pos
        self.glosses = glosses
        self.score = score
        self.dict_title = dict_title


class YomitanDictionaryManager:
    """Dynamically loads and manages Yomitan-format (.zip archives and directories)
    dictionaries placed in the dictionaries/ directory (e.g. Babylon Japanese-English).
    Uses indexed SQLite databases in dictionaries/.cache/ for ultra-fast startup and queries.
    """

    _RE_YOMI_META = re.compile(
        r'^(?:([a-zA-Z\.\s]+?)\s+)?(?:\((?:Hira|Kana)=([^)]+)\)\s*|\((?:Kanji)=([^)]+)\)\s*|\((?:Alt)=([^)]+)\)\s*)(.*)',
        re.IGNORECASE,
    )

    def __init__(self, dict_dir: str = DICTIONARIES_DIR):
        self.dict_dir = dict_dir
        self.cache_dir = os.path.join(dict_dir, ".cache")
        self.loaded_dicts: List[Tuple[str, int]] = []
        self.known_terms: set[str] = set()
        self._db_conns: List[sqlite3.Connection] = []
        self.load_all()

    def close(self) -> None:
        """Close all open SQLite database connections."""
        for conn in self._db_conns:
            try:
                conn.close()
            except Exception:
                pass
        self._db_conns.clear()

    @staticmethod
    def _is_all_kana(s: str) -> bool:
        return bool(s) and all(('\u3040' <= c <= '\u30ff') or c in 'ー・ ' for c in s)

    def _parse_entry(self, item: list, dict_title: str) -> Optional[YomitanEntry]:
        if not item or len(item) < 2:
            return None
        term = str(item[0])
        reading = str(item[1] or "")
        score = int(item[4]) if len(item) > 4 and isinstance(item[4], (int, float)) else 0
        raw_glosses = item[5] if len(item) > 5 else []

        m_pos, m_reading, m_kanji = "", reading, ""
        clean_glosses: List[str] = []

        if isinstance(raw_glosses, list):
            for g in raw_glosses:
                if isinstance(g, str):
                    m = self._RE_YOMI_META.match(g)
                    if m:
                        p, h, k, _alt, rest = m.groups()
                        if p and not m_pos:
                            m_pos = p.strip()
                        if h and not m_reading:
                            m_reading = h.strip()
                        if k and not m_kanji:
                            m_kanji = k.strip()
                        cleaned = rest.strip() if rest else g
                        clean_glosses.append(cleaned)
                    else:
                        clean_glosses.append(g)
                elif isinstance(g, dict):
                    content = g.get("content", "")
                    if isinstance(content, str):
                        clean_glosses.append(content)
                    else:
                        clean_glosses.append(str(g))
                else:
                    clean_glosses.append(str(g))
        elif isinstance(raw_glosses, str):
            clean_glosses.append(raw_glosses)

        if not clean_glosses:
            return None

        if self._is_all_kana(term):
            headword = m_kanji or term
            resolved_reading = m_reading or term
        else:
            headword = term
            resolved_reading = m_reading or reading

        pos_str = f"[{dict_title}] {m_pos}" if m_pos else f"[{dict_title}]"
        return YomitanEntry(
            term=term,
            reading=resolved_reading,
            headword=headword,
            pos=pos_str,
            glosses=clean_glosses,
            score=score,
            dict_title=dict_title,
        )

    def _open_cached_db(self, db_path: str, terms_path: str, dict_title: str, count: int) -> bool:
        """Open an existing cached SQLite database and load known terms set."""
        try:
            conn = sqlite3.connect(f"file:{os.path.abspath(db_path)}?mode=ro", uri=True, check_same_thread=False)
            self._db_conns.append(conn)
            if os.path.isfile(terms_path):
                with open(terms_path, "rb") as f:
                    terms_set = pickle.load(f)
                self.known_terms.update(terms_set)
            else:
                cur = conn.cursor()
                cur.execute("SELECT DISTINCT lookup_key FROM search_index")
                terms_set = {r[0] for r in cur.fetchall()}
                self.known_terms.update(terms_set)
                try:
                    with open(terms_path, "wb") as f:
                        pickle.dump(terms_set, f, protocol=pickle.HIGHEST_PROTOCOL)
                except Exception:
                    pass
            self.loaded_dicts.append((dict_title, count))
            logger.info("Loaded cached Yomitan dictionary '%s' from %s (%d entries).", dict_title, os.path.basename(db_path), count)
            return True
        except Exception as e:
            logger.warning("Failed opening cached database %s: %s", db_path, e)
            return False

    def _build_and_open_db(
        self,
        db_path: str,
        terms_path: str,
        dict_title: str,
        entry_generator,
    ) -> None:
        """Build indexed SQLite database and known-terms cache from parsed entries."""
        os.makedirs(self.cache_dir, exist_ok=True)
        temp_db = db_path + f".tmp.{os.getpid()}"
        if os.path.exists(temp_db):
            try:
                os.remove(temp_db)
            except OSError:
                pass

        try:
            conn = sqlite3.connect(temp_db)
            conn.execute("PRAGMA synchronous = OFF")
            conn.execute("PRAGMA journal_mode = OFF")
            conn.execute("""
                CREATE TABLE entries (
                    id INTEGER PRIMARY KEY,
                    term TEXT,
                    reading TEXT,
                    headword TEXT,
                    pos TEXT,
                    glosses TEXT,
                    score INTEGER,
                    dict_title TEXT
                )
            """)
            conn.execute("CREATE TABLE search_index (lookup_key TEXT, entry_id INTEGER)")

            entries_rows = []
            index_rows = []
            dict_known = set()
            entry_id = 0

            for ent in entry_generator:
                entry_id += 1
                gloss_json = json.dumps(ent.glosses, ensure_ascii=False)
                entries_rows.append((
                    entry_id, ent.term, ent.reading, ent.headword, ent.pos, gloss_json, ent.score, ent.dict_title
                ))

                keys = {ent.term}
                if ent.headword and ent.headword != ent.term:
                    keys.add(ent.headword)
                if ent.reading:
                    keys.add(ent.reading)
                    h_rd = katakana_to_hiragana(ent.reading)
                    if h_rd != ent.reading:
                        keys.add(h_rd)
                h_tm = katakana_to_hiragana(ent.term)
                if h_tm != ent.term:
                    keys.add(h_tm)

                dict_known.update(keys)
                for k in keys:
                    index_rows.append((k, entry_id))

                if len(entries_rows) >= 50000:
                    conn.executemany("INSERT INTO entries VALUES (?, ?, ?, ?, ?, ?, ?, ?)", entries_rows)
                    conn.executemany("INSERT INTO search_index VALUES (?, ?)", index_rows)
                    entries_rows.clear()
                    index_rows.clear()

            if entries_rows:
                conn.executemany("INSERT INTO entries VALUES (?, ?, ?, ?, ?, ?, ?, ?)", entries_rows)
                conn.executemany("INSERT INTO search_index VALUES (?, ?)", index_rows)

            conn.execute("CREATE INDEX idx_key ON search_index(lookup_key)")
            conn.commit()
            conn.close()

            if os.path.exists(db_path):
                try:
                    os.remove(db_path)
                except OSError:
                    pass
            os.replace(temp_db, db_path)

            try:
                with open(terms_path, "wb") as f:
                    pickle.dump(dict_known, f, protocol=pickle.HIGHEST_PROTOCOL)
            except Exception as e:
                logger.warning("Failed saving terms cache: %s", e)

            # Open in read-only mode
            ro_conn = sqlite3.connect(f"file:{os.path.abspath(db_path)}?mode=ro", uri=True, check_same_thread=False)
            self._db_conns.append(ro_conn)
            self.known_terms.update(dict_known)
            self.loaded_dicts.append((dict_title, entry_id))
            logger.info("Indexed and loaded Yomitan dictionary '%s' (%d entries).", dict_title, entry_id)
        except Exception as exc:
            logger.error("Failed building database cache for %s: %s", dict_title, exc)
            if os.path.exists(temp_db):
                try:
                    os.remove(temp_db)
                except OSError:
                    pass

    def _try_open_cached_db(self, db_path: str, terms_path: str, src_mtime: float) -> bool:
        """Open cached SQLite database if it is up-to-date with the source dictionary."""
        if not (os.path.isfile(db_path) and os.path.getmtime(db_path) >= src_mtime):
            return False
        try:
            test_conn = sqlite3.connect(f"file:{os.path.abspath(db_path)}?mode=ro", uri=True)
            cur = test_conn.cursor()
            cur.execute("SELECT dict_title, COUNT(*) FROM entries LIMIT 1")
            row = cur.fetchone()
            test_conn.close()
            if row and row[0]:
                return self._open_cached_db(db_path, terms_path, row[0], row[1])
        except Exception:
            pass
        return False

    def _load_zip(self, zip_path: str) -> None:
        try:
            base_name = os.path.splitext(os.path.basename(zip_path))[0]
            db_path = os.path.join(self.cache_dir, f"{base_name}.db")
            terms_path = os.path.join(self.cache_dir, f"{base_name}.terms.pkl")
            zip_mtime = os.path.getmtime(zip_path)

            if self._try_open_cached_db(db_path, terms_path, zip_mtime):
                return

            with zipfile.ZipFile(zip_path, "r") as z:
                names = set(z.namelist())
                if "index.json" not in names:
                    return
                try:
                    idx_data = json.loads(z.read("index.json").decode("utf-8"))
                    dict_title = idx_data.get("title") or base_name
                except Exception:
                    dict_title = base_name

                def entry_gen():
                    term_banks = [n for n in names if n.startswith("term_bank_") and n.endswith(".json")]
                    for tb in term_banks:
                        try:
                            data = json.loads(z.read(tb).decode("utf-8"))
                            for item in data:
                                ent = self._parse_entry(item, dict_title)
                                if ent:
                                    yield ent
                        except Exception as e:
                            logger.warning("Error reading %s from %s: %s", tb, zip_path, e)

                self._build_and_open_db(db_path, terms_path, dict_title, entry_gen())
        except Exception as exc:
            logger.warning("Failed to load Yomitan zip %s: %s", zip_path, exc)

    def _load_dir(self, dir_path: str) -> None:
        idx_path = os.path.join(dir_path, "index.json")
        if not os.path.isfile(idx_path):
            return
        try:
            base_name = os.path.basename(dir_path)
            db_path = os.path.join(self.cache_dir, f"{base_name}.db")
            terms_path = os.path.join(self.cache_dir, f"{base_name}.terms.pkl")
            dir_mtime = os.path.getmtime(idx_path)

            if self._try_open_cached_db(db_path, terms_path, dir_mtime):
                return

            with open(idx_path, "r", encoding="utf-8") as f:
                idx_data = json.load(f)
                dict_title = idx_data.get("title") or base_name

            def entry_gen():
                for fname in os.listdir(dir_path):
                    if fname.startswith("term_bank_") and fname.endswith(".json"):
                        fpath = os.path.join(dir_path, fname)
                        try:
                            with open(fpath, "r", encoding="utf-8") as f:
                                data = json.load(f)
                                for item in data:
                                    ent = self._parse_entry(item, dict_title)
                                    if ent:
                                        yield ent
                        except Exception as e:
                            logger.warning("Error reading %s: %s", fpath, e)

            self._build_and_open_db(db_path, terms_path, dict_title, entry_gen())
        except Exception as exc:
            logger.warning("Failed to load Yomitan directory %s: %s", dir_path, exc)

    def load_all(self) -> None:
        """Scan and load all Yomitan dictionaries in the configured directory."""
        self.close()
        self.known_terms.clear()
        self.loaded_dicts.clear()

        if not os.path.isdir(self.dict_dir):
            return
        for entry in sorted(os.listdir(self.dict_dir)):
            full_p = os.path.join(self.dict_dir, entry)
            if entry.lower().endswith(".zip") and os.path.isfile(full_p):
                self._load_zip(full_p)
            elif os.path.isdir(full_p) and not entry.startswith("."):
                self._load_dir(full_p)

    def lookup(self, term: str) -> List[YomitanEntry]:
        """Look up term (or reading/kana equivalent) in loaded Yomitan databases."""
        if not self._db_conns or not term:
            return []

        search_keys = [term]
        h_term = katakana_to_hiragana(term)
        if h_term != term:
            search_keys.append(h_term)

        results: List[YomitanEntry] = []
        seen_entry_keys = set()

        for conn in self._db_conns:
            cur = conn.cursor()
            for key in search_keys:
                cur.execute("""
                    SELECT e.term, e.reading, e.headword, e.pos, e.glosses, e.score, e.dict_title
                    FROM search_index s
                    JOIN entries e ON s.entry_id = e.id
                    WHERE s.lookup_key = ?
                    ORDER BY e.score DESC
                """, (key,))
                for row in cur.fetchall():
                    r_term, r_reading, r_headword, r_pos, r_glosses_json, r_score, r_dict_title = row
                    dedup_key = (r_dict_title, r_headword, r_reading)
                    if dedup_key in seen_entry_keys:
                        continue
                    seen_entry_keys.add(dedup_key)

                    try:
                        glosses = json.loads(r_glosses_json)
                    except Exception:
                        glosses = [r_glosses_json]

                    results.append(YomitanEntry(
                        term=r_term,
                        reading=r_reading,
                        headword=r_headword,
                        pos=r_pos,
                        glosses=glosses,
                        score=r_score,
                        dict_title=r_dict_title,
                    ))

        return results


class WordMetaManager:
    """Provides frequency ranking and JLPT level lookup for Japanese words and kanji."""

    def __init__(self, db_path: str = WORD_META_DB_PATH, jlpt_kanji_path: str = JLPT_KANJI_PATH) -> None:
        self._db_path = db_path
        self._conn: Optional[sqlite3.Connection] = None
        self._lock = threading.Lock()
        self._jlpt_kanji: Dict[str, int] = {}
        if os.path.isfile(jlpt_kanji_path):
            try:
                with open(jlpt_kanji_path, "r", encoding="utf-8") as f:
                    self._jlpt_kanji = json.load(f)
            except Exception as exc:
                logger.warning("Failed to load JLPT kanji JSON %s: %s", jlpt_kanji_path, exc)
        self._kangxi_radicals: Dict[str, List[str]] = {}
        rad_json_path = os.path.join(os.path.dirname(jlpt_kanji_path), "kangxi_radicals.json")
        if os.path.isfile(rad_json_path):
            try:
                with open(rad_json_path, "r", encoding="utf-8") as f:
                    self._kangxi_radicals = json.load(f)
            except Exception as exc:
                logger.warning("Failed to load Kangxi radicals JSON %s: %s", rad_json_path, exc)
        self._cache: Dict[Tuple[str, str], str] = {}
        self._init_db()

    def _init_db(self) -> None:
        if os.path.isfile(self._db_path):
            try:
                self._conn = sqlite3.connect(f"file:{self._db_path}?mode=ro", uri=True, check_same_thread=False)
            except Exception as e:
                logger.warning("Could not open word_meta.db in read-only mode: %s; trying normal connect", e)
                try:
                    self._conn = sqlite3.connect(self._db_path, check_same_thread=False)
                except Exception as e2:
                    logger.warning("Could not open word_meta.db: %s", e2)
                    self._conn = None

    def get_rank_and_jlpt(self, term: str, reading: str = "", jmdict_entry: Any = None) -> str:
        """Return formatted ranking and JLPT string, e.g. '#195 · N5', '#106', 'Common · N3', or 'Uncommon'."""
        if not term or term == "Selection Translation":
            return ""
        cache_key = (term, reading)
        with self._lock:
            if cache_key in self._cache:
                return self._cache[cache_key]

        freq_rank: Optional[Union[int, str]] = None
        jlpt_lvl: Optional[int] = None

        if self._conn:
            try:
                with self._lock:
                    cur = self._conn.cursor()
                    if reading:
                        cur.execute("SELECT freq_rank, jlpt FROM word_meta WHERE term = ? AND reading = ? LIMIT 1", (term, reading))
                        row = cur.fetchone()
                        if row:
                            freq_rank, jlpt_lvl = row[0], row[1]
                    if freq_rank is None and jlpt_lvl is None:
                        cur.execute("SELECT freq_rank, jlpt FROM word_meta WHERE term = ? LIMIT 1", (term,))
                        row = cur.fetchone()
                        if row:
                            freq_rank, jlpt_lvl = row[0], row[1]
            except Exception:
                pass

        if freq_rank is None and jmdict_entry is not None:
            pri_tags = set()
            for kf in getattr(jmdict_entry, "kanji_forms", []) or []:
                pri_tags.update(getattr(kf, "pri", []) or [])
            for kn in getattr(jmdict_entry, "kana_forms", []) or []:
                pri_tags.update(getattr(kn, "pri", []) or [])
            nf_tags = [int(p[2:]) for p in pri_tags if p.startswith("nf") and p[2:].isdigit()]
            if nf_tags:
                min_nf = min(nf_tags)
                freq_rank = min_nf * 500
            elif any(p in pri_tags for p in ("ichi1", "news1", "spec1", "gai1")):
                freq_rank = "Common"

        if jlpt_lvl is None and len(term) == 1 and term in self._jlpt_kanji:
            jlpt_lvl = self._jlpt_kanji[term]

        parts = []
        if freq_rank is not None:
            if isinstance(freq_rank, int):
                parts.append(f"#{freq_rank}")
            else:
                parts.append(str(freq_rank))
        if jlpt_lvl:
            parts.append(f"N{jlpt_lvl}")

        res = " · ".join(parts) if parts else "Uncommon"
        with self._lock:
            if len(self._cache) < 8192:
                self._cache[cache_key] = res
        return res

    def lookup_kanji_info(self, kanji_char: str, jam: Any = None) -> dict:
        """Query Kanjidic2 and local JLPT data for a single kanji character."""
        info: dict = {
            "stroke_count": None,
            "grade": None,
            "freq": None,
            "jlpt": self._jlpt_kanji.get(kanji_char),
            "rad_num": None,
            "rad_char": "",
            "rad_base": "",
            "rad_reading": "",
            "rad_meaning": "",
            "on_readings": [],
            "kun_readings": [],
            "nanori": [],
            "meanings": [],
        }
        if jam is not None:
            try:
                _kanjidic_lookup_aliases = {
                    "\u2ecc": ("\u8fb6", 3),  # ⻌ (1-dot Joyo shinnyou) -> query 辶 in Kanjidic2, 3 strokes
                    "\u2e8c": ("小", 3),      # ⺌ (small variant) -> query 小 in Kanjidic2, 3 strokes
                }
                lookup_char, override_strokes = _kanjidic_lookup_aliases.get(kanji_char, (kanji_char, None))
                kd_res = jam.lookup(lookup_char)
                if kd_res and kd_res.chars:
                    c = kd_res.chars[0]
                    info["stroke_count"] = override_strokes if override_strokes is not None else c.stroke_count
                    info["grade"] = c.grade
                    info["freq"] = c.freq
                    if info["jlpt"] is None and c.jlpt is not None:
                        info["jlpt"] = c.jlpt
                    if c.radicals:
                        try:
                            rad_num = int(c.radicals[0].value)
                            info["rad_num"] = rad_num
                            if 1 <= rad_num <= 214:
                                # Detect which radical variant actually appears in this kanji via
                                # Jamdict's KRADFILE decomposition (`jam.krad`), and pair it with
                                # the canonical base form when they differ.
                                _RADICAL_VARIANT_MAP = {
                                    9: [("⺅", "亻")],
                                    18: [("刂", "刂")],
                                    42: [("⺌", "⺌")],
                                    58: [("彑", "彑"), ("ヨ", "彐")],
                                    61: [("忄", "忄")],
                                    64: [("扌", "扌")],
                                    66: [("攵", "攵")],
                                    80: [("母", "母")],
                                    85: [("氵", "氵")],
                                    86: [("灬", "灬")],
                                    94: [("犭", "犭")],
                                    96: [("王", "王")],
                                    113: [("礻", "礻")],
                                    122: [("罒", "罒")],
                                    125: [("⺹", "耂")],
                                    130: [("月", "月")],
                                    140: [("⺾", "艹")],
                                    145: [("衤", "衤")],
                                    146: [("西", "西")],
                                    162: [("辶", "\u2ecc")],
                                    163: [("⻏", "阝")],
                                    170: [("⻖", "阝")],
                                }
                                _jp_rad_overrides = {
                                    122: "罒",      # 网 -> 罒 (あみがしら)
                                    140: "艹",      # 艸 -> 艹 (くさかんむり)
                                    162: "\u2ecc",  # 辵 -> ⻌ (1-dot しんにょう)
                                    163: "阝",      # 邑 -> 阝 (おおざと)
                                    170: "阝",      # 阜 -> 阝 (こざとへん)
                                }
                                base_char = unicodedata.normalize("NFKC", chr(0x2F00 + rad_num - 1))
                                krad_dict = getattr(jam, "krad", None) or {}
                                comps = krad_dict.get(kanji_char) or krad_dict.get(lookup_char) or []
                                matched_variant = ""
                                for krad_c, disp_v in _RADICAL_VARIANT_MAP.get(rad_num, ()):
                                    if krad_c in comps and disp_v != base_char:
                                        matched_variant = disp_v
                                        break
                                if matched_variant:
                                    info["rad_char"] = matched_variant
                                    info["rad_base"] = base_char
                                elif rad_num in _jp_rad_overrides and kanji_char != base_char:
                                    info["rad_char"] = _jp_rad_overrides[rad_num]
                                    info["rad_base"] = base_char
                                else:
                                    info["rad_char"] = base_char
                                    info["rad_base"] = ""
                                rad_meta = self._kangxi_radicals.get(str(rad_num))
                                if rad_meta and len(rad_meta) >= 2:
                                    info["rad_reading"] = rad_meta[0]
                                    info["rad_meaning"] = rad_meta[1]
                        except (ValueError, TypeError):
                            pass
                    if c.rm_groups:
                        rm0 = c.rm_groups[0]
                        info["on_readings"] = [str(r) for r in getattr(rm0, "on_readings", []) or []]
                        info["kun_readings"] = [str(r) for r in getattr(rm0, "kun_readings", []) or []]
                        info["meanings"] = [
                            getattr(m, "value", str(m))
                            for m in getattr(rm0, "meanings", []) or []
                            if getattr(m, "m_lang", "") in ("", "en")
                        ]
                    info["nanori"] = [getattr(n, "value", str(n)) for n in getattr(c, "nanori", []) or []]
            except Exception as ex:
                logger.debug("Kanjidic lookup error for %s: %s", kanji_char, ex)
        return info

    def get_kanji_details(self, kanji_char: str, jam: Any = None) -> DictionaryResult:
        """Lookup kanji in Kanjidic2 and return a DictionaryResult with stroke count, readings, and meanings."""
        info = self.lookup_kanji_info(kanji_char, jam)
        stroke_count = info["stroke_count"]
        grade = info["grade"]
        freq = info["freq"]
        jlpt = info["jlpt"]
        rad_num = info["rad_num"]
        rad_char = info["rad_char"]
        rad_base = info.get("rad_base", "")
        on_readings = info["on_readings"]
        kun_readings = info["kun_readings"]
        meanings = info["meanings"]

        readings_parts = []
        if on_readings:
            readings_parts.append("音: " + "・".join(on_readings))
        if kun_readings:
            readings_parts.append("訓: " + "・".join(kun_readings))
        reading_str = "  ".join(readings_parts) if readings_parts else ""

        pos_parts = ["Kanji"]
        if stroke_count is not None:
            pos_parts.append(f"Strokes: {stroke_count}")
        if grade is not None:
            pos_parts.append(f"Grade: {grade}")
        if jlpt is not None:
            pos_parts.append(f"JLPT N{jlpt}")
        if freq is not None:
            pos_parts.append(f"Freq: #{freq}")
        if rad_char or rad_num:
            if rad_char and rad_base and rad_char != rad_base:
                pos_parts.append(f"Radical: {rad_char} ({rad_base})")
            else:
                pos_parts.append(f"Radical: {rad_char or '#' + str(rad_num)}")

        rank_jlpt = ""
        if freq is not None and jlpt is not None:
            rank_jlpt = f"#{freq} · N{jlpt}"
        elif freq is not None:
            rank_jlpt = f"#{freq}"
        elif jlpt is not None:
            rank_jlpt = f"N{jlpt}"

        return DictionaryResult(
            headword=kanji_char,
            reading=reading_str,
            glosses=meanings if meanings else ["(No definition found in Kanjidic)"],
            pos=" · ".join(pos_parts),
            matched_surface=kanji_char,
            context_text=f"Kanji {kanji_char} ({reading_str})",
            found=True,
            rank_jlpt=rank_jlpt,
        )


class JapaneseOCRDictionaryPipeline:
    """
    Thread-safe layout-aware 4-Engine Japanese OCR ensemble (MangaOCR + PaddleOCR + YomiToku + NDLOCR),
    multi-line paragraph stitching, morphological deinflection, and JMdict lookup pipeline.
    """

    def __init__(self) -> None:
        self._lock = threading.RLock()
        self._ocr_exec_lock = threading.Lock()
        self._screen_cache_lock = threading.Lock()
        self._screen_lines_cache: Dict[Tuple[str, str], List[DetectedLine]] = {}
        self._screen_cache_inflight: Dict[Tuple[str, str], threading.Event] = {}
        self._hybrid_any_ready: Dict[str, threading.Event] = {}
        self._gemini_cooldown_until: Dict[str, float] = {}
        self._detector = None
        self._manga_ocr = None
        self._paddle_ocr = None
        self._yomi_ocr: Optional[_YomiPARSeqSmallEngine] = None
        self._ndl_sess = None
        self._ndl_charlist: List[str] = []
        self._tagger = None
        self._jamdict = None
        self._jmd_cache: Dict[str, Any] = {}
        self._jmd_known_terms: set = set()
        self._deinflector = JapaneseDeinflector()
        self._custom_dict: Dict[str, List[dict]] = {}
        self._custom_voicing_index: Dict[str, List[str]] = {}
        self._yomitan_mgr: Optional[YomitanDictionaryManager] = None
        self._word_meta_mgr = WordMetaManager(WORD_META_DB_PATH, JLPT_KANJI_PATH)

        self._load_custom_dictionaries()

        # 1. Initialize Scene Text Detector (RapidOCR DBNet ONNX)
        try:
            from rapidocr_onnxruntime import RapidOCR
            self._detector = RapidOCR()
            logger.info("RapidOCR (DBNet text detector) initialized.")
        except ImportError:
            logger.warning(
                "rapidocr-onnxruntime not installed; falling back to pytesseract layout detection."
            )

        # 2. Initialize MangaOCR for high-accuracy Japanese line recognition
        try:
            hf_cache_dir = os.path.expanduser(
                "~/.cache/huggingface/hub/models--kha-white--manga-ocr-base"
            )
            if os.path.isdir(hf_cache_dir):
                os.environ.setdefault("HF_HUB_OFFLINE", "1")
                os.environ.setdefault("TRANSFORMERS_OFFLINE", "1")
            from manga_ocr import MangaOcr
            logger.info("Loading MangaOCR model...")
            try:
                self._manga_ocr = MangaOcr()
            except Exception as ocr_err:
                if os.environ.get("TRANSFORMERS_OFFLINE") == "1":
                    logger.warning("MangaOCR offline load failed (%s); retrying with online check...", ocr_err)
                    os.environ.pop("TRANSFORMERS_OFFLINE", None)
                    os.environ.pop("HF_HUB_OFFLINE", None)
                    self._manga_ocr = MangaOcr()
                else:
                    raise
            logger.info("MangaOCR initialized.")
        except ImportError:
            logger.warning("manga-ocr not installed; using detector/Tesseract text recognition.")

        # 2b. Initialize PaddleOCR Japanese v4, YomiToku PARSeq-Small, and NDLOCR-Lite
        models_dir = find_ocr_models_dir()
        try:
            from rapidocr_onnxruntime.ch_ppocr_rec import TextRecognizer as PaddleRec
            p_onnx = os.path.join(models_dir, "japan_PP-OCRv4_rec.onnx")
            p_dict = os.path.join(models_dir, "japan_dict.txt")
            if os.path.isfile(p_onnx) and os.path.isfile(p_dict):
                self._paddle_ocr = PaddleRec({
                    "model_path": p_onnx,
                    "rec_keys_path": p_dict,
                    "rec_batch_num": 6,
                    "rec_img_shape": [3, 48, 320],
                    "use_cuda": False,
                    "use_dml": False,
                })
                logger.info("PaddleOCR (japan_PP-OCRv4_rec) initialized.")
        except Exception as exc:
            logger.warning("PaddleOCR initialization skipped: %s", exc)

        try:
            y_weights = os.path.join(models_dir, "yomitoku_parseq_small.safetensors")
            y_charset = os.path.join(models_dir, "yomitoku_charset.txt")
            if os.path.isfile(y_weights) and os.path.isfile(y_charset):
                self._yomi_ocr = _YomiPARSeqSmallEngine(y_weights, y_charset)
                logger.info("YomiToku (PARSeq-Small) initialized.")
        except Exception as exc:
            logger.warning("YomiToku initialization skipped: %s", exc)

        try:
            import onnxruntime
            import yaml
            n_onnx = os.path.join(models_dir, "ndlocr_parseq_16x384.onnx")
            n_yaml = os.path.join(models_dir, "NDLmoji.yaml")
            if os.path.isfile(n_onnx) and os.path.isfile(n_yaml):
                with open(n_yaml, "r", encoding="utf-8") as f:
                    ndl_cfg = yaml.safe_load(f)
                self._ndl_charlist = list(ndl_cfg["model"]["charset_train"])
                self._ndl_sess = onnxruntime.InferenceSession(n_onnx, providers=["CPUExecutionProvider"])
                logger.info("NDLOCR-Lite (PARSeq-NDL 16x384) initialized.")
        except Exception as exc:
            logger.warning("NDLOCR initialization skipped: %s", exc)

        # 3. Initialize Morphological Analyzer (Fugashi / MeCab with UniDic)
        try:
            from fugashi import Tagger
            self._tagger = Tagger()
            logger.info("Fugashi (MeCab) morphological analyzer initialized.")
        except Exception as exc:
            logger.warning("Fugashi unavailable (%s); using longest-prefix dictionary scan only.", exc)

        # 4. Initialize JMdict SQLite database via Jamdict (reuse_ctx=True with read-only URI)
        try:
            from jamdict import Jamdict
            db_path = find_jamdict_db_path()
            if db_path:
                logger.info("Found Jamdict database at %s", db_path)
            self._jamdict = Jamdict(db_file=db_path, reuse_ctx=True)
            if db_path and os.path.isfile(db_path):
                try:
                    with sqlite3.connect(db_path) as conn:
                        cur = conn.cursor()
                        cur.execute("SELECT text FROM Kanji UNION ALL SELECT text FROM Kana")
                        self._jmd_known_terms = {r[0] for r in cur.fetchall() if r[0]}
                    self._jamdict.lookup("あ", lookup_chars=False, lookup_ne=False)
                    logger.info("Preloaded %d JMdict headwords/readings for instant pre-filtering.", len(self._jmd_known_terms))
                except Exception as pre_err:
                    logger.warning("Failed to preload JMdict terms set: %s", pre_err)
            logger.info("Jamdict (JMdict) database initialized.")
        except Exception as exc:
            logger.error(
                "Jamdict unavailable (%s). Install via: pip install jamdict jamdict-data",
                exc,
            )

        # 5. Initialize External Yomitan Dictionaries from dictionaries/ (e.g. Babylon Japanese-English)
        try:
            self._yomitan_mgr = YomitanDictionaryManager(dict_dir=DICTIONARIES_DIR)
            if self._yomitan_mgr.known_terms:
                self._jmd_known_terms.update(self._yomitan_mgr.known_terms)
                logger.info(
                    "Integrated %d external dictionary terms into compound recognition set.",
                    len(self._yomitan_mgr.known_terms),
                )
        except Exception as exc:
            logger.warning("External Yomitan dictionary initialization failed: %s", exc)
            self._yomitan_mgr = None

        # 6. Merge built-in colloquial/slang expressions not covered as single headwords in base JMdict
        for k, entries in self._BUILTIN_COLLOQUIAL_DICT.items():
            existing = self._custom_dict.setdefault(k, [])
            for ent in entries:
                if ent not in existing:
                    existing.append(ent)

    @property
    def jamdict(self):
        return self._jamdict

    @property
    def word_meta_mgr(self) -> WordMetaManager:
        return self._word_meta_mgr

    def _load_custom_dictionaries(self) -> None:
        """Scan and load all custom / game dictionaries from dictionaries/ and APP_DIR."""
        self._custom_dict.clear()
        self._custom_voicing_index.clear()
        custom_dict_candidates: List[str] = [
            os.path.join(DICTIONARIES_DIR, "custom_dict.json"),
            os.path.join(APP_DIR, "custom_dict.json"),
        ]
        if os.path.isdir(DICTIONARIES_DIR):
            for fname in sorted(os.listdir(DICTIONARIES_DIR)):
                if (
                    fname.endswith(".json")
                    and fname not in ("index.json", "kangxi_radicals.json")
                    and not fname.startswith("term_bank_")
                    and not fname.startswith("jlpt_")
                ):
                    fpath = os.path.join(DICTIONARIES_DIR, fname)
                    if fpath not in custom_dict_candidates:
                        custom_dict_candidates.append(fpath)

        for c_path in custom_dict_candidates:
            if os.path.exists(c_path):
                try:
                    with open(c_path, "r", encoding="utf-8") as f:
                        loaded_cd = json.load(f)
                        added_count = 0
                        if isinstance(loaded_cd, dict):
                            for k, v in loaded_cd.items():
                                if isinstance(v, list):
                                    valid = [
                                        e for e in v
                                        if isinstance(e, dict) and "headword" in e and "glosses" in e
                                    ]
                                    if valid:
                                        self._custom_dict.setdefault(k, []).extend(valid)
                                        added_count += 1
                                elif isinstance(v, dict) and "headword" in v and "glosses" in v:
                                    self._custom_dict.setdefault(k, []).append(v)
                                    added_count += 1
                    if added_count > 0:
                        logger.info("Loaded custom/Pokémon dictionary terms from %s (total: %d terms).", c_path, len(self._custom_dict))
                except Exception as exc:
                    logger.warning("Failed to load custom dictionary %s: %s", c_path, exc)

        for k in self._custom_dict:
            if len(k) >= 3 and all("\u30a1" <= c <= "\u30fc" for c in k):
                v_key = self._katakana_voicing_key(k)
                self._custom_voicing_index.setdefault(v_key, []).append(k)

    _BUILTIN_COLLOQUIAL_DICT: Dict[str, List[dict]] = {
        "どうだろ": [{
            "headword": "どうだろう",
            "reading": "どうだろう",
            "pos": "[colloquial contraction of どうだろう] expression",
            "glosses": [
                "1. I wonder how it is; how about that?; I'm not sure",
                "2. what do you think? (casual u-dropped form of どうだろう)",
            ],
        }],
        "どうだろう": [{
            "headword": "どうだろう",
            "reading": "どうだろう",
            "pos": "[volitional] expression",
            "glosses": [
                "1. I wonder how it is; how about that?; I'm not sure",
                "2. what do you think? (如何 【どう】 + だろう)",
            ],
        }],
        "なんだろ": [{
            "headword": "なんだろう",
            "reading": "なんだろう",
            "pos": "[colloquial contraction of 何だろう] expression",
            "glosses": ["1. I wonder what it is; what could it be?"],
        }],
        "なんだろう": [{
            "headword": "何だろう",
            "reading": "なんだろう",
            "pos": "[volitional] expression",
            "glosses": ["1. I wonder what it is; what could it be?"],
        }],
        "なんでだろ": [{
            "headword": "なんでだろう",
            "reading": "なんでだろう",
            "pos": "[colloquial contraction of 何でだろう] expression",
            "glosses": ["1. I wonder why; why could that be?"],
        }],
        "なんでだろう": [{
            "headword": "何でだろう",
            "reading": "なんでだろう",
            "pos": "[volitional] expression",
            "glosses": ["1. I wonder why; why could that be?"],
        }],
        "だれだろ": [{
            "headword": "誰だろう",
            "reading": "だれだろう",
            "pos": "[colloquial contraction of 誰だろう] expression",
            "glosses": ["1. I wonder who it is; who could it be?"],
        }],
        "どこだろ": [{
            "headword": "どこだろう",
            "reading": "どこだろう",
            "pos": "[colloquial contraction of どこだろう] expression",
            "glosses": ["1. I wonder where it is; where could it be?"],
        }],
        "わかんない": [{
            "headword": "分からない",
            "reading": "わからない",
            "pos": "[colloquial contraction of わからない] expression",
            "glosses": ["1. I don't know; I don't understand; hard to tell"],
        }],
        "つまんない": [{
            "headword": "詰まらない",
            "reading": "つまらない",
            "pos": "[colloquial contraction of つまらない] adjective (keiyoushi)",
            "glosses": ["1. boring; dull; uninteresting; trivial"],
        }],
        "しょうがない": [{
            "headword": "仕様がない",
            "reading": "しょうがない",
            "pos": "[colloquial] expression, adjective (keiyoushi)",
            "glosses": ["1. it can't be helped; nothing can be done; unavoidable"],
        }],
        "しゃーない": [{
            "headword": "仕様がない",
            "reading": "しょうがない",
            "pos": "[slang/dialect contraction of しょうがない] expression",
            "glosses": ["1. it can't be helped; nothing can be done"],
        }],
        "すげえ": [{
            "headword": "凄い",
            "reading": "すごい",
            "pos": "[slang vowel slur of すごい] adjective (keiyoushi)",
            "glosses": ["1. amazing; awesome; incredible; terrible"],
        }],
        "すげー": [{
            "headword": "凄い",
            "reading": "すごい",
            "pos": "[slang vowel slur of すごい] adjective (keiyoushi)",
            "glosses": ["1. amazing; awesome; incredible; terrible"],
        }],
        "やべえ": [{
            "headword": "やばい",
            "reading": "やばい",
            "pos": "[slang vowel slur of やばい] adjective (keiyoushi)",
            "glosses": ["1. dangerous; oh no; awful; crazy; awesome (slang)"],
        }],
        "やべー": [{
            "headword": "やばい",
            "reading": "やばい",
            "pos": "[slang vowel slur of やばい] adjective (keiyoushi)",
            "glosses": ["1. dangerous; oh no; awful; crazy; awesome (slang)"],
        }],
        "じゃん": [{
            "headword": "じゃん",
            "reading": "じゃん",
            "pos": "[colloquial contraction of じゃないか] particle",
            "glosses": ["1. isn't it?; right?; why not? (emphatic sentence-ending particle)"],
        }],
        "だっけ": [{
            "headword": "だっけ",
            "reading": "だっけ",
            "pos": "[colloquial] expression, particle",
            "glosses": ["1. was it ...?; what was it again? (trying to recall something)"],
        }],
        "っけ": [{
            "headword": "っけ",
            "reading": "っけ",
            "pos": "[colloquial] particle",
            "glosses": ["1. particle indicating that the speaker is trying to recall something ('was it ...?')"],
        }],
        "っす": [{
            "headword": "っす",
            "reading": "っす",
            "pos": "[colloquial contraction of です] auxiliary verb",
            "glosses": ["1. casual/slang contraction of です (be; is)"],
        }],
        "しても": [{
            "headword": "しても",
            "reading": "しても",
            "pos": "[te-form of する + も] expression, grammar",
            "glosses": [
                "1. even if (one does); even though; even doing",
                "2. [in としても] even as; as ... as well; granting that",
            ],
        }],
        "としても": [{
            "headword": "としても",
            "reading": "としても",
            "pos": "[と + して + も] expression, compound particle",
            "glosses": [
                "1. even as (e.g. 'famous even as a resort'); as ... as well",
                "2. even if ...; granting that ...; supposing that ...",
            ],
        }],
        "として": [{
            "headword": "として",
            "reading": "として",
            "pos": "[と + して] expression, compound particle",
            "glosses": [
                "1. as (i.e. in the role or capacity of)",
                "2. for (i.e. from the viewpoint of)",
            ],
        }],
        "にしても": [{
            "headword": "にしても",
            "reading": "にしても",
            "pos": "[に + して + も] expression, compound particle",
            "glosses": ["1. even if; granting that; even so; regardless"],
        }],
        "っていっても": [{
            "headword": "って言っても",
            "reading": "っていっても",
            "pos": "[colloquial contraction of と言っても] expression",
            "glosses": ["1. even though one says ...; although I say ...; having said that"],
        }],
        "といっても": [{
            "headword": "と言っても",
            "reading": "といっても",
            "pos": "expression",
            "glosses": ["1. even though one says ...; although I say ...; having said that"],
        }],
        "なのですね": [{
            "headword": "なのですね",
            "reading": "なのですね",
            "pos": "[attributive だ (な) + explanatory の + です + ね] expression, grammar",
            "glosses": [
                "1. so (you) are ...; it is that ..., isn't it?; I see that ...",
                "2. polite explanatory confirmation after a noun or na-adjective (だ becomes な before の)",
            ],
        }],
        "なのです": [{
            "headword": "なのです",
            "reading": "なのです",
            "pos": "[attributive だ (な) + explanatory のです] expression, grammar",
            "glosses": [
                "1. it is the case that ...; the thing is that ...; the reason is that ...",
                "2. polite explanatory copula after a noun or na-adjective (だ becomes な before の)",
            ],
        }],
        "なのだ": [{
            "headword": "なのだ",
            "reading": "なのだ",
            "pos": "[attributive だ (な) + explanatory のだ] expression, grammar",
            "glosses": [
                "1. it is the case that ...; the expectation/reason is that ...",
                "2. plain explanatory copula after a noun or na-adjective (だ becomes な before の)",
            ],
        }],
        "なの": [{
            "headword": "なの",
            "reading": "なの",
            "pos": "[attributive だ (な) + explanatory の] expression, particle",
            "glosses": [
                "1. is it that ...? (with rising intonation); it is that ... (explanatory)",
                "2. formed from attributive copula な (of だ) + nominalizing/explanatory particle の",
            ],
        }],
        "なのか": [{
            "headword": "なのか",
            "reading": "なのか",
            "pos": "[attributive だ (な) + の + か] expression, grammar",
            "glosses": [
                "1. is it really that ...?; whether it is ...",
            ],
        }],
        "なのかな": [{
            "headword": "なのかな",
            "reading": "なのかな",
            "pos": "[attributive だ (な) + の + かな] expression, grammar",
            "glosses": [
                "1. I wonder if it is ...; could it be ...?",
            ],
        }],
        "なので": [{
            "headword": "なので",
            "reading": "なので",
            "pos": "[attributive だ (な) + の + で] conjunction, expression",
            "glosses": [
                "1. because (it is) ...; since ...; as ...",
            ],
        }],
        "なのに": [{
            "headword": "なのに",
            "reading": "なのに",
            "pos": "[attributive だ (な) + の + に] conjunction, expression",
            "glosses": [
                "1. even though (it is) ...; despite ...; and yet ...",
            ],
        }],
    }

    @staticmethod
    def _expand_colloquial_forms(term: str) -> List[Tuple[str, str]]:
        """
        Generate standard Japanese dictionary forms from common casual/slang phonetic
        contractions (e.g., u-dropping '～だろ' -> '～だろう', '～んない' -> '～らない',
        vowel slurs '～ねえ'/'～ねー' -> '～ない') so lookups succeed even when the
        contracted surface isn't an explicit headword in JMdict.
        """
        expansions: List[Tuple[str, str]] = []
        if len(term) < 2:
            return expansions
        if term.endswith("だろ"):
            expansions.append((term + "う", "colloquial u-dropped volitional"))
        if term.endswith("でしょ"):
            expansions.append((term + "う", "colloquial u-dropped polite volitional"))
        if term.endswith("んない") and len(term) >= 4:
            expansions.append((term[:-3] + "らない", "colloquial -nnai -> -ranai"))
            expansions.append((term[:-3] + "る", "colloquial negative (-nnai)"))
        if (term.endswith("ねえ") or term.endswith("ねー")) and len(term) >= 3:
            expansions.append((term[:-2] + "ない", "slang vowel slur (-nee -> -nai)"))
        if term.endswith("ちゃった") and len(term) >= 5:
            expansions.append((term[:-4] + "てしまった", "colloquial completion (-chatta)"))
        if term.endswith("じゃった") and len(term) >= 5:
            expansions.append((term[:-4] + "でしまった", "colloquial completion (-jatta)"))
        if term.endswith("ちゃう") and len(term) >= 4:
            expansions.append((term[:-3] + "てしまう", "colloquial completion (-chau)"))
        if term.endswith("じゃう") and len(term) >= 4:
            expansions.append((term[:-3] + "でしまう", "colloquial completion (-jau)"))
        return expansions

    @staticmethod
    def _katakana_voicing_key(text: str) -> str:
        """
        Normalize Katakana by stripping combining dakuten (U+3099) and handakuten (U+309A)
        so 1-pixel 3DS screen confusions like 'アプリー' vs 'アブリー' or 'ボケモン' vs 'ポケモン'
        share an identical voicing-insensitive base key ('アフリー', 'ホケモン').
        """
        out: List[str] = []
        for ch in text:
            if "\u30a1" <= ch <= "\u30f6":
                decomp = unicodedata.normalize("NFD", ch)
                stripped = "".join(c for c in decomp if c not in ("\u3099", "\u309a"))
                out.append(unicodedata.normalize("NFC", stripped))
            else:
                out.append(ch)
        return "".join(out)

    def _correct_katakana_voicing(self, text: str) -> str:
        """
        Post-process recognized OCR text to repair 1-pixel dakuten/handakuten confusions in
        Katakana runs (e.g. 'アプリー' -> 'アブリー', 'ボケモン' -> 'ポケモン') while preserving
        exact string length so per-character screen coordinates remain 1-to-1 aligned.
        """
        if not text or not self._custom_voicing_index:
            return text

        def _replace_run(match: re.Match) -> str:
            run = match.group(0)
            if len(run) < 3 or run in self._custom_dict:
                return run
            v_key = self._katakana_voicing_key(run)
            matches = self._custom_voicing_index.get(v_key)
            if matches and len(matches[0]) == len(run):
                return matches[0]
            return run

        return re.sub(r"[\u30a1-\u30fc]{3,}", _replace_run, text)

    @staticmethod
    def _find_main_vertical_band(ink: np.ndarray) -> Optional[Tuple[int, int]]:
        """
        Locate the primary vertical text band `(y1_rel, y2_rel)` within `ink`, ignoring
        thin top/bottom border overlays (such as the speaker nameplate border 'ママ' sitting
        immediately above the first line of dialogue, separated by a 1-2px gap row).
        """
        _, cw = ink.shape
        row_counts = ink.sum(axis=1)
        valid = (row_counts >= 2) & (row_counts <= max(2, int(cw * 0.78)))
        ys = np.where(valid)[0]
        if ys.size == 0:
            ys = np.where(ink.any(axis=1))[0]
            if ys.size == 0:
                return None
            return int(ys[0]), int(ys[-1]) + 1

        bands: List[Tuple[int, int]] = []
        bs = bp = int(ys[0])
        for yv in ys[1:]:
            yv_i = int(yv)
            if yv_i > bp + 1:
                bands.append((bs, bp + 1))
                bs = yv_i
            bp = yv_i
        bands.append((bs, bp + 1))
        return max(bands, key=lambda b: b[1] - b[0])

    @staticmethod
    def _color_aware_gray(rgb_arr: np.ndarray) -> np.ndarray:
        """
        Convert RGB image `(H, W, 3)` to a contrast map where text is dark and background is bright:
        - Normal black/dark text: standard luminance (0.299*R + 0.587*G + 0.114*B) is dark (~40).
        - Colored text (e.g. red '島巡り' R~230, G~45, B~45): the absorbed channels (G, B) are low,
          so min(G, B) makes red text dark (~45), preserving high contrast against light background.
        - Cyan/blue UI borders (e.g. 'ママ' or 'ククイ' speaker nameplate pill B~230, R~20):
          high B and low R is UI chrome, so it maps to bright background (~230) rather than text.
        """
        r = rgb_arr[:, :, 0]
        g = rgb_arr[:, :, 1]
        b = rgb_arr[:, :, 2]
        l_std = 0.299 * r + 0.587 * g + 0.114 * b

        is_cyan_ui = (b > 160.0) & (b > r + 60.0)
        is_reddish = (r > 120.0) & (r > g + 25.0) & (r > b + 25.0)
        is_greenish = (g > 120.0) & (g > r + 25.0) & (g > b + 25.0)

        out = l_std.copy()
        out[is_reddish] = np.minimum(g[is_reddish], b[is_reddish])
        out[is_greenish] = np.minimum(r[is_greenish], b[is_greenish])
        out[is_cyan_ui] = b[is_cyan_ui]
        return out

    @classmethod
    def _compute_crop_ink_mask(cls, crop_rgb: np.ndarray) -> Optional[np.ndarray]:
        """
        Compute a binary foreground ink mask for an RGB crop `(ch, cw, 3)`.
        - For light text on dark/colored backgrounds, uses luminance `l_std - bg_l`.
        - For dark or colored text on light backgrounds (e.g. red highlight text '島巡り'),
          uses `_color_aware_gray` so colored text registers as foreground ink while
          cyan/blue UI borders map to bright background.
        """
        ch, cw = crop_rgb.shape[:2]
        y_in = max(1, ch // 5)
        x_in = max(1, cw // 12)
        l_std = 0.299 * crop_rgb[:, :, 0] + 0.587 * crop_rgb[:, :, 1] + 0.114 * crop_rgb[:, :, 2]
        core_l = l_std[y_in : max(y_in + 1, ch - y_in), x_in : max(x_in + 1, cw - x_in)]
        if core_l.size == 0:
            core_l = l_std
        bg_l = float(np.median(core_l))
        p10 = float(np.percentile(core_l, 10))
        p90 = float(np.percentile(core_l, 90))

        # Check for saturated colored badges with white/light text (e.g. paralysis 'まひ', poison 'どく')
        r_c = crop_rgb[:, :, 0]
        g_c = crop_rgb[:, :, 1]
        b_c = crop_rgb[:, :, 2]
        sat_map = np.maximum(np.maximum(r_c, g_c), b_c) - np.minimum(np.minimum(r_c, g_c), b_c)
        val_map = np.minimum(np.minimum(r_c, g_c), b_c)
        core_sat = sat_map[y_in : max(y_in + 1, ch - y_in), x_in : max(x_in + 1, cw - x_in)]
        med_sat = float(np.median(core_sat)) if core_sat.size > 0 else 0.0

        if cw <= 55 and med_sat >= 70.0 and bg_l <= 180.0:
            is_text = (val_map > 130.0) & (sat_map < 75.0)
            ink = is_text.copy()
            for r_idx in range(min(5, ch // 3)):
                if ink[r_idx, :].mean() > 0.35:
                    ink[r_idx, :] = False
            for r_idx in range(max(0, ch - min(5, ch // 3)), ch):
                if ink[r_idx, :].mean() > 0.35:
                    ink[r_idx, :] = False
            return ink if ink.any() else None

        if (p90 - bg_l) > (bg_l - p10):
            diff = np.maximum(0.0, l_std - bg_l)
            max_diff = float(diff.max())
            if max_diff < 18.0:
                return None
            return diff >= max(20.0, max_diff * 0.28)
        else:
            gray = cls._color_aware_gray(crop_rgb)
            core_g = gray[y_in : max(y_in + 1, ch - y_in), x_in : max(x_in + 1, cw - x_in)]
            if core_g.size == 0:
                core_g = gray
            bg_g = float(np.median(core_g))
            diff = np.maximum(0.0, bg_g - gray)
            max_diff = float(diff.max())
            if max_diff < 18.0:
                return None
            return diff >= max(22.0, max_diff * 0.28)

    def _recognize_line_crop_with_meta(
        self,
        screen_img: Image.Image,
        bbox: Tuple[int, int, int, int],
        keep_punct: bool = False,
    ) -> Tuple[str, bool, bool]:
        """
        Recognize Japanese text inside a single detected line bounding box.
        Normalizes polarity (inverting light-on-dark 3DS game text to dark-on-light),
        applies autocontrast, and adds a clean white border before upscaling for MangaOCR.
        Returns (cleaned_text, has_leading_punct, has_trailing_punct).
        """
        w, h = screen_img.size
        x1 = max(0, min(w, int(bbox[0]) - 2))
        y1 = max(0, min(h, int(bbox[1]) - 2))
        x2 = max(0, min(w, int(bbox[2]) + 2))
        y2 = max(0, min(h, int(bbox[3]) + 2))
        if x2 <= x1 or y2 <= y1:
            return "", False, False
        rgb_crop = screen_img.crop((x1, y1, x2, y2)).convert("RGB")
        rgb_arr = np.array(rgb_crop, dtype=np.float32)
        crop = rgb_crop.convert("L")

        arr = np.array(crop, dtype=np.float32)
        if arr.size > 0 and arr.shape[0] >= 4 and arr.shape[1] >= 4:
            ch, cw = arr.shape
            y_in = max(1, ch // 5)
            x_in = max(1, cw // 12)
            core = arr[
                y_in : max(y_in + 1, ch - y_in),
                x_in : max(x_in + 1, cw - x_in),
            ]
            if core.size == 0:
                core = arr
            bg_val = float(np.median(core))
            p10 = float(np.percentile(core, 10))
            p90 = float(np.percentile(core, 90))

            # Detect colored status badges / buttons (e.g. Pokémon paralysis badge 'まひ',
            # poison 'どく', burn 'やけど', freeze 'こおり') with white/light text.
            r_c = rgb_arr[:, :, 0]
            g_c = rgb_arr[:, :, 1]
            b_c = rgb_arr[:, :, 2]
            sat_map = np.maximum(np.maximum(r_c, g_c), b_c) - np.minimum(np.minimum(r_c, g_c), b_c)
            val_map = np.minimum(np.minimum(r_c, g_c), b_c)
            core_sat = sat_map[y_in : max(y_in + 1, ch - y_in), x_in : max(x_in + 1, cw - x_in)]
            med_sat = float(np.median(core_sat)) if core_sat.size > 0 else 0.0

            if cw <= 55 and med_sat >= 70.0 and bg_val <= 180.0:
                is_text = (val_map > 130.0) & (sat_map < 75.0)
                badge_gray = np.full((ch, cw), 255.0, dtype=np.float32)
                badge_gray[is_text] = 0.0
                for r_idx in range(min(5, ch // 3)):
                    if (badge_gray[r_idx, :] == 0.0).mean() > 0.35:
                        badge_gray[r_idx, :] = 255.0
                for r_idx in range(max(0, ch - min(5, ch // 3)), ch):
                    if (badge_gray[r_idx, :] == 0.0).mean() > 0.35:
                        badge_gray[r_idx, :] = 255.0
                crop = Image.fromarray(badge_gray.astype(np.uint8), mode="L")
            elif (p90 - bg_val) > (bg_val - p10):
                # Light text on darker/colored background (e.g. dark blue dialogue box or
                # yellow-green '消す'/'おわり' buttons): clamp any dark button outline up to
                # `bg_val` and invert so glyphs become dark-on-white.
                clamped = np.clip(arr, bg_val, 255.0)
                crop = ImageOps.invert(Image.fromarray(clamped.astype(np.uint8), mode="L"))
            else:
                cleaned_arr = self._color_aware_gray(rgb_arr)
                core_max = cleaned_arr[
                    y_in : max(y_in + 1, ch - y_in),
                    x_in : max(x_in + 1, cw - x_in),
                ]
                bg_max = float(np.median(core_max)) if core_max.size > 0 else float(np.median(cleaned_arr))
                dark_mask = cleaned_arr < (bg_max - 22.0)
                for r_idx in range(min(5, ch // 3)):
                    if 0 <= r_idx < ch and float(dark_mask[r_idx, :].mean()) > 0.55:
                        cleaned_arr[r_idx, :] = bg_max
                for r_idx in range(max(0, ch - min(5, ch // 3)), ch):
                    if 0 <= r_idx < ch and float(dark_mask[r_idx, :].mean()) > 0.55:
                        cleaned_arr[r_idx, :] = bg_max
                clamped = np.clip(cleaned_arr, 0.0, bg_max)
                crop = Image.fromarray(clamped.astype(np.uint8), mode="L")

        crop = ImageOps.autocontrast(crop, cutoff=1)
        crop = ImageOps.expand(crop, border=6, fill=255)

        punct_chars = r"…・。、．，！？!?\-―～~「」『』（）()\[\]【】［］｛｝｜|¦＜＞<>"
        if self._manga_ocr is not None:
            # Pass native 1x resolution directly to MangaOCR so its internal 224x224 ViTImageProcessor
            # resizes in a single clean step without 3x Lanczos double-resampling ringing artifacts.
            raw = self._manga_ocr(crop.convert("RGB"))
            raw_nowhite = self._correct_katakana_voicing(re.sub(r"\s+", "", raw))
            if keep_punct:
                return raw_nowhite, False, False
            cleaned = re.sub(f"[\\s{punct_chars}]+", "", raw)
            cleaned = self._correct_katakana_voicing(cleaned)
            if cleaned:
                has_lead = bool(re.match(f"^[{punct_chars}]+", raw_nowhite))
                has_trail = bool(re.search(f"[{punct_chars}]+$", raw_nowhite))
                return cleaned, has_lead, has_trail
            return "", False, False

        up_crop = crop.resize(
            (max(1, crop.width * OCR_UPSCALE_FACTOR), max(1, crop.height * OCR_UPSCALE_FACTOR)),
            Image.Resampling.LANCZOS,
        ).convert("RGB")
        try:
            import pytesseract
            is_vert = (y2 - y1) > (x2 - x1) * 1.5
            lang = "jpn_vert" if is_vert else "jpn"
            psm = "--psm 5" if is_vert else "--psm 7"
            raw = pytesseract.image_to_string(up_crop, lang=lang, config=psm)
            raw_nowhite = self._correct_katakana_voicing(re.sub(r"\s+", "", raw))
            if keep_punct:
                return raw_nowhite, False, False
            cleaned = re.sub(f"[\\s{punct_chars}]+", "", raw)
            if cleaned:
                has_lead = bool(re.match(f"^[{punct_chars}]+", raw_nowhite))
                has_trail = bool(re.search(f"[{punct_chars}]+$", raw_nowhite))
                return cleaned, has_lead, has_trail
            return "", False, False
        except Exception as exc:
            logger.error("Line OCR failed: %s", exc)
            return "", False, False

    def _recognize_line_crop(self, screen_img: Image.Image, bbox: Tuple[int, int, int, int]) -> str:
        text, _, _ = self._recognize_line_crop_with_meta(screen_img, bbox)
        return text

    def _clean_ocr_text_pair(self, raw_str: str) -> Tuple[str, str, bool, bool]:
        """Normalize raw OCR text into `(cleaned_text, raw_with_punct, has_leading_punct, has_trailing_punct)`."""
        punct_chars = r"\.…‥・。、．，！？!?\-―～~「」『』（）()\[\]【】［］｛｝｜|¦＜＞<>,;"
        counters = r"[\d番号円年月日時分秒つ個人匹本体台頭羽ばんごうえんにん]"
        norm = unicodedata.normalize("NFKC", raw_str or "")
        norm = re.sub(r"^\d{2,6}$", "...", norm)
        norm = re.sub(rf"^\d{{2,6}}(?!{counters})(?=[\u3040-\u30ff\u4e00-\u9fff])", "...", norm)
        norm = re.sub(rf"(?<=[\u3040-\u30ff\u4e00-\u9fff])\d{{2,6}}(?!{counters})(?=[\u3040-\u30ff\u4e00-\u9fff])", "...", norm)
        norm = re.sub(r"(?<=[\u3040-\u30ff\u4e00-\u9fff])\d{2,6}$", "...", norm)
        raw_nw = self._correct_katakana_voicing(re.sub(r"\s+", "", norm))
        clean_r = re.sub(r"^[「『（(\[<]+", "", raw_nw)
        clean_r = re.sub(r"[」』）)\]>]+$", "", clean_r).strip()
        clean_r = re.sub(r"^\.+", "……", clean_r)
        clean_r = re.sub(r"\.+$", "……", clean_r)
        cleaned = re.sub(f"[\\s{punct_chars}]+", "", norm)
        cleaned = self._correct_katakana_voicing(cleaned)
        if cleaned:
            if is_noise_chunk(norm):
                return "", "", False, False
            h_lead = bool(re.match(f"^[{punct_chars}]+", raw_nw))
            h_trail = bool(re.search(f"[{punct_chars}]+$", raw_nw))
            return cleaned, (clean_r or cleaned), h_lead, h_trail
        return "", "", False, False

    def _recognize_line_crop_all_engines(
        self,
        screen_img: Image.Image,
        bbox: Tuple[int, int, int, int],
    ) -> Tuple[str, bool, bool, str, Dict[str, str], Dict[str, str]]:
        """
        Run a single line bounding box through all 4 Japanese OCR engines:
          1. MangaOCR (`"Manga"`)
          2. PaddleOCR Japanese v4 (`"Paddle"`)
          3. YomiToku PARSeq-Small (`"Yomi"`)
          4. NDLOCR-Lite PARSeq (`"NDL"`)
        Returns `(primary_text, has_lead, has_trail, primary_raw, engine_texts, engine_raw_texts)`.
        """
        w, h = screen_img.size
        x1 = max(0, min(w, int(bbox[0]) - 2))
        y1 = max(0, min(h, int(bbox[1]) - 2))
        x2 = max(0, min(w, int(bbox[2]) + 2))
        y2 = max(0, min(h, int(bbox[3]) + 2))
        if x2 <= x1 or y2 <= y1:
            empty = {eng: "" for eng in OCR_ENGINE_NAMES}
            return "", False, False, "", dict(empty), dict(empty), []
        rgb_crop = screen_img.crop((x1, y1, x2, y2)).convert("RGB")
        rgb_arr = np.array(rgb_crop, dtype=np.float32)
        crop = rgb_crop.convert("L")
        is_light_on_dark = False

        arr = np.array(crop, dtype=np.float32)
        if arr.size > 0 and arr.shape[0] >= 4 and arr.shape[1] >= 4:
            ch, cw = arr.shape
            y_in = max(1, ch // 5)
            x_in = max(1, cw // 12)
            core = arr[
                y_in : max(y_in + 1, ch - y_in),
                x_in : max(x_in + 1, cw - x_in),
            ]
            if core.size == 0:
                core = arr
            bg_val = float(np.median(core))
            p10 = float(np.percentile(core, 10))
            p90 = float(np.percentile(core, 90))

            # Detect colored status badges / buttons (e.g. Pokémon paralysis badge 'まひ',
            # poison 'どく', burn 'やけど', freeze 'こおり') with white/light text.
            r_c = rgb_arr[:, :, 0]
            g_c = rgb_arr[:, :, 1]
            b_c = rgb_arr[:, :, 2]
            sat_map = np.maximum(np.maximum(r_c, g_c), b_c) - np.minimum(np.minimum(r_c, g_c), b_c)
            val_map = np.minimum(np.minimum(r_c, g_c), b_c)
            core_sat = sat_map[y_in : max(y_in + 1, ch - y_in), x_in : max(x_in + 1, cw - x_in)]
            med_sat = float(np.median(core_sat)) if core_sat.size > 0 else 0.0

            if cw <= 55 and med_sat >= 70.0 and bg_val <= 180.0:
                is_text = (val_map > 130.0) & (sat_map < 75.0)
                badge_gray = np.full((ch, cw), 255.0, dtype=np.float32)
                badge_gray[is_text] = 0.0
                for r_idx in range(min(5, ch // 3)):
                    if (badge_gray[r_idx, :] == 0.0).mean() > 0.35:
                        badge_gray[r_idx, :] = 255.0
                for r_idx in range(max(0, ch - min(5, ch // 3)), ch):
                    if (badge_gray[r_idx, :] == 0.0).mean() > 0.35:
                        badge_gray[r_idx, :] = 255.0
                crop = Image.fromarray(badge_gray.astype(np.uint8), mode="L")
            elif (p90 - bg_val) > (bg_val - p10):
                is_light_on_dark = True
                clamped = np.clip(arr, bg_val, 255.0)
                crop = ImageOps.invert(Image.fromarray(clamped.astype(np.uint8), mode="L"))
            else:
                cleaned_arr = self._color_aware_gray(rgb_arr)
                core_max = cleaned_arr[
                    y_in : max(y_in + 1, ch - y_in),
                    x_in : max(x_in + 1, cw - x_in),
                ]
                bg_max = float(np.median(core_max)) if core_max.size > 0 else float(np.median(cleaned_arr))
                dark_mask = cleaned_arr < (bg_max - 22.0)
                for r_idx in range(min(5, ch // 3)):
                    if 0 <= r_idx < ch and float(dark_mask[r_idx, :].mean()) > 0.55:
                        cleaned_arr[r_idx, :] = bg_max
                for r_idx in range(max(0, ch - min(5, ch // 3)), ch):
                    if 0 <= r_idx < ch and float(dark_mask[r_idx, :].mean()) > 0.55:
                        cleaned_arr[r_idx, :] = bg_max
                clamped = np.clip(cleaned_arr, 0.0, bg_max)
                crop = Image.fromarray(clamped.astype(np.uint8), mode="L")

        crop_autocontrast = ImageOps.autocontrast(crop, cutoff=1)
        crop_padded_rgb = ImageOps.expand(crop_autocontrast, border=6, fill=255).convert("RGB")
        tight_for_ndl = crop_autocontrast.convert("RGB") if is_light_on_dark else rgb_crop

        _clean_pair = self._clean_ocr_text_pair

        engine_texts: Dict[str, str] = {}
        engine_raw_texts: Dict[str, str] = {}
        has_lead_map: Dict[str, bool] = {}
        has_trail_map: Dict[str, bool] = {}

        # 1. MangaOCR
        if self._manga_ocr is not None:
            try:
                m_raw = self._manga_ocr(crop_padded_rgb)
                c_txt, r_txt, hl, ht = _clean_pair(m_raw)
                engine_texts["Manga"] = c_txt
                engine_raw_texts["Manga"] = r_txt
                has_lead_map["Manga"] = hl
                has_trail_map["Manga"] = ht
            except Exception as exc:
                logger.debug("MangaOCR failed on %s: %s", bbox, exc)

        # 2. PaddleOCR Japanese v4
        if self._paddle_ocr is not None:
            try:
                bgr_pad = np.array(crop_padded_rgb)[:, :, ::-1]
                p_res, _ = self._paddle_ocr([bgr_pad])
                p_raw = p_res[0][0] if (p_res and p_res[0]) else ""
                c_txt, r_txt, hl, ht = _clean_pair(p_raw)
                engine_texts["Paddle"] = c_txt
                engine_raw_texts["Paddle"] = r_txt
                has_lead_map["Paddle"] = hl
                has_trail_map["Paddle"] = ht
            except Exception as exc:
                logger.debug("PaddleOCR failed on %s: %s", bbox, exc)

        # 3. YomiToku PARSeq-Small
        if self._yomi_ocr is not None:
            try:
                y_raw = self._yomi_ocr.recognize(crop_padded_rgb)
                c_txt, r_txt, hl, ht = _clean_pair(y_raw)
                engine_texts["Yomi"] = c_txt
                engine_raw_texts["Yomi"] = r_txt
                has_lead_map["Yomi"] = hl
                has_trail_map["Yomi"] = ht
            except Exception as exc:
                logger.debug("YomiToku failed on %s: %s", bbox, exc)

        # 4. NDLOCR-Lite PARSeq
        if self._ndl_sess is not None and self._ndl_charlist:
            try:
                ndl_rgb = np.array(tight_for_ndl)
                nh, nw = ndl_rgb.shape[:2]
                if nh > nw:
                    ndl_rgb = np.rot90(ndl_rgb, 1)
                ndl_resized = np.array(
                    Image.fromarray(ndl_rgb).resize((384, 16), Image.Resampling.BICUBIC),
                    dtype=np.float32,
                )
                inp = (ndl_resized / 127.5 - 1.0).transpose(2, 0, 1)[np.newaxis, :, :, :]
                out = self._ndl_sess.run(None, {self._ndl_sess.get_inputs()[0].name: inp})[0][0]
                indices = np.argmax(out, axis=1)
                stop_idx = np.where(indices == 0)[0]
                end_pos = int(stop_idx[0]) if stop_idx.size > 0 else len(indices)
                n_raw = "".join(
                    self._ndl_charlist[i - 1]
                    for i in indices[:end_pos].tolist()
                    if 0 < i <= len(self._ndl_charlist)
                )
                c_txt, r_txt, hl, ht = _clean_pair(n_raw)
                engine_texts["NDL"] = c_txt
                engine_raw_texts["NDL"] = r_txt
                has_lead_map["NDL"] = hl
                has_trail_map["NDL"] = ht
            except Exception as exc:
                logger.debug("NDLOCR failed on %s: %s", bbox, exc)

        primary_text = engine_texts.get("Manga", "")
        primary_raw = engine_raw_texts.get("Manga", "")
        has_lead = any(has_lead_map.values())
        has_trail = any(has_trail_map.values())
        if not primary_text:
            for eng in OCR_ENGINE_NAMES:
                if engine_texts.get(eng):
                    primary_text = engine_texts[eng]
                    primary_raw = engine_raw_texts.get(eng, primary_text)
                    break

        for eng in OCR_ENGINE_NAMES:
            if not engine_texts.get(eng):
                engine_texts[eng] = primary_text
                engine_raw_texts[eng] = primary_raw

        # Short phrase runner-up / tying detection (<= 2 characters)
        # When context was the decider between visually near-identical candidates (e.g. 間に vs 闇に),
        # capture the runner-up options so they are added to the dictionary match list.
        alt_texts: List[str] = []
        if primary_text and len(primary_text) <= 2:
            if self._manga_ocr is not None:
                try:
                    m_model = self._manga_ocr.model
                    m_proc = self._manga_ocr.processor
                    m_tok = self._manga_ocr.tokenizer
                    pix = m_proc(crop_padded_rgb, return_tensors="pt").pixel_values.to(m_model.device)
                    with torch.no_grad():
                        m_out = m_model.generate(
                            pix,
                            max_length=6,
                            num_beams=4,
                            num_return_sequences=4,
                            return_dict_in_generate=True,
                            output_scores=True,
                        )
                    if hasattr(m_out, "sequences_scores") and len(m_out.sequences_scores) > 1:
                        top_score = m_out.sequences_scores[0].item()
                        for seq, s in zip(m_out.sequences[1:], m_out.sequences_scores[1:]):
                            score_diff = top_score - s.item()
                            if score_diff <= 0.18:
                                cand_txt = m_tok.decode(seq, skip_special_tokens=True)
                                cand_clean, _, _, _ = _clean_pair(cand_txt)
                                if (
                                    cand_clean
                                    and cand_clean != primary_text
                                    and len(cand_clean) <= len(primary_text)
                                    and cand_clean not in alt_texts
                                ):
                                    alt_texts.append(cand_clean)
                except Exception as b_exc:
                    logger.debug("MangaOCR beam tying search failed: %s", b_exc)

            if self._ndl_sess is not None and self._ndl_charlist and "out" in locals():
                try:
                    for p in range(min(end_pos, len(primary_text))):
                        top1_val = float(out[p, indices[p]])
                        sort_p = np.argsort(out[p])[::-1]
                        for r_idx in sort_p[1:3]:
                            r_val = float(out[p, r_idx])
                            if (top1_val - r_val) <= 5.0 and 0 < r_idx <= len(self._ndl_charlist):
                                r_ch = self._ndl_charlist[r_idx - 1]
                                if r_ch and r_ch != primary_text[p] and ("\u4e00" <= r_ch <= "\u9fff"):
                                    alt_p = primary_text[:p] + r_ch + primary_text[p + 1 :]
                                    if alt_p != primary_text and alt_p not in alt_texts:
                                        alt_texts.append(alt_p)
                except Exception as n_exc:
                    logger.debug("NDLOCR logit tying check failed: %s", n_exc)

        return primary_text, has_lead, has_trail, (primary_raw or primary_text), engine_texts, engine_raw_texts, alt_texts

    def _detect_short_phrase_tying_options(
        self,
        screen_img: Image.Image,
        bbox: Tuple[int, int, int, int],
        primary_text: str,
    ) -> List[str]:
        """
        For a short phrase (<= 2 characters), check if OCR models had close tying alternatives
        where language context tipped the balance over visual features (e.g. '間に' vs '闇に').
        """
        if not primary_text or len(primary_text) > 2:
            return []
        _, _, _, _, _, _, alt_texts = self._recognize_line_crop_all_engines(screen_img, bbox)
        return alt_texts

    def _compute_line_char_spans(
        self, screen_img: Image.Image, ln: DetectedLine
    ) -> List[CharSpan]:
        """
        Compute accurate per-character 3DS screen bounding boxes (`CharSpan`) for `ln.text`:
          1. Trims DBNet box padding to tight foreground glyph ink bounds.
          2. Drops leading/trailing stripped punctuation ink slivers (e.g. '！', '？', '。')
             when `ln.has_leading_punct` / `ln.has_trailing_punct` indicate stripped punctuation.
          3. Splits horizontal ink spans across full-width inter-word spaces (e.g. 'いよいよ　ちかづいてきたね！')
             and distributes `len(ln.text)` characters proportionally across each word segment so
             inter-word spaces never shift character coordinates.
        """
        lx1, ly1, lx2, ly2 = ln.bbox
        num_chars = len(ln.text)
        if num_chars == 0:
            return []

        if ln.is_vertical:
            char_h = (ly2 - ly1) / float(num_chars)
            return [
                CharSpan(ch, (float(lx1), ly1 + idx * char_h, float(lx2), ly1 + (idx + 1) * char_h), alt_texts=list(ln.alt_texts))
                for idx, ch in enumerate(ln.text)
            ]

        w, h = screen_img.size
        bx1, by1, bx2, by2 = max(0, lx1), max(0, ly1), min(w, lx2), min(h, ly2)
        if bx2 - bx1 >= 6 and by2 - by1 >= 6:
            crop = np.array(screen_img.crop((bx1, by1, bx2, by2)), dtype=np.float32)
            ink = self._compute_crop_ink_mask(crop)
            if ink is not None:
                vband = self._find_main_vertical_band(ink)
                if vband is not None:
                    y1_rel, y2_rel = vband
                    line_h = max(8, y2_rel - y1_rel)
                    band_ink = ink[y1_rel:y2_rel, :].copy()
                    col_ink = band_ink.any(axis=0)
                    xs = np.where(col_ink)[0]
                    if xs.size > 0:
                        raw_runs: List[List[int]] = []
                        r_start = int(xs[0])
                        r_prev = int(xs[0])
                        for x_val in xs[1:]:
                            xv = int(x_val)
                            if xv > r_prev + 1:
                                raw_runs.append([r_start, r_prev + 1])
                                r_start = xv
                            r_prev = xv
                        raw_runs.append([r_start, r_prev + 1])

                        punct_chars = r"\.…‥・。、．，！？!?\-―～~「」『』（）()\[\]【】［］｛｝｜|¦＜＞<>,;"
                        # Full-width punctuation ('？', '！', '…', '―', '「', '」', etc.) can be up to ~1.0x line_h wide
                        punct_max_w = max(10, int(line_h * 1.05))

                        # Determine how many trailing/leading punctuation glyphs were stripped from raw_text
                        trail_match = re.search(f"[{punct_chars}]+$", ln.raw_text or "")
                        max_trail_to_pop = len(trail_match.group(0)) if trail_match else (1 if ln.has_trailing_punct else 0)

                        lead_match = re.match(f"^[{punct_chars}]+", ln.raw_text or "")
                        max_lead_to_pop = len(lead_match.group(0)) if lead_match else (1 if ln.has_leading_punct else 0)

                        trail_popped = 0
                        if max_trail_to_pop > 0:
                            while len(raw_runs) > num_chars and trail_popped < max_trail_to_pop:
                                last_w = raw_runs[-1][1] - raw_runs[-1][0]
                                gap = raw_runs[-1][0] - raw_runs[-2][1]
                                if last_w <= punct_max_w and gap >= 1:
                                    raw_runs.pop()
                                    trail_popped += 1
                                else:
                                    break
                        elif len(raw_runs) > num_chars and len(raw_runs) > 1:
                            # Fallback: drop ultra-narrow isolated trailing sliver (e.g. '！')
                            sliver_max_w = max(4, int(line_h * 0.35))
                            sliver_min_gap = max(4, int(line_h * 0.35))
                            last_w = raw_runs[-1][1] - raw_runs[-1][0]
                            gap = raw_runs[-1][0] - raw_runs[-2][1]
                            if last_w <= sliver_max_w and gap >= sliver_min_gap:
                                raw_runs.pop()

                        lead_popped = 0
                        if max_lead_to_pop > 0:
                            while len(raw_runs) > num_chars and lead_popped < max_lead_to_pop:
                                first_w = raw_runs[0][1] - raw_runs[0][0]
                                gap = raw_runs[1][0] - raw_runs[0][1]
                                if first_w <= punct_max_w and gap >= 1:
                                    raw_runs.pop(0)
                                    lead_popped += 1
                                else:
                                    break

                        total_span = max(1, raw_runs[-1][1] - raw_runs[0][0])
                        avg_pitch = total_span / float(num_chars)
                        space_gap_thresh = max(11, int(line_h * 0.78), int(avg_pitch * 0.72))

                        merged_segments: List[List[int]] = [raw_runs[0][:]]
                        for run in raw_runs[1:]:
                            gap = run[0] - merged_segments[-1][1]
                            if gap >= space_gap_thresh:
                                merged_segments.append(run[:])
                            else:
                                merged_segments[-1][1] = run[1]

                        if len(merged_segments) > num_chars:
                            merged_segments = [[raw_runs[0][0], raw_runs[-1][1]]]

                        seg_widths = [max(1.0, float(seg[1] - seg[0] + 2)) for seg in merged_segments]
                        total_seg_w = sum(seg_widths)

                        # Allocate num_chars across merged_segments proportionally to width
                        exact_counts = [num_chars * (sw / total_seg_w) for sw in seg_widths]
                        counts = [max(1, int(math.floor(c))) for c in exact_counts]
                        while sum(counts) > num_chars:
                            idx_max = max(range(len(counts)), key=lambda k: counts[k])
                            if counts[idx_max] > 1:
                                counts[idx_max] -= 1
                            else:
                                break
                        while sum(counts) < num_chars:
                            remainders = [
                                (exact_counts[k] - counts[k], k) for k in range(len(counts))
                            ]
                            _, best_k = max(remainders)
                            counts[best_k] += 1

                        sy1 = float(max(0, by1 + y1_rel - 2))
                        sy2 = float(min(h, by1 + y2_rel + 2))
                        out_spans: List[CharSpan] = []
                        char_cursor = 0
                        for seg, seg_n in zip(merged_segments, counts):
                            sx1 = float(max(0, bx1 + seg[0] - 1))
                            sx2 = float(min(w, bx1 + seg[1] + 1))
                            seg_char_w = (sx2 - sx1) / float(seg_n)
                            for k in range(seg_n):
                                ch = ln.text[char_cursor]
                                cx1 = sx1 + k * seg_char_w
                                cx2 = cx1 + seg_char_w
                                out_spans.append(CharSpan(ch, (cx1, sy1, cx2, sy2), alt_texts=list(ln.alt_texts)))
                                char_cursor += 1
                        return out_spans

        char_w = (lx2 - lx1) / float(num_chars)
        return [
            CharSpan(ch, (lx1 + idx * char_w, float(ly1), lx1 + (idx + 1) * char_w, float(ly2)), alt_texts=list(ln.alt_texts))
            for idx, ch in enumerate(ln.text)
        ]

    def _split_horizontal_box_on_spaces(
        self, screen_img: Image.Image, bbox: Tuple[int, int, int, int]
    ) -> List[Tuple[int, int, int, int]]:
        """
        Split a wide horizontal DBNet text box across full-width inter-word spaces (e.g.
        '縮めて　ポケモンが　たくさんいる！' -> ['縮めて', 'ポケモンが', 'たくさんいる！']) BEFORE
        passing to MangaOCR. Because MangaOCR's ViTImageProcessor resizes every input to a
        fixed 224x224 square, feeding a 280px-wide multi-chunk line crushes horizontal stroke
        detail (causing dense kanji like '縮' to be misread as '締') and hallucinates characters
        inside the wide spaces.
        """
        x1, y1, x2, y2 = bbox
        w, h = screen_img.size
        bx1, by1, bx2, by2 = max(0, x1), max(0, y1), min(w, x2), min(h, y2)
        box_w = bx2 - bx1
        box_h = by2 - by1
        if box_h <= 0 or box_w < 48 or box_h > box_w * 1.5:
            return [bbox]

        crop = np.array(screen_img.crop((bx1, by1, bx2, by2)), dtype=np.float32)
        if crop.shape[0] < 6 or crop.shape[1] < 6:
            return [bbox]

        ch_c, cw_c = crop.shape[:2]
        ink = self._compute_crop_ink_mask(crop)
        if ink is None:
            return [bbox]

        vband = self._find_main_vertical_band(ink)
        if vband is None:
            return [bbox]

        y1_rel, y2_rel = vband
        line_h = max(8, y2_rel - y1_rel)
        band_ink = ink[y1_rel:y2_rel, :].copy()
        if cw_c >= ch_c * 3.5:
            band_ink[:, band_ink.mean(axis=0) > 0.88] = False
        col_ink = band_ink.any(axis=0)
        xs = np.where(col_ink)[0]
        if xs.size == 0:
            return [bbox]

        raw_runs: List[List[int]] = []
        r_start = int(xs[0])
        r_prev = int(xs[0])
        for x_val in xs[1:]:
            xv = int(x_val)
            if xv > r_prev + 1:
                raw_runs.append([r_start, r_prev + 1])
                r_start = xv
            r_prev = xv
        raw_runs.append([r_start, r_prev + 1])

        space_gap_thresh = max(13, int(line_h * 0.85))
        merged_segments: List[List[int]] = [raw_runs[0][:]]
        for run in raw_runs[1:]:
            gap = run[0] - merged_segments[-1][1]
            if gap >= space_gap_thresh:
                merged_segments.append(run[:])
            else:
                merged_segments[-1][1] = run[1]

        valid_segments = [seg for seg in merged_segments if (seg[1] - seg[0]) >= 4]
        if not valid_segments:
            return [bbox]

        sy1 = max(0, by1 + y1_rel - 2)
        sy2 = min(h, by1 + y2_rel + 2)
        sub_boxes: List[Tuple[int, int, int, int]] = []
        for seg in valid_segments:
            sx1 = max(0, bx1 + seg[0] - 2)
            sx2 = min(w, bx1 + seg[1] + 2)
            if sx2 - sx1 >= 6 and sy2 - sy1 >= 6:
                sub_boxes.append((sx1, sy1, sx2, sy2))

        return sub_boxes if sub_boxes else [bbox]

    @staticmethod
    def _get_configured_ocr_mode() -> str:
        cfg = _load_anki_config()
        mode = str(cfg.get("ocr_mode", "local_cached") or "local_cached").strip().lower()
        if mode in ("local_cached", "hybrid_cached", "gemini_cached"):
            return mode
        return "local_cached"

    def _get_gemini_candidate_models(self) -> List[str]:
        cfg = _load_anki_config()
        primary = str(cfg.get("gemini_model", "gemini-3.5-flash-lite") or "gemini-3.5-flash-lite").strip()
        fallback = str(cfg.get("gemini_fallback_model", "gemini-3.1-flash-lite") or "").strip()
        candidates: List[str] = []
        for m in (primary, fallback):
            if m and m not in candidates:
                candidates.append(m)
        return candidates

    def _post_gemini_json(
        self, path: str, payload: bytes, api_key: str, timeout_sec: float
    ) -> dict:
        """POST JSON to Gemini REST API reusing a thread-local HTTPS keep-alive socket."""
        tls_store = getattr(self, "_gemini_http_local", None)
        if tls_store is None:
            tls_store = threading.local()
            self._gemini_http_local = tls_store
        headers = {
            "Content-Type": "application/json",
            "x-goog-api-key": api_key,
            "Connection": "keep-alive",
        }
        for attempt in range(2):
            conn = getattr(tls_store, "conn", None)
            if conn is None:
                conn = http.client.HTTPSConnection("generativelanguage.googleapis.com", timeout=timeout_sec)
                tls_store.conn = conn
            else:
                conn.timeout = timeout_sec
                if conn.sock is not None:
                    conn.sock.settimeout(timeout_sec)
            try:
                conn.request("POST", path, body=payload, headers=headers)
                resp = conn.getresponse()
                body = resp.read()
                if resp.status >= 400:
                    err_txt = body.decode("utf-8", errors="replace")[:200]
                    raise urllib.error.HTTPError(
                        f"https://generativelanguage.googleapis.com{path}",
                        resp.status,
                        f"{resp.reason}: {err_txt}",
                        resp.headers,
                        None,
                    )
                return json.loads(body.decode("utf-8"))
            except (http.client.RemoteDisconnected, ConnectionResetError, BrokenPipeError):
                try:
                    conn.close()
                except Exception:
                    pass
                tls_store.conn = None
                if attempt == 0:
                    continue
                raise
            except Exception:
                try:
                    conn.close()
                except Exception:
                    pass
                tls_store.conn = None
                raise
        return {}

    def _call_gemini_generate_content(
        self,
        parts: List[Dict[str, Any]],
        generation_config: Dict[str, Any],
        api_key: Union[str, List[str]],
        candidates: List[str],
        timeout_sec: float,
        op_label: str,
        max_wall_sec: float = 18.0,
        max_timeouts: int = 2,
        cancel_event: Optional[threading.Event] = None,
        meta_out: Optional[Dict[str, int]] = None,
    ) -> Optional[str]:
        """Call Gemini/Gemma REST generateContent across candidate models and API keys with cooldown on 429/503/timeouts."""
        stats = {
            "attempts": 0,
            "auth_errors": 0,
            "rate_limited": 0,
            "service_unavailable": 0,
            "timeouts": 0,
        }
        try:
            keys = [k for k in ([api_key] if isinstance(api_key, str) else list(api_key)) if k]
            if not keys or not candidates:
                return None

            pairs = [(k_idx, k, m) for m in candidates for k_idx, k in enumerate(keys)]
            now = time.monotonic()
            active = [(ki, k, m) for (ki, k, m) in pairs if self._gemini_cooldown_until.get((ki, m), 0.0) <= now]
            ordered = active if active else pairs

            call_start = time.monotonic()
            consecutive_timeouts = 0
            saw_503 = False

            def _try_one(key_idx: int, key_val: str, model_name: str) -> Tuple[Optional[str], bool, bool]:
                stats["attempts"] += 1
                gen_cfg = dict(generation_config)
                if model_name.startswith(("gemini-3", "gemma-4")):
                    gen_cfg.setdefault("thinkingConfig", {"thinkingLevel": "minimal"})
                elif "2.5" in model_name:
                    gen_cfg.setdefault("thinkingConfig", {"thinkingBudget": 0})
                if model_name.startswith("gemma"):
                    gen_cfg.pop("responseMimeType", None)

                payload = json.dumps({
                    "contents": [{"parts": parts}],
                    "generationConfig": gen_cfg,
                }).encode("utf-8")
                path = f"/v1beta/models/{urllib.parse.quote(model_name)}:generateContent"
                try:
                    resp_obj = self._post_gemini_json(path, payload, key_val, timeout_sec)
                    cands = resp_obj.get("candidates") or []
                    if not cands:
                        return None, False, False
                    res_parts = (cands[0].get("content") or {}).get("parts") or []
                    out_txt = "".join(
                        str(p.get("text", ""))
                        for p in res_parts
                        if isinstance(p, dict) and not p.get("thought")
                    ).strip()
                    if out_txt:
                        self._gemini_cooldown_until.pop((key_idx, model_name), None)
                        return out_txt, False, False
                    return None, False, False
                except Exception as exc:
                    http_code = getattr(exc, "code", None)
                    is_timeout = False
                    is_503 = (http_code == 503)
                    if http_code in (401, 403, 429):
                        cooldown_sec = 300.0
                    elif is_503:
                        cooldown_sec = 10.0
                    elif http_code is not None and http_code >= 500:
                        cooldown_sec = 60.0
                    else:
                        cooldown_sec = 90.0
                        is_timeout = True
                    if http_code in (401, 403):
                        stats["auth_errors"] += 1
                    elif http_code == 429:
                        stats["rate_limited"] += 1
                    elif is_503:
                        stats["service_unavailable"] += 1
                    elif is_timeout:
                        stats["timeouts"] += 1
                    self._gemini_cooldown_until[(key_idx, model_name)] = time.monotonic() + cooldown_sec
                    logger.warning(
                        "Gemini %s failed with key#%d model=%s (%s); cooling down %.0fs.",
                        op_label,
                        key_idx + 1,
                        model_name,
                        exc,
                        cooldown_sec,
                    )
                    return None, is_timeout, is_503

            for idx, (key_idx, key_val, model_name) in enumerate(ordered):
                if cancel_event is not None and cancel_event.is_set():
                    return None
                if idx > 0 and (consecutive_timeouts >= max_timeouts or (time.monotonic() - call_start) >= max_wall_sec):
                    break
                out_txt, is_to, is_503 = _try_one(key_idx, key_val, model_name)
                if out_txt:
                    return out_txt
                if is_to:
                    consecutive_timeouts += 1
                elif is_503:
                    saw_503 = True

            if (
                consecutive_timeouts == 0
                and saw_503
                and ordered
                and (time.monotonic() - call_start) < max_wall_sec - 4.0
                and not (cancel_event is not None and cancel_event.is_set())
            ):
                if cancel_event is not None:
                    if cancel_event.wait(0.4):
                        return None
                else:
                    time.sleep(0.4)
                ki, kv, mn = ordered[0]
                out_txt, _, _ = _try_one(ki, kv, mn)
                if out_txt:
                    return out_txt
            return None
        finally:
            if meta_out is not None:
                meta_out.update(stats)

    @staticmethod
    def _parse_batch_ocr_json_response(raw_json: str) -> Dict[str, str]:
        """Parse batch OCR JSON responses supporting dicts, markdown fences, prose wrappers, and lists of dicts."""
        txt = re.sub(r"^```(?:json)?\s*|\s*```$", "", (raw_json or "").strip(), flags=re.IGNORECASE).strip()
        if not txt:
            return {}
        parsed = None
        try:
            parsed = json.loads(txt)
        except Exception:
            for open_ch, close_ch in (("{", "}"), ("[", "]")):
                i0 = txt.find(open_ch)
                i1 = txt.rfind(close_ch)
                if i0 != -1 and i1 > i0:
                    try:
                        parsed = json.loads(txt[i0 : i1 + 1])
                        break
                    except Exception:
                        pass
        if isinstance(parsed, dict):
            return {str(k): v for k, v in parsed.items() if isinstance(v, str)}
        if isinstance(parsed, list):
            merged: Dict[str, str] = {}
            for item in parsed:
                if isinstance(item, dict):
                    for k, v in item.items():
                        if isinstance(v, str):
                            merged[str(k)] = v
            return merged
        return {}

    def _batch_ocr_crops_with_gemini(
        self,
        crop_specs: List[Tuple[Image.Image, Tuple[int, int, int, int]]],
        cancel_event: Optional[threading.Event] = None,
        persistent_retry: bool = False,
        max_retry_wall_sec: float = 180.0,
        retry_backoff_sec: Tuple[float, ...] = (2.0, 4.0, 6.0, 8.0, 10.0, 15.0),
    ) -> Dict[int, str]:
        """
        Batch-transcribe detected text crop boxes (possibly spanning multiple screens)
        in a single Gemini call using a native 1x stacked PNG strip sheet.
        Returns `{crop_index: raw_japanese_text}` for recognized entries.
        """
        if not crop_specs or not _load_gemini_api_keys():
            return {}

        rgb_cache: Dict[int, Image.Image] = {}
        pad_x = 40
        row_gap = 6
        chunk_size = 24
        prepared_chunks: List[Tuple[List[str], List[Dict[str, Any]]]] = []

        for start_idx in range(0, len(crop_specs), chunk_size):
            chunk = crop_specs[start_idx : start_idx + chunk_size]
            crops: List[Image.Image] = []
            local_ids: List[str] = []
            for offset, (src_img, (bx1, by1, bx2, by2)) in enumerate(chunk):
                img_key = id(src_img)
                rgb_src = rgb_cache.get(img_key)
                if rgb_src is None:
                    rgb_src = src_img.convert("RGB")
                    rgb_cache[img_key] = rgb_src
                w, h = rgb_src.size
                cx1 = max(0, min(w, bx1 - 1))
                cy1 = max(0, min(h, by1 - 1))
                cx2 = max(0, min(w, bx2 + 1))
                cy2 = max(0, min(h, by2 + 1))
                if cx2 <= cx1 or cy2 <= cy1:
                    continue
                crops.append(rgb_src.crop((cx1, cy1, cx2, cy2)))
                local_ids.append(str(start_idx + offset))

            if not crops:
                continue

            row_heights = [max(c.height, 18) for c in crops]
            canvas_w = pad_x + max(c.width for c in crops) + 10
            canvas_h = sum(rh + row_gap for rh in row_heights) + row_gap
            sheet = Image.new("RGB", (canvas_w, canvas_h), (24, 24, 24))
            draw = ImageDraw.Draw(sheet)
            y_cur = row_gap
            for lid, c, rh in zip(local_ids, crops, row_heights):
                draw.rectangle((4, y_cur, 34, y_cur + 16), fill=(0, 120, 215))
                draw.text((7, y_cur + 2), lid, fill=(255, 255, 255))
                sheet.paste(c, (pad_x, y_cur + (rh - c.height) // 2))
                y_cur += rh + row_gap

            buf = io.BytesIO()
            sheet.save(buf, format="PNG")
            b64 = base64.b64encode(buf.getvalue()).decode("ascii")

            prompt = (
                "Transcribe the exact Japanese text shown in each numbered row image. "
                "Pay careful attention to dakuten/handakuten (゛/゜, e.g. が vs か, ば vs は, ぱ vs は) and small kana (っ, ゃ, ゅ, ょ). "
                "Preserve punctuation exactly as shown. Do not translate. "
                "If a row contains only non-text graphics or no readable text, return an empty string.\n"
                "Return ONLY valid JSON mapping each row ID string to its exact text.\n"
                f"Row IDs: {local_ids}"
            )
            parts = [
                {"text": prompt},
                {"inline_data": {"mime_type": "image/png", "data": b64}},
            ]
            prepared_chunks.append((local_ids, parts))

        if not prepared_chunks:
            return {}

        results: Dict[int, str] = {}
        pending_chunks = list(range(len(prepared_chunks)))
        batch_start = time.monotonic()
        round_num = 0

        while pending_chunks:
            if cancel_event is not None and cancel_event.is_set():
                break

            round_num += 1
            api_keys = _load_gemini_api_keys()
            candidates = self._get_gemini_candidate_models()
            if not api_keys or not candidates:
                break

            if not persistent_retry:
                r_timeout = 12.0
                r_wall = 16.0
                r_max_to = 2
            elif round_num == 1:
                r_timeout = 15.0
                r_wall = 24.0
                r_max_to = 2
            else:
                r_timeout = 25.0
                r_wall = 40.0
                r_max_to = max(2, len(api_keys) * len(candidates))

            next_pending: List[int] = []
            round_attempts = 0
            round_auth_errors = 0
            round_rate_limited = 0

            for chunk_idx in pending_chunks:
                if cancel_event is not None and cancel_event.is_set():
                    next_pending.append(chunk_idx)
                    continue
                local_ids, parts = prepared_chunks[chunk_idx]
                call_meta: Dict[str, int] = {}
                raw_json = self._call_gemini_generate_content(
                    parts=parts,
                    generation_config={
                        "temperature": 0.0,
                        "maxOutputTokens": 1024,
                        "responseMimeType": "application/json",
                    },
                    api_key=api_keys,
                    candidates=candidates,
                    timeout_sec=r_timeout,
                    op_label="batch OCR",
                    max_wall_sec=r_wall,
                    max_timeouts=r_max_to,
                    cancel_event=cancel_event,
                    meta_out=call_meta,
                )
                round_attempts += call_meta.get("attempts", 0)
                round_auth_errors += call_meta.get("auth_errors", 0)
                round_rate_limited += call_meta.get("rate_limited", 0)

                parsed_ok = False
                if raw_json:
                    try:
                        parsed = self._parse_batch_ocr_json_response(raw_json)
                        if parsed:
                            matched_any = False
                            for lid in local_ids:
                                if lid in parsed and isinstance(parsed[lid], str):
                                    results[int(lid)] = parsed[lid]
                                    matched_any = True
                            if matched_any:
                                parsed_ok = True
                    except Exception as exc:
                        logger.warning("Gemini batch OCR JSON parse failed (%s).", exc)
                if not parsed_ok:
                    next_pending.append(chunk_idx)

            pending_chunks = next_pending
            if (
                not pending_chunks
                or not persistent_retry
                or (cancel_event is not None and cancel_event.is_set())
            ):
                break

            if round_attempts > 0 and round_auth_errors == round_attempts:
                logger.warning(
                    "Hybrid AI OCR stopped retrying: all API keys failed authentication (401/403)."
                )
                break

            if round_rate_limited > 0 and round_rate_limited == round_attempts:
                backoff = min(30.0, 10.0 * round_num)
            elif retry_backoff_sec:
                backoff = float(
                    retry_backoff_sec[min(round_num - 1, len(retry_backoff_sec) - 1)]
                )
            else:
                backoff = 2.0

            if (time.monotonic() - batch_start) + backoff >= max_retry_wall_sec:
                break

            logger.info(
                "Hybrid AI OCR round %d incomplete (%d/%d chunks pending); retrying in %.0fs while Local OCR serves this turn...",
                round_num,
                len(pending_chunks),
                len(prepared_chunks),
                backoff,
            )
            if cancel_event is not None:
                if cancel_event.wait(backoff):
                    break
            else:
                time.sleep(backoff)

        return results

    def _batch_ocr_with_gemini(
        self,
        screen_img: Image.Image,
        sub_boxes: List[Tuple[int, int, int, int]],
    ) -> Dict[int, str]:
        """
        Batch-transcribe detected text crop boxes in a single Gemini call using a native 1x
        stacked PNG strip sheet. Returns `{box_index: raw_japanese_text}` for recognized entries.
        """
        if not sub_boxes:
            return {}
        return self._batch_ocr_crops_with_gemini([(screen_img, box) for box in sub_boxes])

    def _resolve_ocr_mode(self, ocr_mode: Optional[str] = None) -> str:
        """Resolve `"local_cached"`, `"hybrid_cached"`, or `"gemini_cached"` from an override or config."""
        mode = (
            str(ocr_mode).strip().lower()
            if ocr_mode is not None
            else self._get_configured_ocr_mode()
        )
        if mode in ("local_cached", "hybrid_cached", "gemini_cached"):
            return mode
        return "local_cached"

    def _store_screen_lines_cache_locked(
        self, cache_key: Tuple[str, str], lines: List[DetectedLine]
    ) -> None:
        self._screen_lines_cache[cache_key] = list(lines)
        while len(self._screen_lines_cache) > 16:
            oldest_key = next(iter(self._screen_lines_cache))
            self._screen_lines_cache.pop(oldest_key, None)

    def _store_screen_lines_cache(
        self, cache_key: Tuple[str, str], lines: List[DetectedLine]
    ) -> None:
        with self._screen_cache_lock:
            self._store_screen_lines_cache_locked(cache_key, lines)

    def has_cached_screen_lines(self, screen_img: Image.Image, mode: str) -> bool:
        """Return True if screen lines for `(img_digest, mode)` are already in cache."""
        concrete_mode = self._resolve_ocr_mode(mode)
        img_digest = hashlib.blake2b(screen_img.tobytes(), digest_size=16).hexdigest()
        cache_key = (img_digest, concrete_mode)
        with self._screen_cache_lock:
            return cache_key in self._screen_lines_cache

    def detect_screen_lines_with_source(
        self, screen_img: Image.Image, ocr_mode: Optional[str] = None
    ) -> Tuple[List[DetectedLine], str]:
        """
        Detect and recognize all text lines across the entire 3DS screen, returning
        `(lines, concrete_mode)`. In `"hybrid_cached"` mode, returns `"gemini_cached"`
        immediately if ready, else `"local_cached"` (waiting only while both are still pending).
        """
        mode = self._resolve_ocr_mode(ocr_mode)
        if mode != "hybrid_cached":
            return self.detect_screen_lines(screen_img, ocr_mode=mode), mode

        img_digest = hashlib.blake2b(screen_img.tobytes(), digest_size=16).hexdigest()
        deadline = time.monotonic() + 30.0
        while True:
            with self._screen_cache_lock:
                g_key = (img_digest, "gemini_cached")
                if g_key in self._screen_lines_cache:
                    return list(self._screen_lines_cache[g_key]), "gemini_cached"
                l_key = (img_digest, "local_cached")
                if l_key in self._screen_lines_cache:
                    return list(self._screen_lines_cache[l_key]), "local_cached"
                any_evt = self._hybrid_any_ready.get(img_digest)
            if any_evt is None:
                break
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                break
            any_evt.wait(timeout=remaining)

        return self.detect_screen_lines(screen_img, ocr_mode="local_cached"), "local_cached"

    def detect_screen_lines(
        self, screen_img: Image.Image, ocr_mode: Optional[str] = None
    ) -> List[DetectedLine]:
        """
        Detect and recognize all text lines across the entire 3DS screen, with per-screen
        caching and in-flight deduplication across `_bg_precache`, Dictionary Lookup, and
        Selection Translate. Supports `"local_cached"`, `"hybrid_cached"`, and `"gemini_cached"` modes.
        """
        mode = self._resolve_ocr_mode(ocr_mode)
        if mode == "hybrid_cached":
            lines, _ = self.detect_screen_lines_with_source(screen_img, ocr_mode="hybrid_cached")
            return lines

        img_digest = hashlib.blake2b(screen_img.tobytes(), digest_size=16).hexdigest()
        cache_key = (img_digest, mode)

        while True:
            with self._screen_cache_lock:
                if cache_key in self._screen_lines_cache:
                    return list(self._screen_lines_cache[cache_key])
                inflight_evt = self._screen_cache_inflight.get(cache_key)
                if inflight_evt is None:
                    inflight_evt = threading.Event()
                    self._screen_cache_inflight[cache_key] = inflight_evt
                    is_owner = True
                    break
                is_owner = False
            if not is_owner:
                inflight_evt.wait(timeout=30.0)

        t0 = time.monotonic()
        try:
            with self._ocr_exec_lock:
                result_lines = self._detect_screen_lines_uncached(screen_img, mode)
            self._store_screen_lines_cache(cache_key, result_lines)
            logger.info(
                "Screen OCR completed: %d lines (mode=%s, %.2fs).",
                len(result_lines),
                mode,
                time.monotonic() - t0,
            )
            return list(result_lines)
        finally:
            with self._screen_cache_lock:
                self._screen_cache_inflight.pop(cache_key, None)
            inflight_evt.set()

    def precache_screens_batch(
        self, screen_imgs: List[Image.Image], ocr_mode: Optional[str] = None
    ) -> List[List[DetectedLine]]:
        """
        Detect and recognize text lines across multiple screens (e.g., Top + Bottom) together,
        sending uncached DBNet crops in a single Gemini batch OCR call when in `gemini_cached`
        mode while populating per-screen cache entries and honoring in-flight deduplication.
        """
        if not screen_imgs:
            return []

        mode = self._resolve_ocr_mode(ocr_mode)
        keys = [
            (hashlib.blake2b(img.tobytes(), digest_size=16).hexdigest(), mode)
            for img in screen_imgs
        ]

        results_by_key: Dict[Tuple[str, str], List[DetectedLine]] = {}
        owned_events: Dict[Tuple[str, str], threading.Event] = {}
        owned_specs: List[Tuple[Tuple[str, str], Image.Image]] = []

        with self._screen_cache_lock:
            for img, cache_key in zip(screen_imgs, keys):
                if cache_key in results_by_key or cache_key in owned_events:
                    continue
                if cache_key in self._screen_lines_cache:
                    results_by_key[cache_key] = list(self._screen_lines_cache[cache_key])
                    continue
                if cache_key in self._screen_cache_inflight:
                    continue
                evt = threading.Event()
                self._screen_cache_inflight[cache_key] = evt
                owned_events[cache_key] = evt
                owned_specs.append((cache_key, img))

        if owned_specs:
            t0 = time.monotonic()
            try:
                per_screen_sub_boxes: Dict[
                    Tuple[str, str], Optional[List[Tuple[int, int, int, int]]]
                ] = {}
                crop_specs: List[Tuple[Image.Image, Tuple[int, int, int, int]]] = []
                slice_ranges: Dict[Tuple[str, str], Tuple[int, int]] = {}
                computed_results: Dict[Tuple[str, str], List[DetectedLine]] = {}

                with self._ocr_exec_lock:
                    for cache_key, img in owned_specs:
                        sub_boxes = self._detect_screen_sub_boxes(img)
                        per_screen_sub_boxes[cache_key] = sub_boxes
                        if mode == "gemini_cached" and sub_boxes:
                            start_idx = len(crop_specs)
                            crop_specs.extend((img, box) for box in sub_boxes)
                            slice_ranges[cache_key] = (start_idx, len(crop_specs))

                    combined_gemini_map: Dict[int, str] = {}
                    if mode == "gemini_cached" and crop_specs:
                        combined_gemini_map = self._batch_ocr_crops_with_gemini(crop_specs)

                    for cache_key, img in owned_specs:
                        sub_boxes = per_screen_sub_boxes[cache_key]
                        if sub_boxes is None:
                            lines = self._detect_screen_lines_pytesseract_fallback(img)
                        elif not sub_boxes:
                            lines = []
                        else:
                            local_gemini_map: Dict[int, str] = {}
                            if cache_key in slice_ranges and combined_gemini_map:
                                s_idx, e_idx = slice_ranges[cache_key]
                                for g_idx in range(s_idx, e_idx):
                                    if g_idx in combined_gemini_map:
                                        local_gemini_map[g_idx - s_idx] = combined_gemini_map[g_idx]
                            lines = self._finalize_screen_lines(img, sub_boxes, local_gemini_map)
                        computed_results[cache_key] = lines

                with self._screen_cache_lock:
                    for cache_key, lines in computed_results.items():
                        self._store_screen_lines_cache_locked(cache_key, lines)
                        results_by_key[cache_key] = list(lines)

                total_crops = sum(len(b) if b else 0 for b in per_screen_sub_boxes.values())
                if len(owned_specs) > 1:
                    counts_str = ", ".join(str(len(computed_results[k])) for k, _ in owned_specs)
                    logger.info(
                        "Combined %d-screen OCR completed: [%s] lines (%d total crops, mode=%s, %.2fs).",
                        len(owned_specs),
                        counts_str,
                        total_crops,
                        mode,
                        time.monotonic() - t0,
                    )
                else:
                    only_key = owned_specs[0][0]
                    logger.info(
                        "Screen OCR completed: %d lines (mode=%s, %.2fs).",
                        len(computed_results[only_key]),
                        mode,
                        time.monotonic() - t0,
                    )
            finally:
                with self._screen_cache_lock:
                    for cache_key in owned_events:
                        self._screen_cache_inflight.pop(cache_key, None)
                for evt in owned_events.values():
                    evt.set()

        for img, cache_key in zip(screen_imgs, keys):
            if cache_key not in results_by_key:
                results_by_key[cache_key] = self.detect_screen_lines(img, ocr_mode=mode)

        return [list(results_by_key[k]) for k in keys]

    def start_hybrid_screens_batch(
        self,
        screen_imgs: List[Image.Image],
        cancel_event: Optional[threading.Event] = None,
        persistent_retry: bool = True,
        max_retry_wall_sec: float = 180.0,
        retry_backoff_sec: Tuple[float, ...] = (2.0, 4.0, 6.0, 8.0, 10.0, 15.0),
    ) -> HybridBatchHandle:
        """
        Run DBNet detection once across `screen_imgs`, then launch Local 4-Engine OCR and
        combined Gemini Batch OCR in parallel. Whichever completes first unblocks immediate
        queries; when Gemini succeeds, it upgrades the cache and cancels remaining local work.
        """
        handle = HybridBatchHandle()
        if not screen_imgs:
            handle.gemini_succeeded = True
            handle.gemini_ready_evt.set()
            handle.local_all_done_evt.set()
            handle.any_batch_ready_evt.set()
            return handle

        unique_specs: List[Tuple[str, Image.Image]] = []
        seen_digests = set()
        for img in screen_imgs:
            d = hashlib.blake2b(img.tobytes(), digest_size=16).hexdigest()
            if d not in seen_digests:
                seen_digests.add(d)
                unique_specs.append((d, img))

        uncached_gemini_specs: List[Tuple[str, Image.Image]] = []
        owned_any_events: Dict[str, threading.Event] = {}

        with self._screen_cache_lock:
            for d, img in unique_specs:
                if (d, "gemini_cached") not in self._screen_lines_cache:
                    uncached_gemini_specs.append((d, img))
                    if (d, "local_cached") not in self._screen_lines_cache and d not in self._hybrid_any_ready:
                        evt = threading.Event()
                        self._hybrid_any_ready[d] = evt
                        owned_any_events[d] = evt

        if not uncached_gemini_specs:
            handle.gemini_succeeded = True
            handle.gemini_ready_evt.set()
            handle.local_all_done_evt.set()
            handle.any_batch_ready_evt.set()
            return handle

        def _cleanup_owned_any_events() -> None:
            if not owned_any_events:
                return
            with self._screen_cache_lock:
                for d_key, evt_obj in owned_any_events.items():
                    if self._hybrid_any_ready.get(d_key) is evt_obj:
                        self._hybrid_any_ready.pop(d_key, None)
            for evt_obj in owned_any_events.values():
                evt_obj.set()

        try:
            with self._ocr_exec_lock:
                per_screen_sub_boxes: Dict[str, Optional[List[Tuple[int, int, int, int]]]] = {
                    d: self._detect_screen_sub_boxes(img) for d, img in uncached_gemini_specs
                }

            crop_specs: List[Tuple[Image.Image, Tuple[int, int, int, int]]] = []
            slice_ranges: Dict[str, Tuple[int, int]] = {}
            for d, img in uncached_gemini_specs:
                sub_boxes = per_screen_sub_boxes[d]
                if sub_boxes:
                    s_idx = len(crop_specs)
                    crop_specs.extend((img, box) for box in sub_boxes)
                    slice_ranges[d] = (s_idx, len(crop_specs))

            if not crop_specs and all(per_screen_sub_boxes[d] is not None for d, _ in uncached_gemini_specs):
                with self._screen_cache_lock:
                    for d, _ in uncached_gemini_specs:
                        self._store_screen_lines_cache_locked((d, "gemini_cached"), [])
                        self._store_screen_lines_cache_locked((d, "local_cached"), [])
                _cleanup_owned_any_events()
                handle.gemini_succeeded = True
                handle.gemini_ready_evt.set()
                handle.local_all_done_evt.set()
                handle.any_batch_ready_evt.set()
                return handle
        except Exception:
            _cleanup_owned_any_events()
            handle.gemini_ready_evt.set()
            handle.local_all_done_evt.set()
            handle.any_batch_ready_evt.set()
            raise

        abort_local_evt = threading.Event()
        worker_lock = threading.Lock()
        workers_remaining = [2]

        def _release_worker() -> None:
            is_last = False
            with worker_lock:
                workers_remaining[0] -= 1
                is_last = (workers_remaining[0] <= 0)
            if is_last:
                _cleanup_owned_any_events()

        def _run_gemini_worker() -> None:
            t_gem = time.monotonic()
            try:
                combined_gemini_map = (
                    self._batch_ocr_crops_with_gemini(
                        crop_specs,
                        cancel_event=cancel_event,
                        persistent_retry=persistent_retry,
                        max_retry_wall_sec=max_retry_wall_sec,
                        retry_backoff_sec=retry_backoff_sec,
                    )
                    if crop_specs
                    else {}
                )
                if combined_gemini_map and not (cancel_event is not None and cancel_event.is_set()):
                    abort_local_evt.set()
                    computed_gem: Dict[str, Optional[List[DetectedLine]]] = {}
                    with self._screen_cache_lock:
                        local_cache_snapshot = {
                            d: list(self._screen_lines_cache[(d, "local_cached")])
                            for d, _ in uncached_gemini_specs
                            if (d, "local_cached") in self._screen_lines_cache
                        }
                    with self._ocr_exec_lock:
                        for d, img in uncached_gemini_specs:
                            sub_boxes = per_screen_sub_boxes[d]
                            if sub_boxes is None:
                                computed_gem[d] = None
                            elif not sub_boxes:
                                computed_gem[d] = []
                            else:
                                local_g_map: Dict[int, str] = {}
                                if d in slice_ranges:
                                    s_idx, e_idx = slice_ranges[d]
                                    for g_idx in range(s_idx, e_idx):
                                        if g_idx in combined_gemini_map:
                                            local_g_map[g_idx - s_idx] = combined_gemini_map[g_idx]
                                gem_lines = self._finalize_screen_lines(img, sub_boxes, local_g_map)
                                if gem_lines and d in local_cache_snapshot:
                                    self._reconcile_gemini_with_local_lines(
                                        gem_lines, local_cache_snapshot[d]
                                    )
                                computed_gem[d] = gem_lines

                    with self._screen_cache_lock:
                        for d, lines in computed_gem.items():
                            if lines is not None:
                                self._store_screen_lines_cache_locked((d, "gemini_cached"), lines)

                    handle.gemini_succeeded = True
                    for evt_obj in owned_any_events.values():
                        evt_obj.set()
                    counts_str = ", ".join(
                        str(len(computed_gem[d])) for d, _ in uncached_gemini_specs if computed_gem.get(d) is not None
                    )
                    logger.info(
                        "Hybrid AI OCR completed: [%s] lines (%d total crops, %.2fs).",
                        counts_str,
                        len(crop_specs),
                        time.monotonic() - t_gem,
                    )
                else:
                    if not (cancel_event is not None and cancel_event.is_set()):
                        logger.info(
                            "Hybrid AI OCR unavailable/failed after %.2fs; keeping local OCR results.",
                            time.monotonic() - t_gem,
                        )
            except Exception as exc:
                logger.warning("Hybrid AI OCR worker error: %s", exc)
            finally:
                handle.gemini_ready_evt.set()
                handle.any_batch_ready_evt.set()
                _release_worker()

        def _run_local_worker() -> None:
            try:
                for scr_idx, (d, img) in enumerate(uncached_gemini_specs):
                    if abort_local_evt.is_set() or (cancel_event is not None and cancel_event.is_set()):
                        break

                    inflight_evt: Optional[threading.Event] = None
                    with self._screen_cache_lock:
                        if (d, "gemini_cached") in self._screen_lines_cache or (d, "local_cached") in self._screen_lines_cache:
                            evt_obj = owned_any_events.get(d)
                            if evt_obj is not None:
                                evt_obj.set()
                            continue
                        if (d, "local_cached") in self._screen_cache_inflight:
                            continue
                        inflight_evt = threading.Event()
                        self._screen_cache_inflight[(d, "local_cached")] = inflight_evt

                    try:
                        t_loc = time.monotonic()
                        sub_boxes = per_screen_sub_boxes[d]
                        screen_lines: Optional[List[DetectedLine]] = None
                        aborted = False

                        if sub_boxes is None:
                            with self._ocr_exec_lock:
                                screen_lines = self._detect_screen_lines_pytesseract_fallback(img)
                        elif not sub_boxes:
                            screen_lines = []
                        else:
                            raw_lines: List[DetectedLine] = []
                            for sub_box in sub_boxes:
                                if abort_local_evt.is_set() or (cancel_event is not None and cancel_event.is_set()):
                                    aborted = True
                                    break
                                with self._ocr_exec_lock:
                                    ln = self._recognize_local_detected_line(img, sub_box)
                                if ln is not None:
                                    raw_lines.append(ln)
                            if not aborted:
                                screen_lines = self._filter_overlapping_lines(raw_lines)

                        if not aborted and screen_lines is not None:
                            self._store_screen_lines_cache((d, "local_cached"), screen_lines)
                            evt_obj = owned_any_events.get(d)
                            if evt_obj is not None:
                                evt_obj.set()
                            logger.info(
                                "Hybrid local OCR completed for screen %d: %d lines (%.2fs).",
                                scr_idx,
                                len(screen_lines),
                                time.monotonic() - t_loc,
                            )
                    finally:
                        if inflight_evt is not None:
                            with self._screen_cache_lock:
                                self._screen_cache_inflight.pop((d, "local_cached"), None)
                            inflight_evt.set()
            except Exception as exc:
                logger.warning("Hybrid local OCR worker error: %s", exc)
            finally:
                handle.local_all_done_evt.set()
                handle.any_batch_ready_evt.set()
                _release_worker()

        threading.Thread(target=_run_gemini_worker, daemon=True).start()
        threading.Thread(target=_run_local_worker, daemon=True).start()
        return handle

    def _detect_screen_sub_boxes(
        self, screen_img: Image.Image
    ) -> Optional[List[Tuple[int, int, int, int]]]:
        """Run DBNet text detection and horizontal space splitting; returns None if DBNet is unavailable."""
        if self._detector is None:
            return None

        w, h = screen_img.size
        img_np = np.array(screen_img)
        det_out = self._detector(
            img_np,
            use_det=True,
            use_cls=False,
            use_rec=False,
            box_thresh=0.2,
            unclip_ratio=2.0,
        )
        boxes = det_out[0] if isinstance(det_out, tuple) else det_out
        if not boxes:
            return []

        sub_boxes: List[Tuple[int, int, int, int]] = []
        for item in boxes:
            pts = item[0] if (isinstance(item, (list, tuple)) and len(item) == 3 and isinstance(item[1], str)) else item
            xs = [p[0] for p in pts]
            ys = [p[1] for p in pts]
            x1, y1 = int(max(0, min(xs))), int(max(0, min(ys)))
            x2, y2 = int(min(w, max(xs))), int(min(h, max(ys)))
            if x2 - x1 < 4 or y2 - y1 < 4:
                continue
            # Pad horizontally by 4px so edge kanji radicals are never clipped
            x1 = max(0, x1 - 4)
            x2 = min(w, x2 + 4)

            for sub_box in self._split_horizontal_box_on_spaces(screen_img, (x1, y1, x2, y2)):
                sx1, sy1, sx2, sy2 = sub_box
                if (sx2 - sx1) < 8 or (sy2 - sy1) < 8:
                    continue
                sub_boxes.append(sub_box)

        return sub_boxes

    def _recognize_local_detected_line(
        self, screen_img: Image.Image, sub_box: Tuple[int, int, int, int]
    ) -> Optional[DetectedLine]:
        """Recognize a single DBNet sub-box with all 4 local OCR engines, returning None for noise/empty crops."""
        sx1, sy1, sx2, sy2 = sub_box
        is_vert = (sy2 - sy1) > (sx2 - sx1) * 1.5
        text, has_lead, has_trail, clean_raw, eng_texts, eng_raw_texts, alt_texts = (
            self._recognize_line_crop_all_engines(screen_img, sub_box)
        )
        if text and not is_noise_chunk(clean_raw or text):
            return DetectedLine(
                sub_box,
                text,
                is_vert,
                has_lead,
                has_trail,
                raw_text=clean_raw or text,
                engine_texts=eng_texts,
                engine_raw_texts=eng_raw_texts,
                alt_texts=alt_texts,
            )
        return None

    def _filter_overlapping_lines(self, lines: List[DetectedLine]) -> List[DetectedLine]:
        """Suppress tiny redundant overlapping subset boxes and inter-word bridge artifacts on the same line."""
        filtered_lines: List[DetectedLine] = []
        for l in lines:
            is_sub = False
            lx1, ly1, lx2, ly2 = l.bbox
            l_area = (lx2 - lx1) * (ly2 - ly1)
            lh = ly2 - ly1
            lw = lx2 - lx1

            # 1. Direct subset check: l is largely covered by a single larger box o
            for o in lines:
                if o is l:
                    continue
                ox1, oy1, ox2, oy2 = o.bbox
                o_area = (ox2 - ox1) * (oy2 - oy1)
                ov_w = max(0, min(ox2, lx2) - max(ox1, lx1))
                ov_h = max(0, min(oy2, ly2) - max(oy1, ly1))
                if ov_w * ov_h >= 0.70 * l_area and o_area > l_area * 1.4:
                    is_sub = True
                    break

            # 2. Horizontal bridge check: l straddles the gap between two adjacent words a and b on the same line
            if not is_sub and not l.is_vertical:
                for a in lines:
                    if a is l or a.is_vertical:
                        continue
                    ax1, ay1, ax2, ay2 = a.bbox
                    v_ov_a = min(ly2, ay2) - max(ly1, ay1)
                    if v_ov_a < 0.5 * min(lh, ay2 - ay1):
                        continue
                    if not (ax1 < lx1 < ax2):
                        continue
                    if (ax2 - lx1) >= (ax2 - ax1) * 0.65:
                        continue

                    for b in lines:
                        if b is l or b is a or b.is_vertical:
                            continue
                        bx1, by1, bx2, by2 = b.bbox
                        v_ov_b = min(ly2, by2) - max(ly1, by1)
                        if v_ov_b < 0.5 * min(lh, by2 - by1):
                            continue
                        if not (bx1 < lx2 < bx2):
                            continue
                        if bx1 < ax1 or (bx1 + bx2) <= (ax1 + ax2):
                            continue
                        if (lx2 - bx1) >= (bx2 - bx1) * 0.65:
                            continue
                        if (lx1 - ax1 >= 6) and (bx2 - lx2 >= 6):
                            logger.debug("Filtered out straddle bridge line crop '%s' between '%s' and '%s'", l.text, a.text, b.text)
                            is_sub = True
                            break
                    if is_sub:
                        break

            # 3. Vertical bridge check for vertical Japanese text columns
            if not is_sub and l.is_vertical:
                for a in lines:
                    if a is l or not a.is_vertical:
                        continue
                    ax1, ay1, ax2, ay2 = a.bbox
                    h_ov_a = min(lx2, ax2) - max(lx1, ax1)
                    if h_ov_a < 0.5 * min(lw, ax2 - ax1):
                        continue
                    if not (ay1 < ly1 < ay2):
                        continue
                    if (ay2 - ly1) >= (ay2 - ay1) * 0.65:
                        continue

                    for b in lines:
                        if b is l or b is a or not b.is_vertical:
                            continue
                        bx1, by1, bx2, by2 = b.bbox
                        h_ov_b = min(lx2, bx2) - max(lx1, bx1)
                        if h_ov_b < 0.5 * min(lw, bx2 - bx1):
                            continue
                        if not (by1 < ly2 < by2):
                            continue
                        if by1 < ay1 or (by1 + by2) <= (ay1 + ay2):
                            continue
                        if (ly2 - by1) >= (by2 - by1) * 0.65:
                            continue
                        if (ly1 - ay1 >= 6) and (by2 - ly2 >= 6):
                            logger.debug("Filtered out vertical straddle bridge line crop '%s' between '%s' and '%s'", l.text, a.text, b.text)
                            is_sub = True
                            break
                    if is_sub:
                        break

            if not is_sub:
                filtered_lines.append(l)

        return filtered_lines

    def _finalize_screen_lines(
        self,
        screen_img: Image.Image,
        sub_boxes: List[Tuple[int, int, int, int]],
        gemini_map: Dict[int, str],
    ) -> List[DetectedLine]:
        """Build DetectedLine objects from sub-boxes and optional Gemini transcriptions, then filter overlaps."""
        if not sub_boxes:
            return []

        lines: List[DetectedLine] = []
        for idx, sub_box in enumerate(sub_boxes):
            sx1, sy1, sx2, sy2 = sub_box
            is_vert = (sy2 - sy1) > (sx2 - sx1) * 1.5
            if idx in gemini_map:
                g_raw = gemini_map[idx]
                if not g_raw or not g_raw.strip():
                    continue
                c_txt, r_txt, hl, ht = self._clean_ocr_text_pair(g_raw)
                if c_txt and not is_noise_chunk(r_txt or c_txt):
                    eng_texts = {eng: c_txt for eng in OCR_ENGINE_NAMES}
                    eng_raw_texts = {eng: (r_txt or c_txt) for eng in OCR_ENGINE_NAMES}
                    lines.append(
                        DetectedLine(
                            sub_box,
                            c_txt,
                            is_vert,
                            hl,
                            ht,
                            raw_text=r_txt or c_txt,
                            engine_texts=eng_texts,
                            engine_raw_texts=eng_raw_texts,
                            alt_texts=[],
                            ocr_source="gemini",
                        )
                    )
                continue

            ln = self._recognize_local_detected_line(screen_img, sub_box)
            if ln is not None:
                lines.append(ln)

        return self._filter_overlapping_lines(lines)

    _KATAKANA_CONFUSABLE_GROUPS = (
        frozenset("スツシッ"),
        frozenset("ソンノ"),
        frozenset("クワフウ"),
        frozenset("アマ"),
        frozenset("ヌス"),
        frozenset("ラヲ"),
    )

    @classmethod
    def _is_confusable_katakana_pair(cls, ch_a: str, ch_b: str) -> bool:
        """Return True if two differing characters belong to the same visually confusable katakana group."""
        if ch_a == ch_b:
            return True
        return any(ch_a in grp and ch_b in grp for grp in cls._KATAKANA_CONFUSABLE_GROUPS)

    @classmethod
    def _reconcile_gemini_with_local_lines(
        cls,
        gemini_lines: List[DetectedLine],
        local_lines: List[DetectedLine],
    ) -> None:
        """
        Correct single/few-character visually confusable katakana misreads (e.g. 'ス' vs 'ツ')
        in Gemini transcriptions when >=2 local OCR engines agree on the exact-length alternative.
        """
        if not gemini_lines or not local_lines:
            return

        local_by_bbox = {ln.bbox: ln for ln in local_lines}
        for g_ln in gemini_lines:
            loc_ln = local_by_bbox.get(g_ln.bbox)
            if loc_ln is None or not loc_ln.engine_texts or not g_ln.text:
                continue

            g_txt = g_ln.text
            cand_counts: Dict[str, int] = {}
            for eng_txt in loc_ln.engine_texts.values():
                if not eng_txt or eng_txt == g_txt or len(eng_txt) != len(g_txt):
                    continue
                if all(cls._is_confusable_katakana_pair(gc, lc) for gc, lc in zip(g_txt, eng_txt)):
                    cand_counts[eng_txt] = cand_counts.get(eng_txt, 0) + 1

            if not cand_counts:
                continue

            best_cand, best_votes = max(cand_counts.items(), key=lambda kv: kv[1])
            if best_votes >= 2:
                logger.info(
                    "Hybrid OCR katakana reconciliation: '%s' -> '%s' (%d local engines agreed).",
                    g_txt,
                    best_cand,
                    best_votes,
                )
                if g_ln.raw_text and g_txt in g_ln.raw_text:
                    g_ln.raw_text = g_ln.raw_text.replace(g_txt, best_cand, 1)
                else:
                    g_ln.raw_text = best_cand
                g_ln.text = best_cand
                for eng in OCR_ENGINE_NAMES:
                    g_ln.engine_texts[eng] = best_cand
                    if g_ln.engine_raw_texts and eng in g_ln.engine_raw_texts:
                        r_val = g_ln.engine_raw_texts[eng]
                        g_ln.engine_raw_texts[eng] = (
                            r_val.replace(g_txt, best_cand, 1) if g_txt in r_val else best_cand
                        )

    def _detect_screen_lines_pytesseract_fallback(
        self, screen_img: Image.Image
    ) -> List[DetectedLine]:
        """Fallback layout detection when RapidOCR DBNet detector is unavailable."""
        w, h = screen_img.size
        lines: List[DetectedLine] = []
        upscaled = screen_img.resize(
            (w * OCR_UPSCALE_FACTOR, h * OCR_UPSCALE_FACTOR),
            Image.Resampling.LANCZOS,
        )
        try:
            import pytesseract
            data = pytesseract.image_to_data(
                upscaled, lang="jpn", output_type=pytesseract.Output.DICT
            )
            line_boxes: Dict[Tuple[int, int, int], List[int]] = {}
            for i, text in enumerate(data["text"]):
                if not text or not text.strip():
                    continue
                key = (data["block_num"][i], data["par_num"][i], data["line_num"][i])
                lx = data["left"][i] // OCR_UPSCALE_FACTOR
                ly = data["top"][i] // OCR_UPSCALE_FACTOR
                lw = data["width"][i] // OCR_UPSCALE_FACTOR
                lh = data["height"][i] // OCR_UPSCALE_FACTOR
                if key not in line_boxes:
                    line_boxes[key] = [lx, ly, lx + lw, ly + lh]
                else:
                    b = line_boxes[key]
                    b[0] = min(b[0], lx)
                    b[1] = min(b[1], ly)
                    b[2] = max(b[2], lx + lw)
                    b[3] = max(b[3], ly + lh)

            for b in line_boxes.values():
                bbox = (max(0, b[0]), max(0, b[1]), min(w, b[2]), min(h, b[3]))
                text, has_lead, has_trail = self._recognize_line_crop_with_meta(screen_img, bbox)
                if text:
                    is_vert = (bbox[3] - bbox[1]) > (bbox[2] - bbox[0]) * 1.5
                    lines.append(DetectedLine(bbox, text, is_vert, has_lead, has_trail))
        except Exception as exc:
            logger.error("Fallback layout detection failed: %s", exc)

        return lines

    def _detect_screen_lines_uncached(
        self, screen_img: Image.Image, mode: str
    ) -> List[DetectedLine]:
        """
        Run DBNet (`use_det=True, use_rec=False`) and recognize each detected sub-box
        via either Gemini Batch OCR (`"gemini_cached"`) or the 4 local engines (`"local_cached"`).
        """
        sub_boxes = self._detect_screen_sub_boxes(screen_img)
        if sub_boxes is None:
            return self._detect_screen_lines_pytesseract_fallback(screen_img)
        if not sub_boxes:
            return []

        gemini_map: Dict[int, str] = {}
        if mode == "gemini_cached":
            gemini_map = self._batch_ocr_with_gemini(screen_img, sub_boxes)
        return self._finalize_screen_lines(screen_img, sub_boxes, gemini_map)

    def _lines_belong_to_same_paragraph(self, a: DetectedLine, b: DetectedLine) -> bool:
        """
        Determine whether two detected lines belong to the same dialogue box / paragraph
        so wrapped lines (and space-separated chunks on the same row) can be stitched together.
        """
        if a.is_vertical != b.is_vertical:
            return False

        ax1, ay1, ax2, ay2 = a.bbox
        bx1, by1, bx2, by2 = b.bbox

        if not a.is_vertical:
            avg_h = (a.height + b.height) / 2.0
            v_gap = max(by1 - ay2, ay1 - by2, 0)
            v_overlap = min(ay2, by2) - max(ay1, by1)
            h_gap = max(bx1 - ax2, ax1 - bx2, 0)
            h_overlap = min(ax2, bx2) - max(ax1, bx1)
            left_aligned = abs(ax1 - bx1) <= avg_h * 2.5
            same_row = v_overlap >= avg_h * 0.35 and h_gap <= avg_h * 1.9
            return same_row or (v_gap <= avg_h * 1.4 and (h_overlap > 0 or left_aligned))
        else:
            avg_w = (a.width + b.width) / 2.0
            h_gap = max(bx1 - ax2, ax1 - bx2, 0)
            v_overlap = min(ay2, by2) - max(ay1, by1)
            top_aligned = abs(ay1 - by1) <= avg_w * 2.5
            return h_gap <= avg_w * 1.6 and (v_overlap > 0 or top_aligned)

    def _analyze_morphemes(self, full_text: str) -> List[MorphemeSpan]:
        """
        Tokenize the stitched multi-line paragraph with Fugashi (MeCab), merge trailing
        auxiliary verbs, suffixes, and subsidiary conjugations (e.g. 食べ + させ + られ + て + い + なかっ + た)
        into contiguous MorphemeSpans anchored to the root word's lemma, and record the ordered
        human-readable conjugation chain (`conjugation_steps`, e.g. `['causative', 'passive', 'continuous', 'negative', 'past']`).
        """
        if not full_text or self._tagger is None:
            return []

        nodes = self._tagger(full_text)
        spans: List[MorphemeSpan] = []
        cursor = 0

        AUX_POS1 = {"助動詞"}
        CONJ_PARTICLES = {"て", "で", "ば", "たら", "だり", "たり", "ながら", "ちゃ", "じゃ"}
        SUBSIDIARY_TE_VERBS = {
            "居る": "continuous",
            "来る": "-te kuru",
            "行く": "-te iku",
            "呉れる": "-te kureru",
            "上げる": "-te ageru",
            "貰う": "-te morau",
            "見る": "-te miru",
            "下さる": "-te kudasaru",
            "頂く": "-te itadaku",
            "遣る": "-te yaru",
            "有る": "resultative (-te aru)",
            "置く": "preparatory (-te oku)",
            "仕舞う": "completion (-te shimau)",
        }

        def _append_step(steps: List[str], step: str) -> None:
            if step and (not steps or steps[-1] != step):
                steps.append(step)

        for node in nodes:
            surf = node.surface
            idx = full_text.find(surf, cursor)
            if idx == -1:
                idx = cursor
            end_idx = idx + len(surf)
            cursor = end_idx

            pos1 = getattr(node.feature, "pos1", "") or ""
            pos2 = getattr(node.feature, "pos2", "") or ""
            cForm = getattr(node.feature, "cForm", "") or ""
            raw_lemma = getattr(node.feature, "lemma", None) or ""
            if "-" in raw_lemma:
                raw_lemma = raw_lemma.split("-")[0]
            lemma = getattr(node.feature, "orthBase", None) or raw_lemma or surf
            if "-" in lemma:
                lemma = lemma.split("-")[0]
            kana = getattr(node.feature, "kana", None) or ""

            # Prevent UniDic classical copula 'だ' / 'なり' 連用形 analysis from overriding
            # standard modern particles 'に' and 'で'
            if surf in {"に", "で"} and (lemma in {"だ", "なり"} or raw_lemma in {"だ", "なり"}):
                lemma = surf
                raw_lemma = surf
                pos1 = "助詞"
                kana = "ニ" if surf == "に" else "デ"

            prev = spans[-1] if spans else None
            SURU_LEMMAS = {"為る", "する", "出来る", "できる"}
            is_suru_continuation = (
                prev is not None
                and prev.pos1 in {"名詞", "形状詞"}
                and (
                    (pos1 == "動詞" and (raw_lemma in SURU_LEMMAS or lemma in SURU_LEMMAS or surf in {"し", "した", "して", "しよう", "される", "させる", "できる"}))
                    or (pos1 == "動詞" and pos2 == "非自立可能" and (raw_lemma in SURU_LEMMAS or lemma in SURU_LEMMAS))
                )
            )
            is_te_subsidiary = (
                prev is not None
                and prev.pos1 in {"動詞", "形容詞", "形状詞"}
                and prev.conjugation_steps
                and prev.conjugation_steps[-1] in {"te-form", "continuous"}
                and pos1 == "動詞"
                and pos2 == "非自立可能"
                and raw_lemma in SUBSIDIARY_TE_VERBS
            )
            is_adj_neg = (
                prev is not None
                and prev.pos1 in {"動詞", "形容詞", "形状詞"}
                and pos1 == "形容詞"
                and pos2 == "非自立可能"
                and raw_lemma in {"無い", "ない"}
            )
            is_conj_particle = (
                prev is not None
                and prev.pos1 in {"動詞", "形容詞", "形状詞"}
                and pos1 == "助詞"
                and (
                    (
                        pos2 in {"接続助詞", "副助詞"}
                        and (surf in CONJ_PARTICLES or lemma in {"て", "ば", "たり", "ながら"})
                    )
                    or (
                        prev.conjugation_steps
                        and prev.conjugation_steps[-1] == "te-form"
                        and surf in {"も", "は"}
                    )
                )
            )
            is_aux_continuation = (
                prev is not None
                and (
                    prev.pos1 in {"動詞", "形容詞", "形状詞", "名詞"}
                    or (prev.pos1 == "副詞" and prev.lemma in {"どう", "そう", "こう", "ああ"})
                )
                and pos1 in AUX_POS1
                and not (prev.pos1 == "名詞" and lemma in {"だ", "です"})
            )

            if prev is not None and (
                is_suru_continuation or is_te_subsidiary or is_adj_neg or is_conj_particle or is_aux_continuation
            ):
                prev.end_idx = end_idx
                prev.surface = full_text[prev.start_idx : end_idx]

                if is_suru_continuation:
                    prev.pos1 = "動詞"
                    _append_step(prev.conjugation_steps, "suru")
                    if cForm.startswith("意志推量形") or surf in {"しよう", "そう"}:
                        _append_step(prev.conjugation_steps, "volitional")
                    elif cForm.startswith("命令形") or surf in {"しろ", "せよ"}:
                        _append_step(prev.conjugation_steps, "imperative")
                    elif lemma in {"出来る", "できる"} or surf.startswith("でき"):
                        _append_step(prev.conjugation_steps, "potential")
                    elif surf in {"される", "された", "されない"}:
                        _append_step(prev.conjugation_steps, "passive")
                    elif surf in {"させる", "させた", "させない"}:
                        _append_step(prev.conjugation_steps, "causative")
                    elif surf in {"して", "し"}:
                        _append_step(prev.conjugation_steps, "te-form")
                    elif surf == "した":
                        _append_step(prev.conjugation_steps, "past")
                elif is_te_subsidiary:
                    sub_label = SUBSIDIARY_TE_VERBS[raw_lemma]
                    if prev.conjugation_steps[-1] == "te-form":
                        prev.conjugation_steps[-1] = sub_label
                    else:
                        _append_step(prev.conjugation_steps, sub_label)
                    if cForm.startswith("命令形"):
                        _append_step(prev.conjugation_steps, "imperative")
                    elif cForm.startswith("意志推量形"):
                        _append_step(prev.conjugation_steps, "volitional")
                elif is_adj_neg:
                    _append_step(prev.conjugation_steps, "negative")
                elif is_conj_particle:
                    if lemma == "ば" or surf == "ば":
                        _append_step(prev.conjugation_steps, "conditional (-eba)")
                    elif lemma == "たり" or surf in {"たり", "だり"}:
                        _append_step(prev.conjugation_steps, "-tari")
                    elif lemma == "ながら" or surf == "ながら":
                        _append_step(prev.conjugation_steps, "while (-nagara)")
                    elif surf in {"ちゃ", "じゃ"}:
                        _append_step(prev.conjugation_steps, "te-wa (contracted)")
                    elif surf == "も":
                        _append_step(prev.conjugation_steps, "even if (-temo)")
                    elif surf == "は":
                        _append_step(prev.conjugation_steps, "-te wa")
                    else:
                        _append_step(prev.conjugation_steps, "te-form")
                elif pos1 == "接尾辞":
                    prev.lemma = prev.surface
                    prev.kanji_lemma = ""
                    prev.reading = (prev.reading or "") + kana
                elif pos1 == "助動詞":
                    if lemma in {"せる", "させる"}:
                        _append_step(prev.conjugation_steps, "causative")
                    elif lemma in {"れる", "られる"}:
                        _append_step(prev.conjugation_steps, "passive")
                    elif lemma in {"てる", "でる"}:
                        _append_step(prev.conjugation_steps, "continuous")
                    elif lemma in {"てく", "でく"}:
                        _append_step(prev.conjugation_steps, "-te iku")
                    elif lemma in {"ちゃう", "じゃう", "ちまう", "じまう"}:
                        _append_step(prev.conjugation_steps, "completion (-te shimau)")
                    elif lemma in {"とく", "どく"}:
                        _append_step(prev.conjugation_steps, "preparatory (-te oku)")
                    elif lemma in {"ない", "ぬ", "ん"}:
                        _append_step(prev.conjugation_steps, "negative")
                    elif lemma == "まい":
                        _append_step(prev.conjugation_steps, "negative volitional")
                    elif lemma in {"たい", "たがる"}:
                        _append_step(prev.conjugation_steps, "desiderative (-tai)")
                    elif lemma in {"ます", "です"}:
                        _append_step(prev.conjugation_steps, "polite")
                    elif raw_lemma == "た" or (lemma in {"た", "だ"} and raw_lemma != "だ"):
                        if cForm.startswith("仮定形") or surf in {"たら", "だら"}:
                            _append_step(prev.conjugation_steps, "conditional (-tara)")
                        else:
                            _append_step(prev.conjugation_steps, "past")
                    elif raw_lemma == "だ":
                        if cForm.startswith("仮定形") or surf == "なら":
                            _append_step(prev.conjugation_steps, "conditional (-nara)")
                        elif cForm.startswith("意志推量形") or surf == "だろう":
                            _append_step(prev.conjugation_steps, "volitional")
                        elif cForm.startswith("連体形") or surf == "な":
                            _append_step(prev.conjugation_steps, "attributive (な before の/ん)")
                    elif lemma in {"う", "よう"}:
                        _append_step(prev.conjugation_steps, "volitional")
                    elif lemma == "らしい":
                        _append_step(prev.conjugation_steps, "conjectural (-rashii)")
                    elif lemma in {"そうだ", "そう"}:
                        _append_step(prev.conjugation_steps, "seeming (-sou)")
                    elif lemma == "べし":
                        _append_step(prev.conjugation_steps, "obligation (-beshi)")

                    if cForm.startswith("命令形"):
                        _append_step(prev.conjugation_steps, "imperative")
                    elif cForm.startswith("意志推量形"):
                        _append_step(prev.conjugation_steps, "volitional")
            else:
                init_steps: List[str] = []
                if pos1 in {"動詞", "形容詞"}:
                    raw_l = getattr(node.feature, "lemma", None) or ""
                    if "-" in raw_l:
                        raw_l = raw_l.split("-")[0]
                    orth_l = getattr(node.feature, "orthBase", None) or raw_l or surf
                    if "-" in orth_l:
                        orth_l = orth_l.split("-")[0]
                    if (
                        pos1 == "動詞"
                        and orth_l.endswith(("せる", "させる"))
                        and not raw_l.endswith(("せる", "させる"))
                    ):
                        init_steps.append("causative")

                    if cForm.startswith("命令形"):
                        init_steps.append("imperative")
                    elif cForm.startswith("意志推量形"):
                        init_steps.append("volitional")
                elif pos1 == "助動詞":
                    if raw_lemma == "だ":
                        if cForm.startswith("意志推量形") or surf in {"だろ", "だろう"}:
                            lemma = "だろう"
                            raw_lemma = "だろう"
                            kana = "ダロウ"
                            init_steps.append("volitional")
                        elif cForm.startswith("仮定形") or surf == "なら":
                            lemma = "なら"
                            raw_lemma = "なら"
                            kana = "ナラ"
                            init_steps.append("conditional (-nara)")
                        elif cForm.startswith("連体形") or surf == "な":
                            init_steps.append("attributive (な before の/ん)")
                    elif raw_lemma == "です" and (cForm.startswith("意志推量形") or surf in {"でしょ", "でしょう"}):
                        lemma = "でしょう"
                        raw_lemma = "でしょう"
                        kana = "デショウ"
                        init_steps.extend(["polite", "volitional"])
                spans.append(
                    MorphemeSpan(
                        start_idx=idx,
                        end_idx=end_idx,
                        surface=surf,
                        lemma=lemma,
                        reading=kana,
                        kanji_lemma=raw_lemma or lemma,
                        pos1=pos1,
                        conjugation_steps=init_steps,
                    )
                )

        return spans

    def build_stitched_paragraphs_by_engine(
        self, screen_img: Image.Image, ocr_mode: Optional[str] = None
    ) -> Dict[str, List[StitchedParagraph]]:
        """
        Cluster detected lines into paragraphs and build a separate `List[StitchedParagraph]`
        for each of the 4 OCR engines (`"Manga"`, `"Paddle"`, `"Yomi"`, `"NDL"`).
        """
        lines = self.detect_screen_lines(screen_img, ocr_mode=ocr_mode)
        with self._lock:
            if not lines:
                return {eng: [] for eng in OCR_ENGINE_NAMES}

            n = len(lines)
            visited = [False] * n
            clusters: List[List[DetectedLine]] = []

            for i in range(n):
                if visited[i]:
                    continue
                visited[i] = True
                queue = [i]
                cluster = [lines[i]]
                while queue:
                    curr = queue.pop(0)
                    for j in range(n):
                        if not visited[j] and self._lines_belong_to_same_paragraph(lines[curr], lines[j]):
                            visited[j] = True
                            queue.append(j)
                            cluster.append(lines[j])
                clusters.append(cluster)

            paragraphs_by_engine: Dict[str, List[StitchedParagraph]] = {
                eng: [] for eng in OCR_ENGINE_NAMES
            }
            for cluster in clusters:
                is_vert = cluster[0].is_vertical
                if not is_vert:
                    by_cy = sorted(cluster, key=lambda ln: ln.center[1])
                    rows: List[List[DetectedLine]] = []
                    for ln in by_cy:
                        if not rows:
                            rows.append([ln])
                        else:
                            prev_row = rows[-1]
                            row_cy = sum(r.center[1] for r in prev_row) / len(prev_row)
                            row_h = sum(r.height for r in prev_row) / len(prev_row)
                            if abs(ln.center[1] - row_cy) <= max(row_h, ln.height) * 0.45:
                                prev_row.append(ln)
                            else:
                                rows.append([ln])
                    for r in rows:
                        r.sort(key=lambda ln: ln.bbox[0])
                    cluster = [ln for r in rows for ln in r]
                else:
                    cluster.sort(key=lambda ln: (-ln.bbox[2], ln.bbox[1]))

                px1 = min(ln.bbox[0] for ln in cluster)
                py1 = min(ln.bbox[1] for ln in cluster)
                px2 = max(ln.bbox[2] for ln in cluster)
                py2 = max(ln.bbox[3] for ln in cluster)

                cluster_has_jp = any(JP_CHAR_RE.search(ln.text) for ln in cluster)
                cluster_ocr_source = (
                    "gemini"
                    if (cluster and all(ln.ocr_source == "gemini" for ln in cluster))
                    else "local"
                )
                for eng in OCR_ENGINE_NAMES:
                    chars: List[CharSpan] = []
                    text_parts: List[str] = []
                    alt_phrases: Dict[int, List[str]] = {}
                    current_char_offset = 0
                    for ln in cluster:
                        eng_txt = (ln.engine_texts.get(eng) if ln.engine_texts else None) or ln.text
                        if not eng_txt:
                            continue
                        if cluster_has_jp and is_noise_chunk(eng_txt):
                            continue
                        text_parts.append(eng_txt)
                        eng_ln = DetectedLine(
                            bbox=ln.bbox,
                            text=eng_txt,
                            is_vertical=ln.is_vertical,
                            has_leading_punct=ln.has_leading_punct,
                            has_trailing_punct=ln.has_trailing_punct,
                            raw_text=(ln.engine_raw_texts.get(eng) if ln.engine_raw_texts else "") or eng_txt,
                            alt_texts=list(ln.alt_texts),
                        )
                        line_chars = self._compute_line_char_spans(screen_img, eng_ln)
                        for cs in line_chars:
                            cs.alt_texts = list(ln.alt_texts)
                        chars.extend(line_chars)
                        if ln.alt_texts:
                            alt_phrases[current_char_offset] = list(ln.alt_texts)
                        current_char_offset += len(eng_txt)

                    full_text = "".join(text_parts)
                    if not full_text:
                        continue
                    morphemes = self._analyze_morphemes(full_text)
                    paragraphs_by_engine[eng].append(
                        StitchedParagraph(
                            bbox=(px1, py1, px2, py2),
                            full_text=full_text,
                            chars=chars,
                            morphemes=morphemes,
                            alt_phrases=alt_phrases,
                            ocr_source=cluster_ocr_source,
                        )
                    )

            logger.info(
                "Indexed %d lines into %d stitched paragraphs across 4 OCR engines.",
                len(lines),
                len(paragraphs_by_engine.get("Manga", [])),
            )
            return paragraphs_by_engine



    def _fallback_direct_crop_paragraph(
        self, screen_img: Image.Image, x: int, y: int
    ) -> Optional[StitchedParagraph]:
        """
        Fallback if full-screen layout detector found no boxes near (x, y) (e.g. small 2-char
        white-on-blue speaker nameplates like 'ママ' or stylized UI buttons):
        isolates the local vertical/horizontal ink band around (x, y) and runs OCR + MeCab.
        """
        w, h = screen_img.size
        cx = max(0, min(x, w - 1))
        cy = max(0, min(y, h - 1))

        sx1, sy1 = max(0, cx - 48), max(0, cy - 14)
        sx2, sy2 = min(w, cx + 56), min(h, cy + 14)
        if sx2 - sx1 < 6 or sy2 - sy1 < 6:
            return None

        sub = np.array(screen_img.crop((sx1, sy1, sx2, sy2)), dtype=np.float32)
        sh, sw = sub.shape[:2]
        rx, ry = cx - sx1, cy - sy1
        seed = sub[max(0, ry - 6) : min(sh, ry + 7), max(0, rx - 18) : min(sw, rx + 19), :]
        l_sub = 0.299 * sub[:, :, 0] + 0.587 * sub[:, :, 1] + 0.114 * sub[:, :, 2]
        l_seed = 0.299 * seed[:, :, 0] + 0.587 * seed[:, :, 1] + 0.114 * seed[:, :, 2]
        bg_l = float(np.median(l_seed))
        p10, p90 = float(np.percentile(l_seed, 10)), float(np.percentile(l_seed, 90))
        if (p90 - bg_l) > (bg_l - p10):
            ink = (l_sub - bg_l) >= max(22.0, (p90 - bg_l) * 0.40)
        else:
            gray_sub = self._color_aware_gray(sub)
            gray_seed = self._color_aware_gray(seed)
            bg_m = float(np.median(gray_seed))
            p10_m = float(np.percentile(gray_seed, 10))
            ink = (bg_m - gray_sub) >= max(22.0, (bg_m - p10_m) * 0.40)

        box = (max(0, cx - 24), max(0, cy - 9), min(w, cx + 28), min(h, cy + 9))
        x_win_ink = ink[:, max(0, rx - 24) : min(sw, rx + 25)]
        row_has = (x_win_ink.sum(axis=1) >= 2) & (x_win_ink.mean(axis=1) <= 0.80)
        ys = np.where(row_has)[0]
        if ys.size > 0:
            bands: List[Tuple[int, int]] = []
            bs = bp = int(ys[0])
            for yv in ys[1:]:
                yv_i = int(yv)
                if yv_i > bp + 1:
                    bands.append((bs, bp + 1))
                    bs = yv_i
                bp = yv_i
            bands.append((bs, bp + 1))
            valid_b = [b for b in bands if b[1] - b[0] >= 5] or bands
            vy1, vy2 = min(
                valid_b,
                key=lambda b: 0 if b[0] <= ry <= b[1] else min(abs(ry - b[0]), abs(ry - b[1])),
            )
            col_has = ink[vy1:vy2, :].any(axis=0)
            xs = np.where(col_has)[0]
            if xs.size > 0:
                runs: List[List[int]] = []
                rs = rp = int(xs[0])
                for xv in xs[1:]:
                    xv_i = int(xv)
                    if xv_i > rp + 1:
                        runs.append([rs, rp + 1])
                        rs = xv_i
                    rp = xv_i
                runs.append([rs, rp + 1])
                merged: List[List[int]] = [runs[0][:]]
                for r in runs[1:]:
                    if r[0] - merged[-1][1] <= 9:
                        merged[-1][1] = r[1]
                    else:
                        merged.append(r[:])
                valid_r = [r for r in merged if r[1] - r[0] >= 5] or merged
                vx1, vx2 = min(
                    valid_r,
                    key=lambda r: 0 if r[0] <= rx <= r[1] else min(abs(rx - r[0]), abs(rx - r[1])),
                )
                box = (
                    max(0, sx1 + vx1 - 3),
                    max(0, sy1 + vy1 - 2),
                    min(w, sx1 + vx2 + 3),
                    min(h, sy1 + vy2 + 2),
                )

        if box[2] <= box[0] or box[3] <= box[1]:
            return None
        text = self._recognize_line_crop(screen_img, box)
        if not text:
            return None

        char_w = (box[2] - box[0]) / float(max(1, len(text)))
        chars = [
            CharSpan(
                ch,
                (box[0] + i * char_w, float(box[1]), box[0] + (i + 1) * char_w, float(box[3])),
            )
            for i, ch in enumerate(text)
        ]
        return StitchedParagraph(
            bbox=box,
            full_text=text,
            chars=chars,
            morphemes=self._analyze_morphemes(text),
        )

    def _fallback_direct_crop_paragraphs_by_engine(
        self, screen_img: Image.Image, x: int, y: int
    ) -> Dict[str, StitchedParagraph]:
        """Run local ink-band fallback crop around `(x, y)` through all 4 OCR engines."""
        base_para = self._fallback_direct_crop_paragraph(screen_img, x, y)
        if base_para is None:
            return {}
        box = base_para.bbox
        _, _, _, _, eng_texts, _, *rest = self._recognize_line_crop_all_engines(screen_img, box)
        out: Dict[str, StitchedParagraph] = {}
        for eng in OCR_ENGINE_NAMES:
            txt = eng_texts.get(eng, "")
            if not txt:
                continue
            char_w = (box[2] - box[0]) / float(max(1, len(txt)))
            chars = [
                CharSpan(
                    ch,
                    (box[0] + i * char_w, float(box[1]), box[0] + (i + 1) * char_w, float(box[3])),
                )
                for i, ch in enumerate(txt)
            ]
            out[eng] = StitchedParagraph(
                bbox=box,
                full_text=txt,
                chars=chars,
                morphemes=self._analyze_morphemes(txt),
            )
        return out

    @staticmethod
    def _katakana_to_hiragana(text: str) -> str:
        """Convert Katakana characters (U+30A1..U+30F6) to Hiragana (U+3041..U+3096)."""
        out: List[str] = []
        for ch in text:
            code = ord(ch)
            if 0x30A1 <= code <= 0x30F6:
                out.append(chr(code - 0x60))
            else:
                out.append(ch)
        return "".join(out)

    @staticmethod
    def _has_kanji(text: str) -> bool:
        return any(0x4E00 <= ord(ch) <= 0x9FFF or ord(ch) == 0x3005 for ch in text)

    @staticmethod
    def _check_pos_match(senses: list, required_pos: set[str]) -> bool:
        if not required_pos or "base" in required_pos:
            return True
        all_pos_str = " ".join(
            str(pos).lower() for s in senses for pos in (getattr(s, "pos", []) or [])
        )
        for req in required_pos:
            if req == "v5s" and ("godan verb with 'su'" in all_pos_str or "v5s" in all_pos_str):
                return True
            if req == "v5k" and ("godan verb with 'ku'" in all_pos_str or "v5k" in all_pos_str):
                return True
            if req == "v5k-s" and ("godan verb - iku/yuku" in all_pos_str or "godan verb with 'ku'" in all_pos_str or "v5k" in all_pos_str):
                return True
            if req == "v5g" and ("godan verb with 'gu'" in all_pos_str or "v5g" in all_pos_str):
                return True
            if req == "v5t" and ("godan verb with 'tsu'" in all_pos_str or "v5t" in all_pos_str):
                return True
            if req == "v5u" and ("godan verb with 'u'" in all_pos_str or "v5u" in all_pos_str):
                return True
            if req == "v5r" and ("godan verb with 'ru'" in all_pos_str or "v5r" in all_pos_str):
                return True
            if req == "v5m" and ("godan verb with 'mu'" in all_pos_str or "v5m" in all_pos_str):
                return True
            if req == "v5b" and ("godan verb with 'bu'" in all_pos_str or "v5b" in all_pos_str):
                return True
            if req == "v5n" and ("godan verb with 'nu'" in all_pos_str or "v5n" in all_pos_str):
                return True
            if req == "v1" and ("ichidan verb" in all_pos_str or "v1" in all_pos_str):
                return True
            if req == "vs-i" and ("suru verb - included" in all_pos_str or "suru verb - irregular" in all_pos_str or "vs-i" in all_pos_str or "suru verb" in all_pos_str):
                return True
            if req == "vs" and ("takes the aux. verb suru" in all_pos_str or "suru verb" in all_pos_str or "vs" in all_pos_str):
                return True
            if req == "vk" and ("kuru verb" in all_pos_str or "vk" in all_pos_str):
                return True
            if req == "adj-i" and ("adjective (keiyoushi)" in all_pos_str or "adj-i" in all_pos_str):
                return True
            if req == "adj-na" and ("keiyodoshi" in all_pos_str or "adj-na" in all_pos_str):
                return True
        return False

    def _score_jmdict_entry(
        self,
        entry: object,
        query_term: str,
        matched_surface: str,
        reading_hint: Optional[str],
        kanji_hint: Optional[str],
        pos1_hint: str,
        required_pos: Optional[set[str]] = None,
    ) -> int:
        """
        Score a JMdict entry so contextually appropriate and common entries rank first
        (e.g. particle 'が' ranks #1 ahead of '蛾' (moth) when clicking 'が' in '日が',
        while keeping '蛾' and other homophones in the list for L/R cycling).
        """
        score = 0
        kanji_forms = getattr(entry, "kanji_forms", []) or []
        kana_forms = getattr(entry, "kana_forms", []) or []
        senses = getattr(entry, "senses", []) or []

        # 0. Surface match length bonus (longer surface text explanations rank higher)
        score += len(matched_surface) * 60

        # 1. Exact surface/query kanji match vs kanji hint match
        if self._has_kanji(query_term) and any(kf.text == query_term for kf in kanji_forms):
            score += 550
        elif self._has_kanji(matched_surface) and any(
            kf.text == matched_surface or matched_surface.startswith(kf.text) for kf in kanji_forms
        ):
            score += 500
        elif kanji_hint and any(kf.text == kanji_hint for kf in kanji_forms):
            score += 450

        # 2. Primary vs secondary reading match
        target_hira = self._katakana_to_hiragana(reading_hint or query_term)
        primary_kana_match = False
        if kana_forms:
            first_kana_hira = self._katakana_to_hiragana(kana_forms[0].text)
            if first_kana_hira == target_hira or kana_forms[0].text == query_term:
                primary_kana_match = True
                score += 200
            elif any(self._katakana_to_hiragana(kf.text) == target_hira for kf in kana_forms[1:]):
                score -= 100

        # 3. Pure-kana surface preference for kana-only / usually-kana entries (only if primary reading matches)
        all_misc = [str(m).lower() for s in senses for m in (getattr(s, "misc", []) or [])]
        is_uk = any("usually written using kana alone" in m or m == "uk" for m in all_misc)
        if not self._has_kanji(matched_surface) and primary_kana_match:
            if not kanji_forms:
                score += 320
            elif is_uk:
                score += 260

        # Archaic / obsolete / rare penalty (prevents obscure archaisms like 弥 or 為【す】 from eclipsing common words)
        all_pos_check = " ".join(str(p).lower() for s in senses for p in (getattr(s, "pos", []) or []))
        if (
            any(m in ("archaism", "arch", "obsolete term", "obs", "rare", "obscure term", "obsc") for m in all_misc)
            or "precursor to the modern suru" in all_pos_check
        ):
            score -= 380

        # 4. POS alignment (strict requirement if coming from deinflection rule, else MeCab POS mapping)
        all_pos_str = " ".join(
            str(p).lower() for s in senses for p in (getattr(s, "pos", []) or [])
        )
        if required_pos and not ("base" in required_pos):
            if self._check_pos_match(senses, required_pos):
                score += 450
            else:
                score -= 350
        else:
            pos_map = {
                "助詞": ("particle",),
                "助動詞": ("auxiliary", "suffix", "copula"),
                "接続詞": ("conjunction",),
                "動詞": ("verb",),
                "形容詞": ("adjective",),
                "形状詞": ("adjectival", "adjective"),
                "副詞": ("adverb",),
                "名詞": ("noun", "pronoun"),
                "代名詞": ("pronoun",),
                "感動詞": ("interjection", "expressions"),
                "連体詞": ("pre-noun adjectival",),
                "接頭辞": ("prefix",),
                "接尾辞": ("suffix",),
            }
            expected_pos_keywords = pos_map.get(pos1_hint, ())
            if expected_pos_keywords:
                if any(kw in all_pos_str for kw in expected_pos_keywords):
                    score += 220

        # Suru verb bonus
        if pos1_hint == "動詞" or (required_pos and "vs" in required_pos):
            if "takes the aux. verb suru" in all_pos_str or "suru verb" in all_pos_str:
                score += 300

        # 5. JMdict frequency / priority tags (ichi1, spec1, news1, gai1, etc.)
        pri_tags: set[str] = set()
        for kf in kanji_forms:
            if not kanji_hint or kf.text == kanji_hint or kf.text == query_term:
                pri_tags.update(str(p) for p in (getattr(kf, "pri", []) or []))
        for kn in kana_forms:
            if self._katakana_to_hiragana(kn.text) == target_hira or kn.text == query_term:
                pri_tags.update(str(p) for p in (getattr(kn, "pri", []) or []))

        if any(p in pri_tags for p in ("ichi1", "spec1", "news1", "gai1")):
            score += 160
        elif any(p in pri_tags for p in ("ichi2", "spec2", "news2", "gai2")) or any(
            p.startswith("nf") for p in pri_tags
        ):
            score += 85

        return score

    def lookup_at_cursor_all(
        self,
        screen_img: Image.Image,
        paragraphs: List[StitchedParagraph],
        x: int,
        y: int,
        allow_fallback_ocr: bool = True,
        allow_short_tying_ocr: bool = True,
    ) -> List[DictionaryResult]:
        """
        Resolve a cursor click at (x, y) against the screen's stitched multi-line paragraphs
        and return a ranked list of all matching DictionaryResults (for L/R cycling).
        """
        with self._lock:
            best_para: Optional[StitchedParagraph] = None
            best_char_idx: int = 0
            best_dist = float("inf")

            for para in paragraphs:
                for idx, ch_span in enumerate(para.chars):
                    d = ch_span.distance_to(float(x), float(y))
                    if d < best_dist:
                        best_dist = d
                        best_para = para
                        best_char_idx = idx

            if best_para is None or best_dist > MAX_CLICK_DISTANCE_PX:
                fallback_para = (
                    self._fallback_direct_crop_paragraph(screen_img, x, y)
                    if allow_fallback_ocr
                    else None
                )
                if fallback_para is not None:
                    paragraphs.append(fallback_para)
                    best_para = fallback_para
                    best_char_idx = 0
                    if fallback_para.chars:
                        best_char_idx = min(
                            range(len(fallback_para.chars)),
                            key=lambda i: fallback_para.chars[i].distance_to(float(x), float(y)),
                        )
                else:
                    return [
                        DictionaryResult(
                            matched_surface="",
                            context_text="(No text at cursor)",
                            headword="認識失敗",
                            reading="",
                            pos="[OCR]",
                            glosses=["No Japanese text detected near the clicked coordinates."],
                            found=False,
                        )
                    ]

            # Yomitan-style forward scan: strictly starting at the clicked character (`best_char_idx`).
            # We never look backward before `best_char_idx` to see if it was part of an earlier word.
            full_text = best_para.full_text
            anchor_idx = best_char_idx

            # If an existing paragraph morpheme begins exactly at anchor_idx, inherit its context;
            # otherwise analyze morphemes starting from anchor_idx ahead.
            first_morph: Optional[MorphemeSpan] = None
            if best_para.morphemes:
                for pm in best_para.morphemes:
                    if pm.start_idx == anchor_idx:
                        first_morph = pm
                        break

            if first_morph is not None:
                pos1_hint = first_morph.pos1
                sub_morphemes = [
                    m for m in best_para.morphemes if m.start_idx >= anchor_idx
                ]
            else:
                sub_text = full_text[anchor_idx:]
                sub_morphemes = self._analyze_morphemes(sub_text)
                first_morph = sub_morphemes[0] if sub_morphemes else None
                pos1_hint = first_morph.pos1 if first_morph else ""

            morph_len = (
                (first_morph.end_idx - first_morph.start_idx)
                if first_morph is not None
                else 1
            )
            morph_end_in_full = anchor_idx + morph_len

            candidates: List[Tuple[str, Optional[str], Optional[str], str, int, int, List[str], Optional[set[str]], Optional[str]]] = []
            seen_cand: Dict[Tuple[str, int, int, Optional[str]], int] = {}

            def add_candidate(
                term: str,
                reading: Optional[str],
                kanji_hint: Optional[str],
                p_hint: str,
                s_idx: int,
                e_idx: int,
                c_steps: Optional[List[str]] = None,
                r_pos: Optional[set[str]] = None,
                alt_surface: Optional[str] = None,
            ) -> None:
                if not term:
                    return
                # Yomitan forward-only rule: candidate must start at or after anchor_idx
                if s_idx < anchor_idx:
                    return
                key = (term, s_idx, e_idx, alt_surface)
                if key not in seen_cand:
                    seen_cand[key] = len(candidates)
                    candidates.append((term, reading, kanji_hint, p_hint, s_idx, e_idx, list(c_steps or []), r_pos, alt_surface))
                else:
                    idx_c = seen_cand[key]
                    old_t, old_r, old_k, old_p, old_s, old_e, old_cs, old_rp, old_as = candidates[idx_c]
                    candidates[idx_c] = (
                        old_t,
                        old_r or reading,
                        old_k or kanji_hint,
                        old_p or p_hint,
                        old_s,
                        old_e,
                        old_cs or list(c_steps or []),
                        old_rp if old_rp is not None else r_pos,
                        old_as or alt_surface,
                    )

            # 0. Exact & voicing-fuzzy longest-prefix check in custom/Pokémon dictionary
            #    (handles multi-token Katakana names and 1px dakuten/handakuten confusions like アプリー -> アブリー)
            max_len = min(16, len(full_text) - anchor_idx)
            if self._custom_dict:
                for length in range(max_len, 1, -1):
                    sub = full_text[anchor_idx : anchor_idx + length]
                    if sub in self._custom_dict:
                        add_candidate(sub, None, None, "名詞", anchor_idx, anchor_idx + length)
                    elif length >= 3 and all("\u30a1" <= c <= "\u30fc" for c in sub):
                        v_matches = self._custom_voicing_index.get(self._katakana_voicing_key(sub))
                        if v_matches:
                            for vm in v_matches:
                                add_candidate(vm, None, None, "名詞", anchor_idx, anchor_idx + length)

            # 1. Deinflected root lemma & kanji lemma of the full morpheme starting at `anchor_idx` FIRST
            #    so `seen_idseq` claims the full conjugated span (e.g. '感謝しております') before shorter prefixes ('感謝して').
            if first_morph is not None:
                fm_lemma = first_morph.lemma
                fm_kanji = first_morph.kanji_lemma
                if first_morph.surface in {"に", "で"} and (fm_lemma in {"だ", "なり"} or fm_kanji in {"だ", "なり"}):
                    fm_lemma = first_morph.surface
                    fm_kanji = first_morph.surface
                add_candidate(
                    fm_lemma,
                    first_morph.reading,
                    fm_kanji,
                    pos1_hint,
                    anchor_idx,
                    morph_end_in_full,
                    first_morph.conjugation_steps,
                    {"vs"} if "suru" in first_morph.conjugation_steps else None,
                )
                if "suru" in first_morph.conjugation_steps:
                    add_candidate(
                        fm_lemma + "する",
                        first_morph.reading,
                        fm_kanji,
                        "動詞",
                        anchor_idx,
                        morph_end_in_full,
                        first_morph.conjugation_steps,
                        {"vs", "vs-i", "v1"},
                    )
                if fm_kanji and fm_kanji != fm_lemma:
                    add_candidate(
                        fm_kanji,
                        first_morph.reading,
                        fm_kanji,
                        pos1_hint,
                        anchor_idx,
                        morph_end_in_full,
                        first_morph.conjugation_steps,
                    )
                add_candidate(
                    first_morph.surface,
                    first_morph.reading,
                    first_morph.kanji_lemma,
                    pos1_hint,
                    anchor_idx,
                    morph_end_in_full,
                    first_morph.conjugation_steps,
                )
                for exp_term, _exp_note in self._expand_colloquial_forms(first_morph.surface):
                    add_candidate(
                        exp_term,
                        None,
                        None,
                        pos1_hint,
                        anchor_idx,
                        morph_end_in_full,
                    )

            # 2 & 3. Multi-morpheme compound check + Yomitan recursive rule-based forward deinflection on prefixes
            valid_morph_ends = {
                (m.end_idx - anchor_idx) if (m.start_idx >= anchor_idx and anchor_idx > 0) else m.end_idx
                for m in sub_morphemes
            }
            is_compound_candidate = (
                first_morph is not None
                and pos1_hint != "助動詞"
            )
            for length in range(max_len, 1, -1):
                sub = full_text[anchor_idx : anchor_idx + length]
                if not JP_CHAR_RE.fullmatch(sub):
                    continue
                if sub in self._jmd_known_terms or (self._custom_dict and sub in self._custom_dict):
                    add_candidate(sub, None, None, "", anchor_idx, anchor_idx + length)
                elif is_compound_candidate and length > morph_len:
                    if not valid_morph_ends or length in valid_morph_ends:
                        add_candidate(sub, None, None, "", anchor_idx, anchor_idx + length)

                # Polite honorific prefix de-prefixing (e.g. 'お手入れ' -> '手入れ', 'お薬' -> '薬', 'お守り' -> '守り')
                if sub.startswith(("お", "ご")) and len(sub) >= 3:
                    sub_stem = sub[1:]
                    if sub_stem in self._jmd_known_terms or (self._custom_dict and sub_stem in self._custom_dict):
                        add_candidate(sub_stem, None, None, "接頭辞+名詞", anchor_idx, anchor_idx + length)
                deinf_cands = self._deinflector.deinflect(sub)
                for dc in deinf_cands:
                    add_candidate(
                        dc.root,
                        None,
                        None,
                        pos1_hint,
                        anchor_idx,
                        anchor_idx + length,
                        dc.steps,
                        dc.required_pos,
                    )
                    if "vs" in dc.required_pos or "suru" in dc.steps:
                        add_candidate(
                            dc.root + "する",
                            None,
                            None,
                            "動詞",
                            anchor_idx,
                            anchor_idx + length,
                            dc.steps,
                            {"vs", "vs-i", "v1"},
                        )

            # 3. Shorter prefixes starting at `anchor_idx` (plus colloquial contraction expansions)
            for length in range(min(6, max_len, max(1, morph_len - 1)), 1, -1):
                sub = full_text[anchor_idx : anchor_idx + length]
                if JP_CHAR_RE.fullmatch(sub):
                    add_candidate(sub, None, None, "", anchor_idx, anchor_idx + length)
                    for exp_term, _exp_note in self._expand_colloquial_forms(sub):
                        add_candidate(exp_term, None, None, "", anchor_idx, anchor_idx + length)

            # 4. If `anchor_idx` points to non-Japanese noise/digit/symbol (e.g. '3', '・', '「')
            #    advance to the first genuine Japanese character so clicking line starts or border artifacts
            #    seamlessly finds the actual starting Japanese word.
            next_jp_idx = anchor_idx
            while next_jp_idx < len(full_text) and not JP_CHAR_RE.fullmatch(full_text[next_jp_idx]):
                next_jp_idx += 1

            if next_jp_idx > anchor_idx and next_jp_idx < len(full_text):
                sub_jp_len = min(16, len(full_text) - next_jp_idx)
                for length in range(sub_jp_len, 1, -1):
                    sub = full_text[next_jp_idx : next_jp_idx + length]
                    if self._custom_dict and sub in self._custom_dict:
                        add_candidate(sub, None, None, "名詞", next_jp_idx, next_jp_idx + length)
                    if not JP_CHAR_RE.fullmatch(sub):
                        continue
                    if sub in self._jmd_known_terms:
                        add_candidate(sub, None, None, "", next_jp_idx, next_jp_idx + length)
                    for dc in self._deinflector.deinflect(sub):
                        add_candidate(
                            dc.root,
                            None,
                            None,
                            "",
                            next_jp_idx,
                            next_jp_idx + length,
                            dc.steps,
                            dc.required_pos,
                        )
                add_candidate(
                    full_text[next_jp_idx : next_jp_idx + 1],
                    None,
                    None,
                    "",
                    next_jp_idx,
                    next_jp_idx + 1,
                )

            # 5. Single character at `anchor_idx` as final fallback
            add_candidate(
                full_text[anchor_idx : anchor_idx + 1],
                None,
                None,
                pos1_hint if morph_len == 1 else "",
                anchor_idx,
                anchor_idx + 1,
            )

            # 6. Tying visual alternatives (when context was the decider on short phrases <= 2 chars)
            tying_options: List[str] = []
            if best_para is not None:
                if anchor_idx in getattr(best_para, "alt_phrases", {}):
                    for at in best_para.alt_phrases[anchor_idx]:
                        if at not in tying_options:
                            tying_options.append(at)
                if anchor_idx < len(best_para.chars) and getattr(best_para.chars[anchor_idx], "alt_texts", None):
                    for at in best_para.chars[anchor_idx].alt_texts:
                        if at not in tying_options:
                            tying_options.append(at)

            # If no precomputed tying options, and cursor is at a <= 2 char phrase/token, run on-demand check
            # (skip when Gemini already performed full-context OCR or during batch screen precomputation).
            if (
                allow_short_tying_ocr
                and not tying_options
                and best_para is not None
                and best_para.ocr_source != "gemini"
                and best_para.chars
                and anchor_idx < len(best_para.chars)
            ):
                cand_len = len(first_morph.surface) if first_morph else 1
                if 1 <= cand_len <= 2 and (anchor_idx + cand_len) <= len(best_para.chars):
                    target_spans = best_para.chars[anchor_idx : anchor_idx + cand_len]
                    sub_bx1 = int(min(cs.bbox[0] for cs in target_spans))
                    sub_by1 = int(min(cs.bbox[1] for cs in target_spans))
                    sub_bx2 = int(max(cs.bbox[2] for cs in target_spans))
                    sub_by2 = int(max(cs.bbox[3] for cs in target_spans))
                    sub_text = full_text[anchor_idx : anchor_idx + cand_len]
                    found_alts = self._detect_short_phrase_tying_options(
                        screen_img, (sub_bx1, sub_by1, sub_bx2, sub_by2), sub_text
                    )
                    if found_alts:
                        # Allow same-length or shorter candidates (e.g. 闇に -> 闇), but prevent
                        # matching to longer hallucinated phrases (e.g. 先 -> 先生)
                        valid_alts = [at for at in found_alts if 1 <= len(at) <= cand_len]
                        if valid_alts:
                            best_para.chars[anchor_idx].alt_texts = valid_alts
                            for at in valid_alts:
                                if at not in tying_options:
                                    tying_options.append(at)

            if tying_options:
                for alt_phrase in tying_options:
                    # A. Full tying phrase itself
                    add_candidate(alt_phrase, None, None, "", anchor_idx, anchor_idx + len(alt_phrase), alt_surface=alt_phrase)
                    for dc in self._deinflector.deinflect(alt_phrase):
                        add_candidate(
                            dc.root,
                            None,
                            None,
                            "",
                            anchor_idx,
                            anchor_idx + len(alt_phrase),
                            dc.steps,
                            dc.required_pos,
                            alt_surface=alt_phrase,
                        )
                    # B. Morphological breakdown (e.g. '闇' [noun] from '闇に' [noun + particle])
                    # Skip auxiliary verbs and grammatical particles (like 'に', 'を', 'が')
                    alt_morphs = self._analyze_morphemes(alt_phrase)
                    if alt_morphs:
                        for am in alt_morphs:
                            if am.pos1 in {"助詞", "助動詞"}:
                                continue
                            if am.lemma:
                                add_candidate(
                                    am.lemma,
                                    am.reading,
                                    am.kanji_lemma,
                                    am.pos1,
                                    anchor_idx,
                                    anchor_idx + len(am.surface),
                                    am.conjugation_steps,
                                    alt_surface=alt_phrase if len(alt_phrase) <= 2 else am.surface,
                                )
                            if am.surface and am.surface != am.lemma:
                                add_candidate(
                                    am.surface,
                                    am.reading,
                                    am.kanji_lemma,
                                    am.pos1,
                                    anchor_idx,
                                    anchor_idx + len(am.surface),
                                    am.conjugation_steps,
                                    alt_surface=alt_phrase if len(alt_phrase) <= 2 else am.surface,
                                )
                    # C. Individual kanji characters if <= 2 chars (e.g. '闇')
                    if len(alt_phrase) <= 2:
                        for c_idx, ch in enumerate(alt_phrase):
                            if "\u4e00" <= ch <= "\u9fff":
                                add_candidate(
                                    ch,
                                    None,
                                    None,
                                    "",
                                    anchor_idx + c_idx,
                                    anchor_idx + c_idx + 1,
                                    alt_surface=ch,
                                )

            results: List[DictionaryResult] = []
            name_results: List[DictionaryResult] = []
            seen_idseq: set[object] = set()
            jmd_cache: Dict[str, object] = getattr(self, "_jmd_cache", None)
            if jmd_cache is None:
                jmd_cache = {}
                self._jmd_cache = jmd_cache

            # Check dynamic Player / Trainer Name from anki_config.json once per click
            cfg = _load_anki_config()
            p_jp = str(cfg.get("player_name", "") or "").strip()
            p_en = str(cfg.get("player_english_name", "") or "").strip()

            for query_term, reading_hint, kanji_hint, p_hint, s_idx, e_idx, c_steps, r_pos, alt_surface in candidates:
                matched_surface = alt_surface or full_text[s_idx:e_idx]
                word_bboxes = [
                    best_para.chars[i].bbox
                    for i in range(s_idx, min(e_idx, len(best_para.chars)))
                ]
                ctx_text = full_text

                if p_jp and query_term == p_jp:
                    t_key = ("trainer", p_jp, "Proper Noun (Trainer Name)", (p_en,))
                    if t_key not in seen_idseq:
                        seen_idseq.add(t_key)
                        results.append(
                            DictionaryResult(
                                matched_surface=matched_surface,
                                context_text=ctx_text,
                                headword=p_jp,
                                reading=p_jp,
                                pos="Proper Noun (Trainer Name)",
                                glosses=[f"1. {p_en} (Configured Player Trainer Name)"],
                                word_bboxes=word_bboxes,
                                found=True,
                            )
                        )

                # A. Check Custom / Pokémon Dictionary (`custom_dict.json`)
                if query_term in self._custom_dict:
                    for c_ent in self._custom_dict[query_term]:
                        c_key = ("custom", c_ent["headword"], c_ent["pos"], tuple(c_ent["glosses"]))
                        if c_key in seen_idseq:
                            continue
                        seen_idseq.add(c_key)
                        results.append(
                            DictionaryResult(
                                matched_surface=matched_surface,
                                context_text=ctx_text,
                                headword=c_ent["headword"],
                                reading=c_ent["reading"],
                                pos=c_ent["pos"],
                                glosses=list(c_ent["glosses"]),
                                word_bboxes=word_bboxes,
                                found=True,
                            )
                        )

                # B. Check JMdict (`res.entries`) and JMnedict (`res.names`)
                if self._jamdict is not None:
                    known_terms = getattr(self, "_jmd_known_terms", None)
                    has_non_hira_q = any(not (0x3041 <= ord(c) <= 0x3096) for c in matched_surface)
                    want_ne = (len(query_term) >= 2 and has_non_hira_q)
                    cache_key = (query_term, want_ne)
                    if cache_key in jmd_cache:
                        res = jmd_cache[cache_key]
                    elif known_terms and not want_ne and (query_term not in known_terms):
                        res = None
                    else:
                        try:
                            res = self._jamdict.lookup(
                                query_term,
                                lookup_chars=False,
                                lookup_ne=want_ne,
                            )
                            if len(jmd_cache) > 4096:
                                jmd_cache.clear()
                            jmd_cache[cache_key] = res
                        except Exception as j_err:
                            logger.warning("Jamdict lookup failed for query %r: %s", query_term, j_err)
                            res = None
                    if res and res.entries:
                        valid_entries = (
                            [ent for ent in res.entries if self._check_pos_match(ent.senses, r_pos)]
                            if (r_pos and "base" not in r_pos)
                            else res.entries
                        )
                        # If querying by kanji_hint for a pure-kana surface with a known reading_hint
                        # (e.g. 'おります' -> reading 'オリ', kanji_hint '居る'), exclude kanji homographs
                        # whose readings do not match (such as '居る 【いる】' when the verb is '居る 【おる】').
                        if (
                            kanji_hint
                            and query_term == kanji_hint
                            and reading_hint
                            and not self._has_kanji(matched_surface)
                        ):
                            target_hira_prefix = self._katakana_to_hiragana(reading_hint)
                            filtered_by_reading = [
                                ent
                                for ent in valid_entries
                                if any(
                                    self._katakana_to_hiragana(kf.text).startswith(target_hira_prefix[:1])
                                    for kf in (getattr(ent, "kana_forms", []) or [])
                                )
                            ]
                            if filtered_by_reading:
                                valid_entries = filtered_by_reading
                        scored_entries = sorted(
                            valid_entries,
                            key=lambda ent: self._score_jmdict_entry(
                                ent, query_term, matched_surface, reading_hint, kanji_hint, p_hint, r_pos
                            ),
                            reverse=True,
                        )

                        for entry in scored_entries:
                            ent_id = ("jmd", getattr(entry, "idseq", id(entry)))
                            if ent_id in seen_idseq:
                                continue
                            seen_idseq.add(ent_id)

                            headword = entry.kanji_forms[0].text if entry.kanji_forms else query_term
                            if entry.kanji_forms:
                                matched_kf = None
                                # 1. Exact match with query_term (e.g. '飛び降りる')
                                for kf in entry.kanji_forms:
                                    if kf.text == query_term:
                                        matched_kf = kf.text
                                        break
                                # 2. Match with matched_surface or prefix (e.g. '飛び降りると' starts with '飛び降りる')
                                if not matched_kf and matched_surface:
                                    for kf in entry.kanji_forms:
                                        if kf.text == matched_surface or matched_surface.startswith(kf.text):
                                            matched_kf = kf.text
                                            break
                                # 3. Match against kanji_hint if supplied (e.g. pure-kana surface matching UniDic lemma)
                                if not matched_kf and kanji_hint:
                                    for kf in entry.kanji_forms:
                                        if kf.text == kanji_hint:
                                            matched_kf = kf.text
                                            break
                                if matched_kf:
                                    headword = matched_kf

                            reading = (
                                entry.kana_forms[0].text
                                if entry.kana_forms
                                else (reading_hint or "")
                            )
                            if len(getattr(entry, "kana_forms", []) or []) > 1 and reading_hint:
                                target_hira = self._katakana_to_hiragana(reading_hint)
                                for knf in entry.kana_forms:
                                    if self._katakana_to_hiragana(knf.text) == target_hira:
                                        reading = knf.text
                                        break

                            pos_notes: List[str] = []
                            gloss_list: List[str] = []
                            for idx, sense in enumerate(entry.senses, start=1):
                                for p in getattr(sense, "pos", []) or []:
                                    ps = str(p)
                                    if ps and ps not in pos_notes:
                                        pos_notes.append(ps)
                                for m in getattr(sense, "misc", []) or []:
                                    ms = str(m)
                                    if ms and ms not in pos_notes:
                                        pos_notes.append(ms)
                                sense_glosses = ", ".join(str(g) for g in sense.gloss)
                                sense_info = "; ".join(str(inf) for inf in (getattr(sense, "info", []) or []))
                                if sense_glosses:
                                    if sense_info:
                                        gloss_list.append(f"{idx}. {sense_glosses} ({sense_info})")
                                    else:
                                        gloss_list.append(f"{idx}. {sense_glosses}")

                            base_pos = ", ".join(pos_notes) if pos_notes else "Word"
                            active_steps = c_steps or (
                                first_morph.conjugation_steps
                                if first_morph
                                and s_idx == anchor_idx
                                and e_idx == morph_end_in_full
                                and query_term in (first_morph.lemma, first_morph.kanji_lemma)
                                else []
                            )
                            if active_steps:
                                conj_chain = " -> ".join(active_steps)
                                full_pos = f"[{conj_chain}] {base_pos}"
                            else:
                                full_pos = base_pos

                            results.append(
                                DictionaryResult(
                                    matched_surface=matched_surface,
                                    context_text=ctx_text,
                                    headword=headword,
                                    reading=reading,
                                    pos=full_pos,
                                    glosses=gloss_list or ["(No English glosses listed)"],
                                    word_bboxes=word_bboxes,
                                    found=True,
                                    rank_jlpt=self._word_meta_mgr.get_rank_and_jlpt(headword, reading, entry),
                                )
                            )

                    # Include JMnedict proper names for multi-character Katakana/Kanji terms
                    # (never turning pure-Hiragana grammar like 'どうだ' into obscure Kanji surnames),
                    # and append them after all standard JMdict vocabulary entries.
                    has_non_hiragana = any(not (0x3041 <= ord(c) <= 0x3096) for c in matched_surface)
                    if res and getattr(res, "names", None) and len(query_term) >= 2 and has_non_hiragana:
                        for ne in res.names[:5]:
                            ne_id = ("jmn", getattr(ne, "idseq", id(ne)))
                            if ne_id in seen_idseq:
                                continue
                            seen_idseq.add(ne_id)
                            hw = ne.kanji_forms[0].text if ne.kanji_forms else query_term
                            if ne.kanji_forms:
                                for kf in ne.kanji_forms:
                                    if kf.text == query_term or (matched_surface and (kf.text == matched_surface or matched_surface.startswith(kf.text))):
                                        hw = kf.text
                                        break
                            rd = ne.kana_forms[0].text if ne.kana_forms else (reading_hint or "")
                            ntypes: List[str] = []
                            nglosses: List[str] = []
                            for idx, sense in enumerate(ne.senses, start=1):
                                for nt in getattr(sense, "name_type", []) or []:
                                    nts = str(nt)
                                    if nts and nts not in ntypes:
                                        ntypes.append(nts)
                                sg = ", ".join(str(g) for g in sense.gloss)
                                if sg:
                                    nglosses.append(f"{idx}. {sg}")
                            if nglosses:
                                name_results.append(
                                    DictionaryResult(
                                        matched_surface=matched_surface,
                                        context_text=ctx_text,
                                        headword=hw,
                                        reading=rd,
                                        pos=f"[Name] {', '.join(ntypes)}" if ntypes else "[Name]",
                                        glosses=nglosses,
                                        word_bboxes=word_bboxes,
                                        found=True,
                                    )
                                )

                # C. Check External Yomitan Dictionaries (e.g. Babylon Japanese-English)
                if self._yomitan_mgr is not None:
                    y_entries = self._yomitan_mgr.lookup(query_term)
                    if not y_entries and kanji_hint:
                        y_entries = self._yomitan_mgr.lookup(kanji_hint)
                    if not y_entries and matched_surface and matched_surface != query_term:
                        y_entries = self._yomitan_mgr.lookup(matched_surface)

                    deinf_steps_by_reading: Dict[str, List[str]] = {}
                    if y_entries:
                        expected_readings: set[str] = set()
                        if not self._has_kanji(query_term):
                            expected_readings.add(self._katakana_to_hiragana(query_term))
                        if matched_surface and not self._has_kanji(matched_surface):
                            surf_hira = self._katakana_to_hiragana(matched_surface)
                            expected_readings.add(surf_hira)
                            for dc in self._deinflector.deinflect(surf_hira):
                                r_hira = self._katakana_to_hiragana(dc.root)
                                expected_readings.add(r_hira)
                                deinf_steps_by_reading.setdefault(r_hira, dc.steps)
                        if reading_hint:
                            rh_hira = self._katakana_to_hiragana(reading_hint)
                            expected_readings.add(rh_hira)
                            for dc in self._deinflector.deinflect(rh_hira):
                                r_hira = self._katakana_to_hiragana(dc.root)
                                expected_readings.add(r_hira)
                                deinf_steps_by_reading.setdefault(r_hira, dc.steps)

                        if expected_readings:
                            reading_matched = [
                                ye
                                for ye in y_entries
                                if ye.reading and self._katakana_to_hiragana(ye.reading) in expected_readings
                            ]
                            if reading_matched:
                                y_entries = reading_matched

                        y_entries = sorted(
                            y_entries,
                            key=lambda ye: 0 if ye.term == ye.headword else 1,
                        )

                    for y_ent in y_entries:
                        y_key = ("yomi", y_ent.dict_title, y_ent.headword, y_ent.reading, tuple(y_ent.glosses))
                        if y_key in seen_idseq:
                            continue
                        seen_idseq.add(y_key)

                        hw = y_ent.headword
                        if not self._has_kanji(hw) and self._has_kanji(matched_surface):
                            if matched_surface.startswith(query_term):
                                hw = query_term

                        y_active_steps = c_steps or (
                            first_morph.conjugation_steps
                            if first_morph
                            and s_idx == anchor_idx
                            and e_idx == morph_end_in_full
                            and query_term in (first_morph.lemma, first_morph.kanji_lemma)
                            else []
                        )
                        if not y_active_steps and y_ent.reading:
                            ent_hira = self._katakana_to_hiragana(y_ent.reading)
                            if (
                                matched_surface != y_ent.headword
                                and self._katakana_to_hiragana(matched_surface) != ent_hira
                            ):
                                y_active_steps = deinf_steps_by_reading.get(ent_hira, [])

                        y_pos = (
                            f"[{' -> '.join(y_active_steps)}] {y_ent.pos}"
                            if y_active_steps
                            else y_ent.pos
                        )

                        formatted_glosses = [
                            f"{idx}. {g}" if not re.match(r"^\d+\.", g) else g
                            for idx, g in enumerate(y_ent.glosses, start=1)
                        ]
                        results.append(
                            DictionaryResult(
                                matched_surface=matched_surface,
                                context_text=ctx_text,
                                headword=hw,
                                reading=y_ent.reading,
                                pos=y_pos,
                                glosses=formatted_glosses,
                                word_bboxes=word_bboxes,
                                found=True,
                            )
                        )

            # Preserve Yomitan ranking: Custom terms first, then longest matched surface prefix,
            # with JMdict beating external Yomitan dictionaries (e.g. Babylon) on ties
            yomi_tags = tuple(
                f"[{t}]" for t, _ in (self._yomitan_mgr.loaded_dicts if self._yomitan_mgr else ())
            )

            def _result_sort_key(res: DictionaryResult) -> Tuple[int, int, int]:
                is_custom = 1 if ("Custom" in res.pos or "Trainer Name" in res.pos or "Pok" in res.pos) else 0
                eff_len = len(res.matched_surface)
                is_not_external = 0 if (yomi_tags and any(tag in res.pos for tag in yomi_tags)) else 1
                return (is_custom, eff_len, is_not_external)

            results.sort(key=_result_sort_key, reverse=True)
            results.extend(name_results)
            for r in results:
                if not getattr(r, "rank_jlpt", ""):
                    r.rank_jlpt = self._word_meta_mgr.get_rank_and_jlpt(r.headword or r.matched_surface, r.reading)
            if results:
                return results

            s_idx = anchor_idx
            e_idx = morph_end_in_full if first_morph else min(len(full_text), best_char_idx + 4)
            surface = full_text[s_idx:e_idx]
            return [
                DictionaryResult(
                    matched_surface=surface,
                    context_text=full_text,
                    headword=surface[:10],
                    reading=first_morph.reading if first_morph else "",
                    pos="[Unmatched]",
                    glosses=[f"Recognized: {surface}", "No matching entry found in JMdict."],
                    word_bboxes=[
                        best_para.chars[i].bbox
                        for i in range(s_idx, min(e_idx, len(best_para.chars)))
                    ],
                    found=False,
                )
            ]

    def lookup_at_cursor_variants(
        self,
        screen_img: Image.Image,
        paragraphs_by_engine: Dict[str, List[StitchedParagraph]],
        x: int,
        y: int,
        allow_fallback_ocr: bool = True,
        allow_short_tying_ocr: bool = True,
    ) -> List[OcrVariant]:
        """
        Resolve a cursor click at `(x, y)` across all 4 OCR engines (`Manga`, `Paddle`, `Yomi`, `NDL`).
        Groups engines that agree on the recognized word/phrase, ranks variants by majority agreement
        and match quality, and annotates all `DictionaryResult` items with agreement/disagreement
        metadata so the 3DS popup can display disagreements and cycle variants via `ZL` / `ZR`.
        """
        with self._lock:
            any_near = False
            for eng in OCR_ENGINE_NAMES:
                for para in paragraphs_by_engine.get(eng, []):
                    for ch_span in para.chars:
                        if ch_span.distance_to(float(x), float(y)) <= MAX_CLICK_DISTANCE_PX:
                            any_near = True
                            break
                    if any_near:
                        break
                if any_near:
                    break

            if allow_fallback_ocr and not any_near:
                fb_by_eng = self._fallback_direct_crop_paragraphs_by_engine(screen_img, x, y)
                for eng, fb_para in fb_by_eng.items():
                    paragraphs_by_engine.setdefault(eng, []).append(fb_para)

            grouped: Dict[Tuple[str, str, bool], OcrVariant] = {}
            computed_by_para_click: Dict[Tuple[str, int], List[DictionaryResult]] = {}
            matched_gemini_flags: Dict[Tuple[str, str, bool], bool] = {}

            for eng in OCR_ENGINE_NAMES:
                eng_paras = paragraphs_by_engine.get(eng, [])
                if not eng_paras:
                    continue

                # Check which paragraph & character index this engine resolves to at (x, y)
                best_p = None
                best_c_idx = 0
                best_d = float("inf")
                for para in eng_paras:
                    for idx, ch_span in enumerate(para.chars):
                        d = ch_span.distance_to(float(x), float(y))
                        if d < best_d:
                            best_d = d
                            best_p = para
                            best_c_idx = idx

                if best_p is None or best_d > MAX_CLICK_DISTANCE_PX:
                    continue

                dedup_key = (best_p.full_text, best_c_idx)
                if dedup_key in computed_by_para_click:
                    res_list = computed_by_para_click[dedup_key]
                else:
                    res_list = self.lookup_at_cursor_all(
                        screen_img,
                        eng_paras,
                        x,
                        y,
                        allow_fallback_ocr=False,
                        allow_short_tying_ocr=allow_short_tying_ocr,
                    )
                    computed_by_para_click[dedup_key] = res_list

                if not res_list:
                    continue
                top_r = res_list[0]
                if not top_r.found and top_r.headword == "認識失敗":
                    continue
                norm_surf = (top_r.matched_surface or top_r.headword).strip()
                norm_hw = (top_r.headword or norm_surf).strip()
                v_key = (norm_surf, norm_hw, bool(top_r.found))
                if v_key not in grouped:
                    grouped[v_key] = OcrVariant(
                        engines=[eng],
                        ocr_text=norm_surf or norm_hw,
                        results=res_list,
                    )
                    matched_gemini_flags[v_key] = (best_p.ocr_source == "gemini")
                else:
                    grouped[v_key].engines.append(eng)
                    matched_gemini_flags[v_key] = matched_gemini_flags[v_key] and (best_p.ocr_source == "gemini")

            if not grouped:
                # Secondary recovery: If user tapped slightly before or after a line (e.g. at the start of a dialogue box)
                for eng in OCR_ENGINE_NAMES:
                    eng_paras = paragraphs_by_engine.get(eng, [])
                    if not eng_paras:
                        continue
                    best_p = None
                    best_d = float("inf")
                    for para in eng_paras:
                        px1, py1, px2, py2 = para.bbox
                        pcy = (py1 + py2) / 2.0
                        ph = max(10, py2 - py1)
                        if abs(y - pcy) <= max(16.0, ph * 0.85):
                            h_dist = max(px1 - x, 0.0, x - px2)
                            if h_dist < best_d and h_dist <= 50.0:
                                best_d = h_dist
                                best_p = para
                    if best_p is not None and best_p.chars:
                        c_idx = 0 if x <= (best_p.bbox[0] + best_p.bbox[2]) / 2.0 else len(best_p.chars) - 1
                        snap_x = int((best_p.chars[c_idx].bbox[0] + best_p.chars[c_idx].bbox[2]) / 2.0)
                        snap_y = int((best_p.chars[c_idx].bbox[1] + best_p.chars[c_idx].bbox[3]) / 2.0)
                        res_list = self.lookup_at_cursor_all(
                            screen_img,
                            [best_p],
                            snap_x,
                            snap_y,
                            allow_fallback_ocr=False,
                            allow_short_tying_ocr=allow_short_tying_ocr,
                        )
                        if res_list:
                            top_r = res_list[0]
                            if top_r.found or top_r.headword != "認識失敗":
                                norm_surf = (top_r.matched_surface or top_r.headword).strip()
                                norm_hw = (top_r.headword or norm_surf).strip()
                                v_key = (norm_surf, norm_hw, bool(top_r.found))
                                if v_key not in grouped:
                                    grouped[v_key] = OcrVariant(engines=[eng], ocr_text=norm_surf or norm_hw, results=res_list)
                                    matched_gemini_flags[v_key] = (best_p.ocr_source == "gemini")
                                else:
                                    grouped[v_key].engines.append(eng)
                                    matched_gemini_flags[v_key] = matched_gemini_flags[v_key] and (best_p.ocr_source == "gemini")

            if not grouped:
                fallback_res = [
                    DictionaryResult(
                        matched_surface="",
                        context_text="(No text at cursor)",
                        headword="認識失敗",
                        reading="",
                        pos="[OCR]",
                        glosses=["No Japanese text detected near the clicked coordinates."],
                        found=False,
                    )
                ]
                return [OcrVariant(engines=list(OCR_ENGINE_NAMES), ocr_text="認識失敗", results=fallback_res)]

            variants = list(grouped.values())
            variants.sort(
                key=lambda v: (
                    1 if v.results[0].found else 0,
                    len(v.engines),
                    len(v.results[0].matched_surface),
                    -OCR_ENGINE_NAMES.index(v.engines[0]),
                ),
                reverse=True,
            )

            if len(variants) == 1 and all(matched_gemini_flags.values()) and matched_gemini_flags:
                variants[0].engines = ["Gemini"]
                has_disagreement = False
                summary_line = ""
            else:
                has_disagreement = len(variants) > 1 or len(variants[0].engines) < len(OCR_ENGINE_NAMES)
                summary_parts = [
                    f"{idx + 1}:{v.ocr_text}({'+'.join(v.engines)})"
                    for idx, v in enumerate(variants)
                ]
                summary_line = (
                    f"★ OCR Diff [ZL/ZR]: {' | '.join(summary_parts)}"
                    if len(variants) > 1
                    else (
                        f"★ OCR ({'+'.join(variants[0].engines)}): {variants[0].ocr_text}"
                        if has_disagreement
                        else ""
                    )
                )

            for v_idx, v in enumerate(variants):
                for r in v.results:
                    r.ocr_disagreement = len(variants) > 1
                    r.ocr_variant_idx = v_idx
                    r.ocr_total_variants = len(variants)
                    r.ocr_engines = list(v.engines)
                    r.ocr_summary_line = summary_line

            return variants

    @staticmethod
    def _split_crop_into_horizontal_row_boxes(
        screen_img: Image.Image, bbox: Tuple[int, int, int, int]
    ) -> List[Tuple[int, int, int, int]]:
        """
        Split a bounding box `(x1, y1, x2, y2)` into individual horizontal text row boxes
        using foreground ink projection (`_compute_crop_ink_mask`), so multi-line selections
        are cleanly separated into single-line crops for MangaOCR even if DBNet merges them
        or misses small UI text.
        """
        bx1, by1, bx2, by2 = bbox
        if bx2 - bx1 < 6 or by2 - by1 < 6:
            return [bbox]
        crop_rgb = np.array(screen_img.crop((bx1, by1, bx2, by2)).convert("RGB"), dtype=np.float32)
        ink = JapaneseOCRDictionaryPipeline._compute_crop_ink_mask(crop_rgb)
        if ink is None:
            return [bbox]

        _, cw = ink.shape
        row_counts = ink.sum(axis=1)
        valid_rows = (row_counts >= 2) & (row_counts <= max(2, int(cw * 0.82)))
        ys = np.where(valid_rows)[0]
        if ys.size == 0:
            return [bbox]

        bands: List[Tuple[int, int]] = []
        bs = bp = int(ys[0])
        for yv in ys[1:]:
            yv_i = int(yv)
            if yv_i > bp + 2:
                if (bp + 1 - bs) >= 6:
                    bands.append((bs, bp + 1))
                bs = yv_i
            bp = yv_i
        if (bp + 1 - bs) >= 6:
            bands.append((bs, bp + 1))

        if not bands:
            return [bbox]

        row_boxes: List[Tuple[int, int, int, int]] = []
        for ry1, ry2 in bands:
            band_ink = ink[ry1:ry2, :]
            col_counts = band_ink.sum(axis=0)
            xs = np.where((col_counts >= 1) & (col_counts < (ry2 - ry1)))[0]
            if xs.size == 0:
                xs = np.where(col_counts >= 1)[0]
            rx1 = max(0, int(xs[0]) - 2) if xs.size > 0 else 0
            rx2 = min(cw, int(xs[-1]) + 3) if xs.size > 0 else cw
            if rx2 - rx1 >= 6:
                row_boxes.append(
                    (
                        bx1 + rx1,
                        max(by1, by1 + ry1 - 1),
                        bx1 + rx2,
                        min(by2, by1 + ry2 + 1),
                    )
                )
        return row_boxes or [bbox]

    @staticmethod
    def _extract_short_english_name(gloss_line: str) -> str:
        """Extract clean English name from a custom_dict gloss like '1. Cutiefly (Pokémon #742 ...)' or '1. Pokémon, Pocket Monsters'."""
        s = re.sub(r"^\d+\.\s*", "", gloss_line).strip()
        s = re.split(r"\s*[\(（;；/]|,\s+", s, maxsplit=1)[0].strip()
        return s

    @staticmethod
    def _compact_japanese_text(text: str) -> str:
        """
        Removes inter-word spaces (wakachigaki) and line breaks from Japanese text
        while keeping punctuation (！, ？, 。, 、, …, 「, 」, etc.) and single spaces
        between ASCII words.
        """
        if not text:
            return ""
        s = text.replace("\u3000", " ").replace("\r", "")
        def repl(m: re.Match) -> str:
            start = m.start()
            end = m.end()
            left = s[start - 1] if start > 0 else ""
            right = s[end] if end < len(s) else ""
            # If both adjacent characters are ASCII (e.g. English words, Latin names), keep single space
            if re.match(r"[\x21-\x7E]", left) and re.match(r"[\x21-\x7E]", right):
                return " "
            # Otherwise remove whitespace completely (between Japanese characters/punctuation)
            return ""
        return re.sub(r"\s+", repl, s).strip()

    @staticmethod
    def _japanese_number_to_hiragana(s: str) -> str:
        """
        Convert a numeric string (half-width or full-width digits) to natural Japanese Hiragana pronunciation:
          e.g. '1' -> 'いち', '50' -> 'ごじゅう', '100' -> 'ひゃく', '300' -> 'さんびゃく',
               '800' -> 'はっぴゃく', '1000' -> 'せん', '3000' -> 'さんぜん', '8000' -> 'はっせん',
               '10000' -> 'いちまん', '025' -> 'ぜろにご'.
        """
        s_norm = unicodedata.normalize("NFKC", str(s)).strip()
        if not s_norm or not s_norm.isdigit():
            return s

        digit_names = {
            "0": "ぜろ", "1": "いち", "2": "に", "3": "さん", "4": "よん",
            "5": "ご", "6": "ろく", "7": "なな", "8": "はち", "9": "きゅう",
        }
        # Leading zeroes (e.g. '025', '007') -> read digit-by-digit
        if len(s_norm) > 1 and s_norm.startswith("0"):
            return "".join(digit_names.get(ch, ch) for ch in s_norm)

        n = int(s_norm)
        if n == 0:
            return "ぜろ"

        digits = ["", "いち", "に", "さん", "よん", "ご", "ろく", "なな", "はち", "きゅう"]
        hundreds = {
            1: "ひゃく", 2: "にひゃく", 3: "さんびゃく", 4: "よんひゃく",
            5: "ごひゃく", 6: "ろっぴゃく", 7: "ななひゃく", 8: "はっぴゃく", 9: "きゅうひゃく",
        }
        thousands = {
            1: "せん", 2: "にせん", 3: "さんぜん", 4: "よんせん",
            5: "ごせん", 6: "ろくせん", 7: "ななせん", 8: "はっせん", 9: "きゅうせん",
        }

        def _under_10000(val: int) -> str:
            res: List[str] = []
            th = val // 1000
            val %= 1000
            if th > 0:
                res.append(thousands[th])
            h = val // 100
            val %= 100
            if h > 0:
                res.append(hundreds[h])
            t = val // 10
            val %= 10
            if t > 0:
                if t == 1:
                    res.append("じゅう")
                else:
                    res.append(digits[t] + "じゅう")
            if val > 0:
                res.append(digits[val])
            return "".join(res)

        big_units = ["", "まん", "おく", "ちょう"]
        parts: List[str] = []
        chunk_idx = 0
        cur = n
        while cur > 0:
            chunk = cur % 10000
            if chunk > 0:
                chunk_str = _under_10000(chunk)
                unit = big_units[chunk_idx] if chunk_idx < len(big_units) else ""
                if chunk_idx >= 1 and chunk == 1:
                    chunk_str = "いち"
                parts.append(chunk_str + unit)
            cur //= 10000
            chunk_idx += 1
        return "".join(reversed(parts))

    def to_hiragana_pronunciation(self, text: str) -> str:
        """
        Convert Japanese text (kanji, katakana, numbers) into pure hiragana pronunciation,
        preserving punctuation, symbols, and inter-word whitespace.
        Uses Fugashi (UniDic) kana readings with custom dictionary overrides and number pronunciation.
        """
        if not text:
            return ""

        def _kata_to_hira(s: str) -> str:
            res = []
            for c in s:
                code = ord(c)
                if 0x30A1 <= code <= 0x30F6:
                    res.append(chr(code - 0x60))
                else:
                    res.append(c)
            return "".join(res)

        if not self._tagger:
            return _kata_to_hira(text)

        clean_text = re.sub(r'(?<=\d),(?=\d{3}(?:\D|$))', '', text)
        out_parts: List[str] = []
        try:
            nodes = self._tagger(clean_text)
            for n in nodes:
                surf = n.surface
                norm_surf = unicodedata.normalize("NFKC", surf).strip()
                if norm_surf.isdigit():
                    out_parts.append(self._japanese_number_to_hiragana(norm_surf))
                    continue
                if surf in self._custom_dict:
                    c_reading = self._custom_dict[surf][0].get("reading", "")
                    if c_reading:
                        out_parts.append(_kata_to_hira(c_reading))
                        continue
                kana = getattr(n.feature, "kana", "") or getattr(n.feature, "pron", "") or ""
                if not kana:
                    if any(c.isdigit() for c in norm_surf):
                        converted = re.sub(r'\d+', lambda m: self._japanese_number_to_hiragana(m.group(0)), norm_surf)
                        out_parts.append(_kata_to_hira(converted))
                    else:
                        out_parts.append(_kata_to_hira(surf))
                else:
                    out_parts.append(_kata_to_hira(kana))
            return "".join(out_parts)
        except Exception as exc:
            logger.warning("Pronunciation generation failed for '%s': %s", text, exc)
            return _kata_to_hira(text)

    def _translate_with_gemini(
        self,
        jp_text: str,
        glossary_terms: List[Tuple[str, str, str]],
        api_key: Union[str, List[str]],
        primary_model: str,
        fallback_model: str = "",
        screen_img: Optional[Image.Image] = None,
        use_vision: bool = True,
        selection_box: Optional[Tuple[int, int, int, int]] = None,
    ) -> Optional[List[str]]:
        """Translate Japanese game dialogue using the Gemini REST API (with optional screenshot context)."""
        glossary_hint = ""
        if glossary_terms:
            glossary_hint = "Glossary: " + ", ".join(f"{j}={e}" for j, e, _ in glossary_terms) + "\n"

        text_prompt = (
            "Translate the following Japanese video game dialogue into natural, concise English. "
            "Ignore any small furigana/ruby reading annotations if present, and do not duplicate readings. "
            "Output ONLY the English translation.\n"
            f"{glossary_hint}Japanese:\n{jp_text}"
        )
        parts: List[Dict[str, Any]] = []
        used_vision_prompt = False
        if use_vision and screen_img is not None:
            used_vision_prompt = True
            box_note = " (with the selected area outlined in red)" if selection_box is not None else ""
            prompt = (
                "Translate ONLY the Japanese text given under 'Selected Japanese:' into natural, concise English. "
                f"The attached screenshot{box_note} is provided strictly for visual and surrounding context—"
                "do NOT translate unselected text from the rest of the screenshot, and do NOT expand beyond the selected fragment. "
                "Ignore any small furigana/ruby reading annotations printed above or beside kanji, and do not duplicate readings. "
                "Output ONLY the English translation of the selected text.\n"
                f"{glossary_hint}Selected Japanese:\n{jp_text}"
            )
            parts.append({"text": prompt})
            try:
                orig_w, orig_h = screen_img.size
                if selection_box is not None and orig_w > 0 and orig_h > 0:
                    bx1, by1, bx2, by2 = (
                        min(selection_box[0], selection_box[2]),
                        min(selection_box[1], selection_box[3]),
                        max(selection_box[0], selection_box[2]),
                        max(selection_box[1], selection_box[3]),
                    )
                    if by2 - by1 < 16:
                        mid_y = (by1 + by2) // 2
                        by1 = max(0, mid_y - 12)
                        by2 = min(orig_h - 1, mid_y + 12)
                    if bx2 - bx1 < 16:
                        mid_x = (bx1 + bx2) // 2
                        bx1 = max(0, mid_x - 12)
                        bx2 = min(orig_w - 1, mid_x + 12)
                    pad_x, pad_y = 24, 20
                    cx1 = max(0, bx1 - pad_x)
                    cy1 = max(0, by1 - pad_y)
                    cx2 = min(orig_w, bx2 + pad_x)
                    cy2 = min(orig_h, by2 + pad_y)
                    img_copy = screen_img.crop((cx1, cy1, cx2, cy2)).convert("RGB")
                    ImageDraw.Draw(img_copy).rectangle(
                        (bx1 - cx1, by1 - cy1, bx2 - cx1, by2 - cy1), outline=(255, 32, 32), width=2
                    )
                else:
                    img_copy = screen_img.convert("RGB")
                if max(img_copy.size) > 640:
                    img_copy.thumbnail((640, 640))
                buf = io.BytesIO()
                img_copy.save(buf, format="JPEG", quality=88)
                parts.append({
                    "inline_data": {
                        "mime_type": "image/jpeg",
                        "data": base64.b64encode(buf.getvalue()).decode("ascii"),
                    }
                })
            except Exception as img_exc:
                logger.debug("Skipping Gemini vision attachment: %s", img_exc)
        else:
            parts.append({"text": text_prompt})

        candidates: List[str] = []
        for m in (primary_model, fallback_model):
            m_clean = str(m or "").strip()
            if m_clean and m_clean not in candidates:
                candidates.append(m_clean)

        llm_out = self._call_gemini_generate_content(
            parts=parts,
            generation_config={"temperature": 0.1, "maxOutputTokens": 256},
            api_key=api_key,
            candidates=candidates,
            timeout_sec=6.0,
            op_label="translation",
            max_wall_sec=11.0,
            max_timeouts=2,
        )
        if not llm_out and used_vision_prompt:
            llm_out = self._call_gemini_generate_content(
                parts=[{"text": text_prompt}],
                generation_config={"temperature": 0.1, "maxOutputTokens": 256},
                api_key=api_key,
                candidates=candidates,
                timeout_sec=6.0,
                op_label="translation (text fallback)",
                max_wall_sec=8.0,
                max_timeouts=1,
            )
        if llm_out:
            return [l.strip() for l in llm_out.splitlines() if l.strip()]
        return None

    def translate_japanese_text(
        self,
        jp_lines: List[str],
        title: Union[int, str] = 0,
        substitute_hiragana: Optional[bool] = None,
        screen_img: Optional[Image.Image] = None,
        selection_box: Optional[Tuple[int, int, int, int]] = None,
        use_gemini: bool = True,
        is_gemini_ocr: bool = False,
    ) -> Tuple[List[str], List[Tuple[str, str, str]]]:
        """
        Translate one or more Japanese lines/sentences into English on the PC:
          1. Scans for game/Pokémon proper nouns in `self._custom_dict` and configured player
             trainer name (`anki_config.json`: `"player_name"`), substituting names before
             neural translation so e.g. 'マット' translates as 'Matt' (name) rather than 'mat',
             and 'アブリー' translates as 'Cutiefly' rather than 'Avery'.
             Title-aware: Pokémon terms (moves, items, abilities) only substitute when playing Pokémon!
             Hiragana toggle: Pure Hiragana game terms can be toggled on/off to prevent overzealous substitutions.
          2. Uses Gemini REST API if an API key is configured (`gemini_credentials.json` or `GEMINI_API_KEY`),
             with optional local Ollama LLM (`anki_config.json`: `"ollama_model"`), fast
             Google Neural MT (`client=gtx`), and offline morpheme-gloss fallback.
        Returns `(english_lines, detected_glossary_terms)`.
        """
        clean_lines = [ln.strip() for ln in jp_lines if ln and ln.strip()]
        if not clean_lines:
            return ["(No Japanese text detected in selected region.)"], []

        # 1. Detect custom / Pokémon proper nouns & player trainer name in the text
        cfg = _load_anki_config()
        player_name_jp = str(cfg.get("player_name", "") or "").strip()
        player_name_en = str(cfg.get("player_english_name", "") or "").strip()

        if substitute_hiragana is None:
            substitute_hiragana = bool(cfg.get("substitute_hiragana_terms", True))

        is_pokemon = is_pokemon_game(title)

        glossary_terms: List[Tuple[str, str, str]] = []
        seen_terms: set[str] = set()

        if player_name_jp and player_name_en:
            for ln in clean_lines:
                if player_name_jp in ln and player_name_jp not in seen_terms:
                    seen_terms.add(player_name_jp)
                    glossary_terms.append((player_name_jp, player_name_en, "Trainer Name"))

        for ln in clean_lines:
            n = len(ln)
            i = 0
            while i < n:
                matched_len = 0
                for length in range(min(12, n - i), 1, -1):
                    sub = ln[i : i + length]
                    if sub in self._custom_dict and sub not in self._BUILTIN_COLLOQUIAL_DICT:
                        ent = self._custom_dict[sub][0]
                        pos_label = ent.get("pos", "Term")
                        # Title awareness: Only detect Pokémon dictionary entries if playing a Pokémon game!
                        if pos_label.startswith("Pokémon") and not is_pokemon:
                            continue
                        en_name = self._extract_short_english_name(ent["glosses"][0]) if ent.get("glosses") else ""
                        if en_name and sub not in seen_terms:
                            seen_terms.add(sub)
                            glossary_terms.append((sub, en_name, pos_label))
                        matched_len = length
                        break
                i += max(1, matched_len)

        # Prepare pre-substituted lines for proper nouns & terms (e.g. Pokémon moves, items, species, Trainer names)
        prepped_lines: List[str] = []
        for ln in clean_lines:
            mod_ln = ln
            for jp_term, en_name, pos_label in sorted(glossary_terms, key=lambda x: len(x[0]), reverse=True):
                is_hiragana_term = all("\u3040" <= c <= "\u309f" for c in jp_term)

                # Mode toggle: Skip substituting pure Hiragana terms if substitute_hiragana is disabled
                if is_hiragana_term and not substitute_hiragana:
                    continue

                is_name_or_term = (
                    "Name" in pos_label
                    or "Trainer" in pos_label
                    or "Pokémon" in pos_label
                    or "Move" in pos_label
                    or "Item" in pos_label
                    or "Ability" in pos_label
                    or "Species" in pos_label
                    or "Nature" in pos_label
                    or (len(jp_term) >= 3 and all("\u30a1" <= c <= "\u30fc" for c in jp_term))
                )
                if is_name_or_term:
                    sub_val = en_name
                    # Common items like 'berry' produce much more natural MT as common nouns rather than capitalized person names
                    if jp_term in {"きのみ", "木の実"}:
                        sub_val = "berry"
                    mod_ln = mod_ln.replace(jp_term, f" {sub_val} ")
            prepped_lines.append(mod_ln.strip())

        # Compact dialogue text by stripping inter-word spaces and line breaks between Japanese characters,
        # preserving legitimate punctuation (！, ？, 。, 、, …, 「, 」, etc.) and single spaces around Latin words.
        query_text = self._compact_japanese_text(" ".join(prepped_lines))
        llm_text = re.sub(r"\s+", " ", " ".join(prepped_lines)).strip()

        # 2. Check Gemini REST API if key(s) are configured in `gemini_credentials.json` or `GEMINI_API_KEY`
        cfg = _load_anki_config()
        gemini_api_keys = _load_gemini_api_keys()
        if use_gemini and gemini_api_keys:
            gemini_model = str(cfg.get("gemini_model", "gemini-3.5-flash-lite") or "gemini-3.5-flash-lite").strip()
            gemini_fallback = str(cfg.get("gemini_fallback_model", "gemini-3.1-flash-lite") or "").strip()
            gemini_use_vision = bool(cfg.get("gemini_use_vision", True)) and not is_gemini_ocr
            gem_lines = self._translate_with_gemini(
                llm_text,
                glossary_terms,
                api_key=gemini_api_keys,
                primary_model=gemini_model,
                fallback_model=gemini_fallback,
                screen_img=screen_img,
                use_vision=gemini_use_vision,
                selection_box=selection_box,
            )
            if gem_lines:
                return gem_lines, glossary_terms

        # 3. Check optional local Ollama LLM if configured in `anki_config.json`
        ollama_model = str(cfg.get("ollama_model", "") or "").strip()
        if ollama_model:
            try:
                glossary_hint = ""
                if glossary_terms:
                    glossary_hint = "Glossary: " + ", ".join(f"{j}={e}" for j, e, _ in glossary_terms) + "\n"
                prompt = (
                    "Translate the following Japanese video game dialogue into natural, concise English. "
                    "Output ONLY the English translation.\n"
                    f"{glossary_hint}Japanese:\n{query_text}\nEnglish:"
                )
                payload = json.dumps({
                    "model": ollama_model,
                    "prompt": prompt,
                    "stream": False,
                }).encode("utf-8")
                req = urllib.request.Request(
                    "http://127.0.0.1:11434/api/generate",
                    data=payload,
                    headers={"Content-Type": "application/json"},
                )
                with urllib.request.urlopen(req, timeout=6.0) as resp:
                    resp_obj = json.loads(resp.read().decode("utf-8"))
                    llm_out = str(resp_obj.get("response", "")).strip()
                    if llm_out:
                        return [l.strip() for l in llm_out.splitlines() if l.strip()], glossary_terms
            except Exception as exc:
                logger.warning("Ollama translation failed (%s); falling back to Google NMT.", exc)

        # 3. Fast Google Neural MT with client rotation & browser headers
        google_clients = ["dict-chrome-ex", "gtx"]
        headers = {
            "User-Agent": (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/124.0.0.0 Safari/537.36"
            )
        }
        for g_client in google_clients:
            try:
                url = (
                    f"https://translate.googleapis.com/translate_a/single?client={g_client}&sl=ja&tl=en&dt=t&dt=qc&q="
                    + urllib.parse.quote(query_text)
                )
                req = urllib.request.Request(url, headers=headers)
                with urllib.request.urlopen(req, timeout=5.0) as resp:
                    data = json.loads(resp.read().decode("utf-8"))
                translated_raw = "".join(part[0] for part in (data[0] or []) if part and part[0])
                en_lines = [re.sub(r"\s+", " ", l).strip() for l in translated_raw.splitlines() if l.strip()]

                # Disambiguate pure-kana homophones using Google's query correction (dt=qc in data[7])
                # E.g. 'つよいなら いってください' -> '強いなら 言ってください' ("tell me/say so" vs "go")
                if len(data) > 7 and data[7] and len(data[7]) > 1:
                    qc_suggested = str(data[7][1]).strip()
                    has_orig_kanji = bool(re.search(r"[\u4E00-\u9FFF]", query_text))
                    has_qc_kanji = bool(re.search(r"[\u4E00-\u9FFF]", qc_suggested))
                    if qc_suggested and (not has_orig_kanji and has_qc_kanji):
                        try:
                            qc_url = (
                                f"https://translate.googleapis.com/translate_a/single?client={g_client}&sl=ja&tl=en&dt=t&q="
                                + urllib.parse.quote(qc_suggested)
                            )
                            qc_req = urllib.request.Request(qc_url, headers=headers)
                            with urllib.request.urlopen(qc_req, timeout=4.0) as qc_resp:
                                qc_data = json.loads(qc_resp.read().decode("utf-8"))
                            qc_tr = "".join(p[0] for p in (qc_data[0] or []) if p and p[0])
                            qc_lines = [re.sub(r"\s+", " ", l).strip() for l in qc_tr.splitlines() if l.strip()]
                            if qc_lines:
                                en_lines = qc_lines
                        except Exception as qc_err:
                            logger.debug("QC disambiguation query skipped (%s): %s", g_client, qc_err)

                # Enhance standalone short colloquial lines (like 'どうだろ……？') when needed
                for idx, orig_ln in enumerate(clean_lines):
                    core_jp = re.sub(r"[…・。、．，！？!?\s]+", "", orig_ln)
                    if core_jp in self._BUILTIN_COLLOQUIAL_DICT and idx < len(en_lines):
                        c_gloss = self._BUILTIN_COLLOQUIAL_DICT[core_jp][0]["glosses"][0]
                        short_idiom = re.sub(r"^\d+\.\s*", "", c_gloss).split(";")[0].strip()
                        if short_idiom.lower() not in en_lines[idx].lower():
                            en_lines[idx] = f"{en_lines[idx]} ({short_idiom})"

                if en_lines:
                    return en_lines, glossary_terms
            except Exception as exc:
                logger.warning("Online neural translation failed with client=%s: %s", g_client, exc)
                continue

        # 4. Offline fallback: assemble morpheme glosses using local Fugashi + JMdict
        fallback_lines: List[str] = []
        for ln in clean_lines:
            spans = self._analyze_morphemes(re.sub(r"[^\u3005\u3040-\u309F\u30A0-\u30FF\u30FC\u4E00-\u9FFF]", "", ln))
            parts: List[str] = []
            for sp in spans:
                if sp.pos1 in {"助詞", "補助記号"}:
                    continue
                if sp.surface in self._custom_dict:
                    g0 = self._extract_short_english_name(self._custom_dict[sp.surface][0]["glosses"][0])
                    parts.append(g0)
                elif self._jamdict is not None:
                    try:
                        r = self._jamdict.lookup(sp.lemma)
                    except Exception:
                        r = None
                    if r and r.entries and r.entries[0].senses:
                        parts.append(str(r.entries[0].senses[0].gloss[0]))
                    elif self._yomitan_mgr is not None and (self._yomitan_mgr.lookup(sp.surface) or self._yomitan_mgr.lookup(sp.lemma)):
                        y_res = self._yomitan_mgr.lookup(sp.surface) or self._yomitan_mgr.lookup(sp.lemma)
                        g_txt = re.sub(r"^\d+\.\s*", "", y_res[0].glosses[0]).split(";")[0].split(",")[0].strip()
                        parts.append(g_txt)
                    else:
                        parts.append(sp.surface)
            fallback_lines.append(" / ".join(parts) if parts else ln)
        return fallback_lines, glossary_terms

    def _clip_detected_line_to_selection(
        self,
        screen_img: Image.Image,
        ln: DetectedLine,
        bx1: int,
        by1: int,
        bx2: int,
        by2: int,
    ) -> Optional[DetectedLine]:
        """
        Clip a DetectedLine so it only includes characters that are at least partially
        contained within the selection window (bx1, by1, bx2, by2).
        Returns None if no characters intersect the selection window.
        """
        if not ln.text:
            return None

        line_chars = self._compute_line_char_spans(screen_img, ln)
        if not line_chars:
            lx1, ly1, lx2, ly2 = ln.bbox
            n_c = len(ln.text)
            if ln.is_vertical:
                c_h = (ly2 - ly1) / float(max(1, n_c))
                line_chars = [
                    CharSpan(ch, (float(lx1), ly1 + i * c_h, float(lx2), ly1 + (i + 1) * c_h))
                    for i, ch in enumerate(ln.text)
                ]
            else:
                c_w = (lx2 - lx1) / float(max(1, n_c))
                line_chars = [
                    CharSpan(ch, (lx1 + i * c_w, float(ly1), lx1 + (i + 1) * c_w, float(ly2)))
                    for i, ch in enumerate(ln.text)
                ]

        overlapping_indices: List[int] = []
        for idx, cs in enumerate(line_chars):
            cx1, cy1, cx2, cy2 = cs.bbox
            c_ov_w = min(bx2, cx2) - max(bx1, cx1)
            c_ov_h = min(by2, cy2) - max(by1, cy1)
            if c_ov_w > 0 and c_ov_h > 0:
                overlapping_indices.append(idx)

        if not overlapping_indices:
            return None

        s_idx = min(overlapping_indices)
        e_idx = max(overlapping_indices) + 1

        sub_spans = line_chars[s_idx:e_idx]
        sub_x1 = int(min(cs.bbox[0] for cs in sub_spans))
        sub_y1 = int(min(cs.bbox[1] for cs in sub_spans))
        sub_x2 = int(max(cs.bbox[2] for cs in sub_spans))
        sub_y2 = int(max(cs.bbox[3] for cs in sub_spans))

        clipped_text = ln.text[s_idx:e_idx]
        if not clipped_text:
            return None

        clipped_eng_texts: Dict[str, str] = {}
        clipped_eng_raw: Dict[str, str] = {}
        for eng in OCR_ENGINE_NAMES:
            e_txt = (ln.engine_texts.get(eng) if ln.engine_texts else None) or ln.text
            if e_txt:
                if len(e_txt) == len(ln.text):
                    clipped_eng_texts[eng] = e_txt[s_idx:e_idx]
                else:
                    s_f = s_idx / float(len(ln.text))
                    e_f = e_idx / float(len(ln.text))
                    es = max(0, min(len(e_txt), int(round(s_f * len(e_txt)))))
                    ee = max(es + 1, min(len(e_txt), int(round(e_f * len(e_txt)))))
                    clipped_eng_texts[eng] = e_txt[es:ee]

            e_raw = (ln.engine_raw_texts.get(eng) if ln.engine_raw_texts else None) or e_txt
            if e_raw:
                if len(e_raw) == len(ln.text):
                    clipped_eng_raw[eng] = e_raw[s_idx:e_idx]
                else:
                    s_f = s_idx / float(len(ln.text))
                    e_f = e_idx / float(len(ln.text))
                    es = max(0, min(len(e_raw), int(round(s_f * len(e_raw)))))
                    ee = max(es + 1, min(len(e_raw), int(round(e_f * len(e_raw)))))
                    clipped_eng_raw[eng] = e_raw[es:ee]

        return DetectedLine(
            bbox=(sub_x1, sub_y1, sub_x2, sub_y2),
            text=clipped_text,
            is_vertical=ln.is_vertical,
            has_leading_punct=(s_idx == 0 and ln.has_leading_punct),
            has_trailing_punct=(e_idx == len(ln.text) and ln.has_trailing_punct),
            raw_text=clipped_eng_raw.get("Manga", clipped_text),
            engine_texts=clipped_eng_texts,
            engine_raw_texts=clipped_eng_raw,
            alt_texts=list(ln.alt_texts),
            ocr_source=ln.ocr_source,
        )

    def _extract_region_lines_by_engine(
        self, screen_img: Image.Image, box: Tuple[int, int, int, int]
    ) -> Tuple[Dict[str, List[str]], List[Tuple[float, float, float, float]], bool]:
        """
        Extract and recognize all Japanese text lines inside `box` across all 4 OCR engines.
        Returns `(sentences_by_engine, line_bboxes)`.
        """
        w, h = screen_img.size
        bx1 = max(0, min(w - 1, min(box[0], box[2])))
        bx2 = max(0, min(w, max(box[0], box[2])))
        by1 = max(0, min(h - 1, min(box[1], box[3])))
        by2 = max(0, min(h, max(box[1], box[3])))

        if bx2 - bx1 < 8:
            mid_x = (bx1 + bx2) // 2
            bx1 = max(0, mid_x - 24)
            bx2 = min(w, mid_x + 24)
        if by2 - by1 < 8:
            mid_y = (by1 + by2) // 2
            by1 = max(0, mid_y - 12)
            by2 = min(h, mid_y + 12)

        all_screen_lines = self.detect_screen_lines(screen_img)
        matched_lines: List[DetectedLine] = []
        for ln in all_screen_lines:
            lx1, ly1, lx2, ly2 = ln.bbox
            ov_w = max(0, min(bx2, lx2) - max(bx1, lx1))
            ov_h = max(0, min(by2, ly2) - max(by1, ly1))
            if ov_w <= 0 or ov_h <= 0:
                continue
            # Only send characters that are at least partially contained within the selection window
            clipped_ln = self._clip_detected_line_to_selection(screen_img, ln, bx1, by1, bx2, by2)
            if clipped_ln is not None:
                matched_lines.append(clipped_ln)

        if not matched_lines:
            cx1 = max(0, bx1 - 3)
            cy1 = max(0, by1 - 3)
            cx2 = min(w, bx2 + 3)
            cy2 = min(h, by2 + 3)
            crop_box = (cx1, cy1, cx2, cy2)
            with self._ocr_exec_lock:
                refined_boxes = self._split_crop_into_horizontal_row_boxes(screen_img, crop_box)
                for rb in refined_boxes:
                    _, has_lead, has_trail, _, eng_clean, eng_raw, *rest = self._recognize_line_crop_all_engines(screen_img, rb)
                    if any(v and JP_CHAR_RE.search(v) for v in eng_raw.values()):
                        is_v = (rb[3] - rb[1]) > (rb[2] - rb[0]) * 1.6
                        m_txt = eng_clean.get("Manga") or eng_raw.get("Manga", "")
                        cand_ln = DetectedLine(
                            rb,
                            m_txt,
                            is_v,
                            has_lead,
                            has_trail,
                            raw_text=eng_raw.get("Manga", m_txt),
                            engine_texts=eng_clean,
                            engine_raw_texts=eng_raw,
                        )
                        clipped_ln = self._clip_detected_line_to_selection(screen_img, cand_ln, bx1, by1, bx2, by2)
                        if clipped_ln is not None:
                            matched_lines.append(clipped_ln)

        if not matched_lines:
            with self._ocr_exec_lock:
                _, _, _, _, _, eng_direct, *rest = self._recognize_line_crop_all_engines(screen_img, (bx1, by1, bx2, by2))
            if any(eng_direct.values()):
                return (
                    {eng: ([eng_direct[eng]] if eng_direct.get(eng) else []) for eng in OCR_ENGINE_NAMES},
                    [(float(bx1), float(by1), float(bx2), float(by2))],
                    False,
                )
            return {eng: [] for eng in OCR_ENGINE_NAMES}, [], False

        matched_lines = self._filter_furigana_region_lines(matched_lines)
        vert_lines = [l for l in matched_lines if l.is_vertical]
        horiz_lines = [l for l in matched_lines if not l.is_vertical]

        rows: List[List[DetectedLine]] = []
        if horiz_lines:
            horiz_lines.sort(key=lambda l: ((l.bbox[1] + l.bbox[3]) / 2.0, l.bbox[0]))
            for l in horiz_lines:
                placed = False
                lh = max(1, l.bbox[3] - l.bbox[1])
                for r_list in rows:
                    ry1 = min(x.bbox[1] for x in r_list)
                    ry2 = max(x.bbox[3] for x in r_list)
                    rh = max(1, ry2 - ry1)
                    v_ov = min(l.bbox[3], ry2) - max(l.bbox[1], ry1)
                    if v_ov >= min(lh, rh) * 0.35:
                        r_list.append(l)
                        placed = True
                        break
                if not placed:
                    rows.append([l])

        if vert_lines:
            vert_lines.sort(key=lambda l: (-(l.bbox[0] + l.bbox[2]) / 2.0, l.bbox[1]))

        terminal_punct = set("。！？!?…‥」』）)")
        sentences_by_eng: Dict[str, List[str]] = {eng: [] for eng in OCR_ENGINE_NAMES}
        line_bboxes: List[Tuple[float, float, float, float]] = []

        for eng_idx, eng in enumerate(OCR_ENGINE_NAMES):
            row_entries: List[Tuple[str, Tuple[int, int, int, int], bool]] = []
            for r_list in rows:
                r_list.sort(key=lambda l: l.bbox[0])
                row_has_jp = any(JP_CHAR_RE.search(l.text) for l in r_list)
                valid_chunks: List[str] = []
                for l in r_list:
                    c_txt = (
                        (l.engine_raw_texts.get(eng) if l.engine_raw_texts else None)
                        or (l.engine_texts.get(eng) if l.engine_texts else None)
                        or ""
                    ).strip()
                    if not c_txt:
                        continue
                    if re.match(r"^[…・．.\s]+$", c_txt) and (l.bbox[2] - l.bbox[0]) <= 14:
                        continue
                    if row_has_jp and is_noise_chunk(c_txt):
                        continue
                    valid_chunks.append(c_txt)
                row_txt = " ".join(valid_chunks)
                if row_txt and JP_CHAR_RE.search(row_txt):
                    rx1 = min(l.bbox[0] for l in r_list)
                    ry1 = min(l.bbox[1] for l in r_list)
                    rx2 = max(l.bbox[2] for l in r_list)
                    ry2 = max(l.bbox[3] for l in r_list)
                    row_entries.append((row_txt, (rx1, ry1, rx2, ry2), False))

            for vl in vert_lines:
                v_txt = (
                    (vl.engine_raw_texts.get(eng) if vl.engine_raw_texts else None)
                    or (vl.engine_texts.get(eng) if vl.engine_texts else None)
                    or ""
                ).strip()
                if v_txt:
                    row_entries.append((v_txt, vl.bbox, True))

            stitched: List[str] = []
            for idx, (txt, lbox, is_v) in enumerate(row_entries):
                if eng_idx == 0:
                    line_bboxes.append((float(lbox[0]), float(lbox[1]), float(lbox[2]), float(lbox[3])))
                if not stitched:
                    stitched.append(txt)
                    continue
                prev_txt, prev_box, prev_v = row_entries[idx - 1]
                prev_ends_sentence = bool(prev_txt) and (prev_txt[-1] in terminal_punct)
                v_gap = lbox[1] - prev_box[3]
                avg_h = ((prev_box[3] - prev_box[1]) + (lbox[3] - lbox[1])) / 2.0
                left_aligned = abs(lbox[0] - prev_box[0]) <= max(36.0, avg_h * 2.2)

                if not is_v and not prev_v and not prev_ends_sentence and v_gap <= avg_h * 1.5 and left_aligned:
                    stitched[-1] = stitched[-1] + " " + txt
                else:
                    stitched.append(txt)

            sentences_by_eng[eng] = stitched

        is_gemini_region = bool(matched_lines) and all(ln.ocr_source == "gemini" for ln in matched_lines)
        return sentences_by_eng, line_bboxes, is_gemini_region

    @staticmethod
    def _filter_furigana_region_lines(lines: List[DetectedLine]) -> List[DetectedLine]:
        """
        Remove small kana-only furigana (ruby) boxes positioned directly above horizontal kanji lines
        or directly to the right of vertical kanji lines inside a phrase selection.
        """
        if len(lines) <= 1:
            return lines

        kana_only_re = re.compile(r"^[\u3040-\u309F\u30A0-\u30FF\u30FC\u30FD\u30FE\u309D\u309E\s・…]+$")
        kanji_re = re.compile(r"[\u4E00-\u9FFF\u3005]")

        def _line_txt(ln: DetectedLine) -> str:
            t = (ln.raw_text or ln.text or "").strip()
            if t:
                return t
            for src in (ln.engine_raw_texts, ln.engine_texts):
                if src:
                    for v in src.values():
                        if v and v.strip():
                            return v.strip()
            return ""

        texts = [_line_txt(ln) for ln in lines]
        keep: List[DetectedLine] = []
        for idx, cand in enumerate(lines):
            ct = texts[idx]
            is_ruby = False
            if ct and len(ct) <= 16 and kana_only_re.fullmatch(ct):
                cx1, cy1, cx2, cy2 = cand.bbox
                cw = max(1, cx2 - cx1)
                ch = max(1, cy2 - cy1)
                c_cx = (cx1 + cx2) * 0.5
                c_cy = (cy1 + cy2) * 0.5
                for j, host in enumerate(lines):
                    if j == idx or cand.is_vertical != host.is_vertical:
                        continue
                    ht = texts[j]
                    if not ht or not kanji_re.search(ht):
                        continue
                    hx1, hy1, hx2, hy2 = host.bbox
                    hw = max(1, hx2 - hx1)
                    hh = max(1, hy2 - hy1)
                    if not cand.is_vertical:
                        h_cy = (hy1 + hy2) * 0.5
                        ov_x = min(cx2, hx2) - max(cx1, hx1)
                        gap_y = hy1 - cy2
                        if (
                            c_cy < h_cy
                            and ch <= hh * 0.72
                            and ov_x >= max(4.0, cw * 0.55)
                            and -hh * 0.45 <= gap_y <= max(8.0, hh * 0.65)
                        ):
                            is_ruby = True
                            break
                    else:
                        h_cx = (hx1 + hx2) * 0.5
                        ov_y = min(cy2, hy2) - max(cy1, hy1)
                        gap_x = cx1 - hx2
                        if (
                            c_cx > h_cx
                            and cw <= hw * 0.72
                            and ov_y >= max(4.0, ch * 0.55)
                            and -hw * 0.45 <= gap_x <= max(8.0, hw * 0.65)
                        ):
                            is_ruby = True
                            break
            if not is_ruby:
                keep.append(cand)
        return keep

    def _build_region_results_from_lines(
        self,
        screen_img: Image.Image,
        box: Tuple[int, int, int, int],
        jp_lines: List[str],
        line_bboxes: List[Tuple[float, float, float, float]],
        title: Union[int, str] = 0,
        substitute_hiragana: Optional[bool] = None,
        use_gemini: bool = True,
        is_gemini_ocr: bool = False,
    ) -> List[DictionaryResult]:
        """Build the Selection Translation card and L/R vocabulary breakdown cards for `jp_lines`."""
        if not jp_lines:
            bx1, by1, bx2, by2 = box
            return [
                DictionaryResult(
                    matched_surface="",
                    context_text="",
                    headword="No Text Found",
                    reading="",
                    pos=f"[Region {bx1},{by1} -> {bx2},{by2}]",
                    glosses=[
                        "No Japanese text was detected inside the selected box.",
                        "Tip: Press [A] twice on opposite corners of the text region.",
                    ],
                    word_bboxes=[],
                    found=False,
                )
            ]

        en_lines, glossary_terms = self.translate_japanese_text(
            jp_lines,
            title=title,
            substitute_hiragana=substitute_hiragana,
            screen_img=screen_img,
            selection_box=box,
            use_gemini=use_gemini,
            is_gemini_ocr=is_gemini_ocr,
        )
        full_jp_inline = " / ".join(jp_lines)
        full_jp_concat = "".join(
            re.sub(r"[^\u3005\u3040-\u309F\u30A0-\u30FF\u30FC\u4E00-\u9FFF]", "", ln)
            for ln in jp_lines
        )

        gloss_display: List[str] = []
        for idx, en_ln in enumerate(en_lines, start=1):
            if len(en_lines) > 1:
                gloss_display.append(f"{idx}. {en_ln}")
            else:
                gloss_display.append(en_ln)

        gloss_display.append("── Japanese ──")
        for jp_ln in jp_lines:
            gloss_display.append(jp_ln)

        gloss_display.append("── Pronunciation ──")
        for jp_ln in jp_lines:
            hira_ln = self.to_hiragana_pronunciation(jp_ln)
            if hira_ln:
                gloss_display.append(hira_ln)

        if glossary_terms:
            gloss_display.append("── Game Terms ──")
            for jp_t, en_t, pos_t in glossary_terms:
                gloss_display.append(f"• {jp_t} = {en_t} ({pos_t})")

        translation_card = DictionaryResult(
            matched_surface=full_jp_inline,
            context_text=full_jp_inline,
            headword="Selection Translation",
            reading="",
            pos=f"JP: {full_jp_inline}",
            glosses=gloss_display,
            word_bboxes=line_bboxes,
            found=True,
        )

        results: List[DictionaryResult] = [translation_card]

        if full_jp_concat:
            with self._lock:
                morphemes = self._analyze_morphemes(full_jp_concat)
            sub_chars = [
                CharSpan(ch, (float(box[0] + idx * 10), float(box[1]), float(box[0] + (idx + 1) * 10), float(box[3])))
                for idx, ch in enumerate(full_jp_concat)
            ]
            targeted_para = StitchedParagraph(
                bbox=box,
                full_text=full_jp_concat,
                chars=sub_chars,
                morphemes=morphemes,
            )
            seen_vocab: set[Tuple[str, str]] = set()
            for m in morphemes:
                if m.pos1 in {"助詞", "助動詞", "補助記号", "記号"} and m.surface not in self._custom_dict:
                    continue
                if not full_jp_concat[m.start_idx :]:
                    continue
                m_results = self.lookup_at_cursor_all(
                    screen_img,
                    [targeted_para],
                    int(box[0] + m.start_idx * 10 + 2),
                    int((box[1] + box[3]) // 2),
                    allow_fallback_ocr=False,
                    allow_short_tying_ocr=False,
                )
                if m_results and m_results[0].found:
                    top_r = m_results[0]
                    v_key = (top_r.headword, top_r.reading)
                    if v_key not in seen_vocab:
                        seen_vocab.add(v_key)
                        top_r.context_text = full_jp_inline
                        results.append(top_r)

        return results

    def translate_region_variants(
        self,
        screen_img: Image.Image,
        box: Tuple[int, int, int, int],
        title: Union[int, str] = 0,
        substitute_hiragana: Optional[bool] = None,
    ) -> List[OcrVariant]:
        """
        Perform 4-engine OCR and English translation on the user-selected region `box`.
        Groups engines by normalized Japanese text, translates each unique OCR variant,
        and annotates all results with agreement/disagreement metadata for `ZL` / `ZR` cycling.
        """
        sentences_by_eng, line_bboxes, is_gemini_region = self._extract_region_lines_by_engine(screen_img, box)

        grouped_lines: Dict[str, Tuple[List[str], List[str]]] = {}
        for eng in OCR_ENGINE_NAMES:
            jp_lines = sentences_by_eng.get(eng, [])
            norm_key = re.sub(
                r"[^\u3005\u3040-\u309F\u30A0-\u30FF\u30FC\u4E00-\u9FFF]+",
                "",
                "".join(jp_lines),
            )
            if not norm_key:
                continue
            if norm_key not in grouped_lines:
                grouped_lines[norm_key] = ([eng], jp_lines)
            else:
                grouped_lines[norm_key][0].append(eng)

        if not grouped_lines:
            empty_res = self._build_region_results_from_lines(
                screen_img, box, [], [], title=title, substitute_hiragana=substitute_hiragana
            )
            return [OcrVariant(engines=list(OCR_ENGINE_NAMES), ocr_text="", results=empty_res)]

        sorted_groups = sorted(
            grouped_lines.values(),
            key=lambda item: (len(item[0]), -OCR_ENGINE_NAMES.index(item[0][0])),
            reverse=True,
        )

        variants: List[OcrVariant] = []
        for g_idx, (eng_list, jp_lines) in enumerate(sorted_groups):
            res_list = self._build_region_results_from_lines(
                screen_img,
                box,
                jp_lines,
                line_bboxes,
                title=title,
                substitute_hiragana=substitute_hiragana,
                use_gemini=(g_idx == 0),
                is_gemini_ocr=is_gemini_region,
            )
            inline_jp = " / ".join(jp_lines)
            variants.append(
                OcrVariant(
                    engines=eng_list,
                    ocr_text=inline_jp,
                    results=res_list,
                )
            )

        if len(variants) == 1 and is_gemini_region:
            variants[0].engines = ["Gemini"]
            summary_line = ""
        else:
            summary_parts = [
                f"{idx + 1}:{'+'.join(v.engines)}({v.ocr_text[:12]}{'…' if len(v.ocr_text) > 12 else ''})"
                for idx, v in enumerate(variants)
            ]
            summary_line = (
                f"★ OCR Diff [ZL/ZR]: {' | '.join(summary_parts)}"
                if len(variants) > 1
                else ""
            )

        for v_idx, v in enumerate(variants):
            for r in v.results:
                r.ocr_disagreement = len(variants) > 1
                r.ocr_variant_idx = v_idx
                r.ocr_total_variants = len(variants)
                r.ocr_engines = list(v.engines)
                r.ocr_summary_line = summary_line

        return variants


def recv_exact(
    sock: socket.socket,
    num_bytes: int,
    max_total_sec: Optional[float] = None,
) -> Optional[bytes]:
    """
    Read exactly `num_bytes` from `sock`, or return None on EOF.
    If `max_total_sec` is set, enforces a cumulative wall-clock deadline across
    all `recv()` calls to prevent Slowloris / 1-byte drip-feed stalls.
    """
    deadline = (time.monotonic() + max_total_sec) if max_total_sec is not None else None
    buf = bytearray()
    while len(buf) < num_bytes:
        if deadline is not None:
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise socket.timeout("Cumulative frame transfer deadline exceeded")
            sock.settimeout(remaining)
        try:
            chunk = sock.recv(num_bytes - len(buf))
        except (ConnectionResetError, ConnectionAbortedError):
            return None
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def load_cjk_ttf_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    """Load a Japanese-capable TTF/TTC font across Windows, macOS, and Linux."""
    candidates = [
        r"C:\Windows\Fonts\msgothic.ttc",
        r"C:\Windows\Fonts\meiryo.ttc",
        r"C:\Windows\Fonts\YuGothM.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/takao-gothic/TakaoPGothic.ttf",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/System/Library/Fonts/AquaKana.ttc",
    ]
    for path in candidates:
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size=size)
            except OSError:
                continue
    logger.warning("No system CJK TTF font found; falling back to default Pillow font.")
    return ImageFont.load_default()


POS_WINDOW_CHARS = 36


def _wrap_result_glosses(result: DictionaryResult) -> List[str]:
    """
    Wrap all definition senses (or English + Japanese translation lines) of `result`
    into display lines for the 320x240 popup, accounting for double-width CJK characters.
    Prepends the 4-engine OCR disagreement summary banner when `result.ocr_summary_line` is set.
    Caches the wrapped lines on `result` to avoid redundant text measurement during scrolling.
    """
    if result._wrapped_lines is not None:
        return result._wrapped_lines

    lines: List[str] = []
    source_glosses: List[str] = []
    if result.ocr_summary_line:
        source_glosses.append(result.ocr_summary_line)
    source_glosses.extend(result.glosses)

    for gloss in source_glosses:
        if not gloss:
            continue
        is_ocr_banner = gloss.startswith("★")
        has_cjk = any(ord(ch) > 0x2E80 for ch in gloss)
        if not has_cjk:
            wrapped = textwrap.wrap(gloss, width=40)
            if wrapped:
                for w_idx, w_ln in enumerate(wrapped):
                    if is_ocr_banner and w_idx > 0 and not w_ln.startswith("★"):
                        lines.append("★ " + w_ln)
                    else:
                        lines.append(w_ln)
        else:
            cur_line: List[str] = []
            cur_cols = 0
            first_sub = True
            for ch in gloss:
                cw = 2 if ord(ch) > 0x2E80 else 1
                if cur_cols + cw > 40 and cur_line:
                    sub_str = "".join(cur_line)
                    lines.append(sub_str if (first_sub or not is_ocr_banner) else ("★ " + sub_str))
                    first_sub = False
                    cur_line = [ch]
                    cur_cols = cw + (2 if is_ocr_banner else 0)
                else:
                    cur_line.append(ch)
                    cur_cols += cw
            if cur_line:
                sub_str = "".join(cur_line)
                lines.append(sub_str if (first_sub or not is_ocr_banner) else ("★ " + sub_str))
    res_lines = lines or ["(No English glosses listed)"]
    result._wrapped_lines = res_lines
    return res_lines


def compute_popup_scroll_limits(result: DictionaryResult) -> Tuple[int, int]:
    """Return `(max_gloss_scroll_y, max_pos_scroll_x)` for `result`."""
    all_lines = _wrap_result_glosses(result)
    max_gloss_y = max(0, len(all_lines) - GLOSS_VISIBLE_LINES)
    pos_text = result.pos or "Word"
    max_pos_x = max(0, len(pos_text) - POS_WINDOW_CHARS)
    return max_gloss_y, max_pos_x


def render_dictionary_popup(
    result: DictionaryResult,
    screen_id: int,
    x: int,
    y: int,
    entry_idx: int = 0,
    total_entries: int = 1,
    gloss_scroll_y: int = 0,
    pos_scroll_x: int = 0,
    status_banner: str = "",
    footer_keys: Optional[Tuple[str, str, str]] = None,
) -> bytes:
    """
    Render a 320x240 RGB24 dictionary popup bitmap for the opposite 3DS screen,
    supporting 4-engine OCR agreement/disagreement indicators, ZL/ZR variant cycling,
    L/R match index display, D-Pad Left/Right grammar note scrolling,
    D-Pad Up/Down definition scrolling, and X-button Anki export status feedback.
    """
    popup = Image.new("RGB", POPUP_DIM, color=(20, 24, 34))
    draw = ImageDraw.Draw(popup)

    if not result.found:
        border_col = (235, 130, 90)
    elif result.ocr_disagreement:
        border_col = (245, 180, 75)
    else:
        border_col = (88, 166, 255)
    draw.rectangle([0, 0, 319, 239], outline=border_col, width=2)
    draw.rectangle([2, 2, 317, 30], fill=(34, 42, 64))

    font_small = load_cjk_ttf_font(12)
    font_title = load_cjk_ttf_font(20)
    font_reading = load_cjk_ttf_font(13)
    font_body = load_cjk_ttf_font(13)

    if result.headword == "Selection Translation":
        surf_info = f"Selection: {result.matched_surface}"
    else:
        surf_info = (
            f"Match: {result.matched_surface} -> {result.headword}"
            if result.matched_surface and result.matched_surface != result.headword
            else f"Text: {result.context_text}"
        )

    # Match counter badge on the right side of the header bar (e.g. "1/7 [L/R]")
    total_clamped = max(1, total_entries)
    idx_clamped = max(0, min(entry_idx, total_clamped - 1))
    badge_text = f"[{idx_clamped + 1}/{total_clamped}]"
    try:
        badge_w = int(font_small.getlength(badge_text))
    except Exception:
        badge_w = len(badge_text) * 7
    badge_x = max(200, 312 - badge_w)
    draw.text(
        (badge_x, 7),
        badge_text,
        fill=(255, 220, 110) if total_clamped > 1 else (150, 175, 210),
        font=font_small,
    )

    # 4-Engine OCR agreement / disagreement badge immediately to the left of the match badge
    ocr_badge_w = 0
    if result.ocr_disagreement and result.ocr_total_variants > 1:
        ocr_badge_text = f"[OCR {result.ocr_variant_idx + 1}/{result.ocr_total_variants}!]"
        ocr_badge_col = (255, 175, 80)
    elif result.ocr_engines == ["Gemini"] and result.found:
        ocr_badge_text = "Gemini"
        ocr_badge_col = (125, 230, 165)
    elif result.ocr_engines and result.found:
        ocr_badge_text = f"[{len(result.ocr_engines)}/4✓]"
        ocr_badge_col = (125, 230, 165)
    else:
        ocr_badge_text = ""
        ocr_badge_col = (150, 175, 210)

    if ocr_badge_text:
        try:
            ocr_badge_w = int(font_small.getlength(ocr_badge_text))
        except Exception:
            ocr_badge_w = len(ocr_badge_text) * 7
        ocr_badge_x = max(140, badge_x - ocr_badge_w - 5)
        draw.text(
            (ocr_badge_x, 7),
            ocr_badge_text,
            fill=ocr_badge_col,
            font=font_small,
        )

    max_header_w = max(125, 312 - badge_w - ocr_badge_w - 14)
    if result.headword == "Selection Translation":
        prefix_tag = "[Translate] "
    elif getattr(result, "rank_jlpt", ""):
        prefix_tag = f"[{result.rank_jlpt}] "
    else:
        prefix_tag = ""
    header_text = f"{prefix_tag}{surf_info}"
    try:
        while font_small.getlength(header_text) > max_header_w and len(header_text) > 8:
            header_text = header_text[:-2] + "…"
    except Exception:
        header_text = header_text[:28]
    draw.text((8, 7), header_text, fill=(185, 215, 255), font=font_small)

    # Headword + Reading — wrap to lines below instead of truncating
    def _wrap_title_seg(seg: str, max_w: int, max_lines: int, out_lines: List[str]) -> None:
        if not seg or len(out_lines) >= max_lines:
            return
        cur = ""
        for ch in seg:
            if len(out_lines) == max_lines - 1:
                cand = cur + ch
                if font_title.getlength(cand) > max_w and cur:
                    while len(cur) > 1 and font_title.getlength(cur + "…") > max_w:
                        cur = cur[:-1]
                    out_lines.append(cur + "…")
                    return
                cur = cand
            else:
                cand = cur + ch
                if font_title.getlength(cand) > max_w and cur:
                    out_lines.append(cur)
                    cur = ch
                else:
                    cur = cand
        if cur and len(out_lines) < max_lines:
            out_lines.append(cur)

    has_reading = bool(result.reading and result.reading != result.headword)
    full_title = f"{result.headword} 【{result.reading}】" if has_reading else result.headword
    title_lines: List[str] = []
    if font_title.getlength(full_title) <= 295:
        title_lines = [full_title]
    elif has_reading:
        _wrap_title_seg(result.headword, 295, 3, title_lines)
        _wrap_title_seg(f"【{result.reading}】", 295, 3, title_lines)
    else:
        _wrap_title_seg(result.headword, 295, 3, title_lines)
    if not title_lines:
        title_lines = [result.headword]

    for ti, t_ln in enumerate(title_lines):
        draw.text((12, 36 + ti * 22), t_ln, fill=(255, 230, 115), font=font_title)

    extra_y = max(0, len(title_lines) - 1) * 22
    pos_y = 64 + extra_y
    div_y = 85 + extra_y
    gloss_start_y = 90 + extra_y
    vis_gloss = max(1, min(GLOSS_VISIBLE_LINES, (212 - gloss_start_y + 3) // 17))

    # Grammar note / Part of speech header (horizontally scrollable via D-Pad Left / Right)
    all_lines = _wrap_result_glosses(result)
    max_gloss_y = max(0, len(all_lines) - vis_gloss)
    _, max_pos_x = compute_popup_scroll_limits(result)
    pos_text = result.pos or "Word"
    pos_offset = max(0, min(pos_scroll_x, max_pos_x))
    pos_slice = pos_text[pos_offset : pos_offset + POS_WINDOW_CHARS]

    if pos_offset > 0:
        draw.text((4, pos_y), "<", fill=(255, 220, 110), font=font_reading)
    pos_x_start = 14 if pos_offset > 0 else 12
    draw.text((pos_x_start, pos_y), pos_slice, fill=(125, 210, 175), font=font_reading)
    if pos_offset < max_pos_x:
        draw.rectangle([302, pos_y - 2, 317, pos_y + 19], fill=(20, 24, 34))
        draw.text((306, pos_y), ">", fill=(255, 220, 110), font=font_reading)

    draw.line([(12, div_y), (308, div_y)], fill=(55, 68, 95), width=1)

    # Word-wrapped English definitions (vertically scrollable via D-Pad Up / Down)
    g_offset = max(0, min(gloss_scroll_y, max_gloss_y))
    visible_lines = all_lines[g_offset : g_offset + vis_gloss]

    y_cursor = gloss_start_y
    for line in visible_lines:
        if line.startswith("★"):
            line_col = (255, 195, 95)
        elif line.startswith("──"):
            line_col = (120, 195, 255)
        elif line.startswith("•"):
            line_col = (255, 225, 135)
        else:
            line_col = (235, 240, 248)
        draw.text((12, y_cursor), line, fill=line_col, font=font_body)
        y_cursor += 17

    # Draw vertical scrollbar on the right edge if definitions exceed visible lines
    if max_gloss_y > 0:
        track_top = gloss_start_y
        track_bot = 206
        track_h = max(24, track_bot - track_top)
        draw.rectangle([311, track_top, 314, track_bot], fill=(38, 46, 68))
        thumb_h = max(14, int(track_h * (vis_gloss / float(len(all_lines)))))
        thumb_y = track_top + int((track_h - thumb_h) * (g_offset / float(max_gloss_y)))
        draw.rectangle([311, thumb_y, 314, thumb_y + thumb_h], fill=(88, 166, 255))

    # Footer controls bar / Anki status banner
    draw.rectangle([2, 214, 317, 237], fill=(28, 34, 50))
    if status_banner:
        is_err = status_banner.startswith("Anki Error") or status_banner.startswith("Set AnkiWeb")
        is_pending = status_banner.startswith("Adding ") or status_banner.startswith("Translating ")
        if is_err:
            banner_col = (255, 160, 120)
        elif is_pending:
            banner_col = (255, 225, 110)
        else:
            banner_col = (120, 240, 165)
        banner_display = status_banner
        try:
            while font_small.getlength(banner_display) > 304 and len(banner_display) > 4:
                banner_display = banner_display[:-2] + "…"
        except Exception:
            banner_display = status_banner[:46]
        draw.text((8, 219), banner_display, fill=banner_col, font=font_small)
    else:
        lbl_lookup = footer_keys[0] if footer_keys else "A"
        lbl_mode = footer_keys[1] if footer_keys else "SEL"
        lbl_anki = footer_keys[2] if footer_keys else "X"
        if result.ocr_disagreement:
            default_footer = (
                f"[{lbl_lookup}]Box [ZL/ZR]OCR [L/R]Vocab [DPad]Scrl [{lbl_anki}]Anki"
                if result.headword == "Selection Translation"
                else f"[{lbl_lookup}]Word [ZL/ZR]OCR [L/R]Match [DPad]Scrl [{lbl_anki}]Anki"
            )
        else:
            default_footer = (
                f"[{lbl_lookup}]Box [{lbl_mode}]Dict [L/R]Vocab [DPad]Scrl [{lbl_anki}]Anki"
                if result.headword == "Selection Translation"
                else f"[{lbl_lookup}]Word [{lbl_mode}]Box [L/R]Match [DPad]Scrl [{lbl_anki}]Anki"
            )
        draw.text(
            (6, 219),
            default_footer,
            fill=(145, 160, 185),
            font=font_small,
        )

    raw_bytes = popup.tobytes("raw", "RGB")
    assert len(raw_bytes) == POPUP_FB_BYTES
    return raw_bytes


def render_kanji_popup(
    kanji_char: str,
    target_screen_id: int,
    jamdict: Any = None,
    word_meta_mgr: Optional[WordMetaManager] = None,
    out_hotspots: Optional[List[Tuple[int, int, int, int, int]]] = None,
) -> bytes:
    """
    Render a 320x400 RGB bitmap for the kanji popup. The 3DS receives the full tall canvas
    and handles scrolling locally by cropping the visible 240px window.
    """
    popup = Image.new("RGB", KANJI_TALL_DIM, color=(20, 24, 34))
    draw = ImageDraw.Draw(popup)

    # Theme colors
    bg_body = (20, 24, 34)
    bg_header = (34, 42, 64)

    # 1. Body fill (no border drawn here — C++ overlays it at fixed screen coords every frame)
    draw.rectangle([0, 0, KANJI_TALL_DIM[0] - 1, KANJI_TALL_DIM[1] - 1], fill=bg_body)
    # Header background (y: 2..31)
    draw.rectangle([2, 2, KANJI_TALL_DIM[0] - 3, 31], fill=bg_header)

    font_hdr = load_cjk_ttf_font(13)
    font_body = load_cjk_ttf_font(12)
    font_small = load_cjk_ttf_font(11)

    def _load_rad_fallback_font(size: int) -> Optional[ImageFont.ImageFont]:
        for path in (
            r"C:\Windows\Fonts\meiryo.ttc",
            r"C:\Windows\Fonts\YuGothM.ttc",
            r"C:\Windows\Fonts\YuGothR.ttc",
        ):
            if os.path.isfile(path):
                try:
                    return ImageFont.truetype(path, size=size)
                except Exception:
                    pass
        return None

    def _draw_text_with_rad_fallback(
        pos: Tuple[int, int],
        text: str,
        fill: tuple,
        font: ImageFont.ImageFont,
        rad_font: Optional[ImageFont.ImageFont],
    ) -> None:
        if rad_font is None or not any(0x2E80 <= ord(ch) <= 0x2EFF for ch in text):
            draw.text(pos, text, fill=fill, font=font)
            return
        pen_x = float(pos[0])
        y0 = pos[1]
        for ch in text:
            use_font = rad_font if (0x2E80 <= ord(ch) <= 0x2EFF) else font
            draw.text((int(round(pen_x)), y0), ch, fill=fill, font=use_font)
            pen_x += float(use_font.getlength(ch))

    rad_font_hdr = _load_rad_fallback_font(13)
    rad_font_small = _load_rad_fallback_font(11)

    # Header text
    hdr_label = f"[Kanji Details]  {kanji_char}"
    _draw_text_with_rad_fallback((10, 8), hdr_label, (185, 215, 255), font_hdr, rad_font_hdr)
    draw.text((250, 9), "[B: Close]", fill=(150, 175, 210), font=font_small)

    # Get kanji details
    if word_meta_mgr is not None and jamdict is not None:
        info = word_meta_mgr.lookup_kanji_info(kanji_char, jamdict)
    else:
        info = {}
    stroke_count = info.get("stroke_count")
    grade = info.get("grade")
    freq = info.get("freq")
    jlpt = info.get("jlpt")
    rad_num = info.get("rad_num")
    rad_char = info.get("rad_char", "")
    rad_base = info.get("rad_base", "")
    rad_reading = info.get("rad_reading", "")
    rad_meaning = info.get("rad_meaning", "")
    on_readings: List[str] = info.get("on_readings", [])
    kun_readings: List[str] = info.get("kun_readings", [])
    nanori: List[str] = info.get("nanori", [])
    meanings: List[str] = info.get("meanings", [])

    # Helper: pixel-width line wrapper (wraps a list of items joined by sep)
    def _wrap_items(items: List[str], sep: str, font: ImageFont.ImageFont, max_w: int) -> List[str]:
        """Join items with sep, wrapping into new lines when max_w is exceeded."""
        lines: List[str] = []
        current = ""
        for item in items:
            candidate = current + (sep if current else "") + item
            if font.getlength(candidate) <= max_w:
                current = candidate
            else:
                if current:
                    lines.append(current)
                current = item
        if current:
            lines.append(current)
        return lines or [""]

    # Helper: truncate a single string to fit max_w pixels
    def _fit_text(text: str, font: ImageFont.ImageFont, max_w: int) -> str:
        if not text:
            return ""
        cur = text
        while font.getlength(cur) > max_w and len(cur) > 3:
            cur = cur[:-2] + "…"
        return cur

    def _is_clickable_kanji_or_rad(ch: str) -> bool:
        cp = ord(ch)
        return (
            (0x4E00 <= cp <= 0x9FFF)
            or (0x3400 <= cp <= 0x4DBF)
            or (0xF900 <= cp <= 0xFAFF)
            or (0x2E80 <= cp <= 0x2FDF)
            or cp == 0x3005
        )

    def _record_text_hotspots(
        text: str,
        x0: int,
        y0: int,
        font: ImageFont.ImageFont,
        line_h: int,
        rad_font: Optional[ImageFont.ImageFont] = None,
    ) -> None:
        if out_hotspots is None or not text:
            return
        pen_x = float(x0)
        for ch in text:
            use_font = rad_font if (rad_font is not None and 0x2E80 <= ord(ch) <= 0x2EFF) else font
            adv = float(use_font.getlength(ch))
            if _is_clickable_kanji_or_rad(ch):
                out_hotspots.append(
                    (int(pen_x) - 1, y0 - 1, int(pen_x + adv) + 1, y0 + line_h, ord(ch))
                )
            pen_x += adv

    # --- Stroke order font (load once, used after body paste) ---
    stroke_font = None
    if os.path.isfile(KANJI_STROKE_FONT_PATH):
        try:
            stroke_font = ImageFont.truetype(KANJI_STROKE_FONT_PATH, size=106)
        except Exception:
            stroke_font = None
    if stroke_font is None:
        stroke_font = load_cjk_ttf_font(84)

    # --- Render all content directly onto the tall popup image ---
    # Header occupies y=0..31. Stroke box occupies x=8..134, y=36..162 (drawn below).
    # Right column and meanings draw freely from y=36 downward.
    # The 3DS shows a 240px window and scrolls it locally.

    right_x = 140
    READ_MAX_W = 168   # pixel width for reading lines in right column

    # Meta line (JLPT / Grade / Freq) — wrapped, no truncation
    meta_parts = []
    if jlpt:
        meta_parts.append(f"JLPT N{jlpt}")
    if grade:
        meta_parts.append(f"Grade {grade}")
    if freq:
        meta_parts.append(f"Freq #{freq}")
    if not meta_parts:
        meta_parts = ["General Kanji"]

    meta_lines = _wrap_items(meta_parts, " · ", font_small, READ_MAX_W)
    y_cur = 36
    for ln in meta_lines:
        draw.text((right_x, y_cur), ln, fill=(255, 220, 110), font=font_small)
        _record_text_hotspots(ln, right_x, y_cur, font_small, 13)
        y_cur += 14
    y_cur += 2  # spacing before readings

    # Readings: On, Kun, Nanori — wrapped, no truncation
    def _draw_readings(label: str, items: List[str], font: ImageFont.ImageFont, color: tuple) -> None:
        nonlocal y_cur
        lines = _wrap_items(items, "・", font, READ_MAX_W - 22)
        for i, ln in enumerate(lines):
            if i == 0:
                draw.text((right_x, y_cur), label, fill=(125, 210, 175), font=font_small)
                _record_text_hotspots(label, right_x, y_cur, font_small, 14)
                draw.text((right_x + 22, y_cur), ln, fill=color, font=font)
            else:
                draw.text((right_x + 22, y_cur), ln, fill=color, font=font)
            _record_text_hotspots(ln, right_x + 22, y_cur, font, 14)
            y_cur += 16

    if on_readings:
        _draw_readings("音:", on_readings, font_body, (255, 230, 115))
    if kun_readings:
        _draw_readings("訓:", kun_readings, font_body, (255, 230, 115))
    if nanori:
        _draw_readings("名:", nanori, font_small, (215, 225, 235))

    # Strokes & Radical (right column, right after readings — no forced min-y)
    y_cur += 4
    draw.line([(right_x, y_cur - 2), (308, y_cur - 2)], fill=(40, 50, 70), width=1)
    strk_lbl = f"Strokes: {stroke_count}" if stroke_count else "Strokes: -"
    draw.text((right_x, y_cur), strk_lbl, fill=(200, 220, 240), font=font_small)
    y_cur += 14
    if rad_char and rad_base and rad_char != rad_base and rad_num:
        rad_lbl = f"Radical: {rad_char} ({rad_base}) #{rad_num}"
    elif rad_char and rad_num:
        rad_lbl = f"Radical: {rad_char} (#{rad_num})"
    elif rad_num:
        rad_lbl = f"Radical: #{rad_num}"
    else:
        rad_lbl = "Radical: -"
    rad_y0 = y_cur
    rad_lbl_disp = _fit_text(rad_lbl, font_small, READ_MAX_W)
    _draw_text_with_rad_fallback(
        (right_x, y_cur),
        rad_lbl_disp,
        (160, 185, 215),
        font_small,
        rad_font_small,
    )
    _record_text_hotspots(rad_lbl_disp, right_x, y_cur, font_small, 14, rad_font=rad_font_small)
    y_cur += 14
    if rad_reading or rad_meaning:
        rad_sub_parts = []
        if rad_reading:
            rad_sub_parts.append(rad_reading)
        if rad_meaning:
            rad_sub_parts.append(f"({rad_meaning})")
        for ln in _wrap_items(rad_sub_parts, " ", font_small, READ_MAX_W):
            draw.text((right_x, y_cur), ln, fill=(140, 170, 200), font=font_small)
            _record_text_hotspots(ln, right_x, y_cur, font_small, 13)
            y_cur += 13
    if out_hotspots is not None and rad_char:
        out_hotspots.append((right_x - 2, rad_y0 - 2, 312, y_cur + 1, ord(rad_char[0])))
    y_cur += 4

    # Meanings (full width, below stroke box and right column)
    y_meanings_start = max(y_cur + 8, 170)
    draw.line([(8, y_meanings_start - 4), (312, y_meanings_start - 4)], fill=(55, 68, 95), width=1)
    draw.text((10, y_meanings_start), "Meanings:", fill=(120, 195, 255), font=font_small)
    meanings_str = ", ".join(meanings) if meanings else "(No English meanings in Kanjidic)"
    wrapped_meanings = textwrap.wrap(meanings_str, width=46)
    y_m = y_meanings_start + 14
    for ln in wrapped_meanings:
        draw.text((12, y_m), ln, fill=(240, 245, 255), font=font_body)
        _record_text_hotspots(ln, 12, y_m, font_body, 14)
        y_m += 14

    # Track actual content bottom (clamped to tall canvas height)
    content_height = min(y_m + 4, KANJI_TALL_DIM[1])

    # Stroke order diagram (drawn last so it's always on top of body content)
    box_x0, box_y0, box_x1, box_y1 = 8, 36, 134, 162
    draw.rectangle([box_x0, box_y0, box_x1, box_y1], fill=(14, 17, 24), outline=(55, 68, 95), width=1)
    cx = (box_x0 + box_x1) // 2
    cy = (box_y0 + box_y1) // 2
    draw.line([(box_x0 + 4, cy), (box_x1 - 4, cy)], fill=(35, 45, 60), width=1)
    draw.line([(cx, box_y0 + 4), (cx, box_y1 - 4)], fill=(35, 45, 60), width=1)
    try:
        bbox = stroke_font.getbbox(kanji_char)
        gw = bbox[2] - bbox[0]
        gh = bbox[3] - bbox[1]
        gx = box_x0 + ((box_x1 - box_x0) - gw) // 2 - bbox[0]
        gy = box_y0 + ((box_y1 - box_y0) - gh) // 2 - bbox[1]
        draw.text((gx, gy), kanji_char, fill=(255, 255, 255), font=stroke_font)
    except Exception:
        draw.text((box_x0 + 10, box_y0 + 10), kanji_char, fill=(255, 255, 255), font=font_hdr)

    # Footer and outer border are drawn by C++ at fixed screen coords every frame —
    # not embedded in the scrollable tall canvas.
    # BUT: we do place the footer text at the bottom of the tall canvas so C++ can copy it.
    footer_y = KANJI_TALL_DIM[1] - 24  # y=376..399
    draw.rectangle([2, footer_y, KANJI_TALL_DIM[0] - 3, KANJI_TALL_DIM[1] - 3], fill=(28, 34, 50))
    draw.text((8, footer_y + 5), "X: Add Kanji to Anki", fill=(125, 230, 165), font=font_small)

    raw_bytes = popup.tobytes("raw", "RGB")
    # Append 2-byte big-endian content_height so the 3DS can compute the real max scroll.
    # max scroll = max(0, content_height - SCROLLABLE_VIEWPORT) where SCROLLABLE_VIEWPORT = POPUP_HEIGHT - 24 = 216
    return raw_bytes + content_height.to_bytes(2, "big")


def _pack_str_u8(text: str) -> bytes:
    b = (text or "").encode("utf-8")[:255]
    return struct.pack("<B", len(b)) + b


def _pack_str_u16(text: str) -> bytes:
    b = (text or "").encode("utf-8")[:65535]
    return struct.pack("<H", len(b)) + b


def build_popup_glyphs(char_pairs: set) -> bytes:
    """
    Pack unique `(font_id, ord(ch))` glyphs into a 4-bpp alpha atlas for client-side 3DS rendering.
    font_id: 0 = small (12px), 1 = body/reading (13px), 2 = title (20px).
    """
    fonts = {
        0: load_cjk_ttf_font(12),
        1: load_cjk_ttf_font(13),
        2: load_cjk_ttf_font(20),
    }
    sorted_pairs = sorted(char_pairs)
    out = bytearray(struct.pack("<H", len(sorted_pairs)))
    for fid, cp in sorted_pairs:
        f = fonts.get(fid, fonts[1])
        ch = chr(cp)
        try:
            adv = max(0, min(255, int(round(f.getlength(ch)))))
        except Exception:
            adv = 12 if cp > 0x7E else 7
        if ch.isspace():
            out.extend(struct.pack("<IBBBbbB", cp, fid, 0, 0, 0, 0, adv))
            continue
        try:
            mask = f.getmask(ch, mode="L")
            mw, mh = mask.size
            bbox = f.getbbox(ch) or (0, 0, mw, mh)
            ox = max(-128, min(127, int(bbox[0])))
            oy = max(-128, min(127, int(bbox[1])))
            mw_c = max(0, min(255, int(mw)))
            mh_c = max(0, min(255, int(mh)))
            raw = bytes(mask)
            if len(raw) != mw_c * mh_c:
                mw_c, mh_c, raw = 0, 0, b""
        except Exception:
            mw_c, mh_c, ox, oy, raw = 0, 0, 0, 0, b""

        packed = bytearray((len(raw) + 1) // 2)
        for idx in range(0, len(raw), 2):
            hi = raw[idx] >> 4
            lo = (raw[idx + 1] >> 4) if (idx + 1 < len(raw)) else 0
            packed[idx // 2] = (hi << 4) | lo
        out.extend(struct.pack("<IBBBbbB", cp, fid, mw_c, mh_c, ox, oy, adv))
        out.extend(packed)
    return bytes(out)


def _format_ocr_badge(result: DictionaryResult) -> str:
    if result.ocr_disagreement and result.ocr_total_variants > 1:
        return f"[OCR {result.ocr_variant_idx + 1}/{result.ocr_total_variants}!]"
    if result.ocr_engines == ["Gemini"] and result.found:
        return "Gemini"
    if result.ocr_engines and result.found:
        return f"[{len(result.ocr_engines)}/4✓]"
    return ""


def _collect_glyph_text(glyph_set: set, fid: int, s: str) -> None:
    for ch in s or "":
        if ch != "\n":
            glyph_set.add((fid, ord(ch)))


def _init_bundle_glyph_set(footer_keys: Optional[List[str]]) -> Tuple[set, str, str, str]:
    lbl_lookup = footer_keys[0] if footer_keys else "A"
    lbl_mode = footer_keys[1] if footer_keys else "SEL"
    lbl_anki = footer_keys[2] if footer_keys else "X"
    glyph_set: set = set()
    for cp in range(0x20, 0x7F):
        glyph_set.add((0, cp))
    for ch in "…✓!":
        glyph_set.add((0, ord(ch)))
    for ch in "<>":
        glyph_set.add((1, ord(ch)))
    for ch in "【】…":
        glyph_set.add((2, ord(ch)))
    for s in (lbl_lookup, lbl_mode, lbl_anki):
        _collect_glyph_text(glyph_set, 0, s)
    return glyph_set, lbl_lookup, lbl_mode, lbl_anki


def _collect_entry_glyphs(glyph_set: set, r: DictionaryResult) -> Tuple[str, List[str]]:
    wrapped = _wrap_result_glosses(r)
    badge = _format_ocr_badge(r)
    _collect_glyph_text(glyph_set, 0, badge)
    _collect_glyph_text(glyph_set, 0, r.matched_surface)
    _collect_glyph_text(glyph_set, 0, r.context_text)
    _collect_glyph_text(glyph_set, 2, r.headword)
    _collect_glyph_text(glyph_set, 2, r.reading)
    _collect_glyph_text(glyph_set, 1, r.pos or "Word")
    for ln in wrapped:
        _collect_glyph_text(glyph_set, 1, ln)
    rank_jlpt = getattr(r, "rank_jlpt", "")
    if rank_jlpt:
        _collect_glyph_text(glyph_set, 0, f"[{rank_jlpt}] ")
    _collect_glyph_text(glyph_set, 0, "[Translate]")
    return badge, wrapped


def build_popup_bundle(
    ocr_variants: List[OcrVariant],
    source_screen_id: int,
    x: int,
    y: int,
    footer_keys: Optional[List[str]] = None,
    status_banner: str = "",
) -> bytes:
    """
    Build a compact binary `POPUP_BUNDLE` payload containing ALL OCR variants, ALL matches,
    pre-wrapped gloss lines, and a deduplicated 4-bpp TTF glyph atlas so the 3DS plugin
    can handle scrolling (DPad), match cycling (L/R), and OCR cycling (ZL/ZR) locally.
    """
    glyph_set, lbl_lookup, lbl_mode, lbl_anki = _init_bundle_glyph_set(footer_keys)
    _collect_glyph_text(glyph_set, 0, status_banner)

    prepared_variants = []
    for v in ocr_variants[:255]:
        eng_str = "+".join(v.engines) if v.engines else "All"
        _collect_glyph_text(glyph_set, 0, eng_str)
        _collect_glyph_text(glyph_set, 0, v.ocr_text[:40])
        entries_data = []
        for r in v.results:
            badge, wrapped = _collect_entry_glyphs(glyph_set, r)
            entries_data.append((r, badge, wrapped))
        prepared_variants.append((eng_str, v.ocr_text, entries_data))

    out = bytearray()
    out.extend(struct.pack("<BHH", source_screen_id & 0xFF, max(0, min(65535, int(x))), max(0, min(65535, int(y)))))
    out.extend(_pack_str_u8(lbl_lookup))
    out.extend(_pack_str_u8(lbl_mode))
    out.extend(_pack_str_u8(lbl_anki))
    out.extend(_pack_str_u8(status_banner))
    out.extend(build_popup_glyphs(glyph_set))
    out.extend(struct.pack("<B", len(prepared_variants)))
    for eng_str, ocr_txt, entries_data in prepared_variants:
        out.extend(_pack_str_u8(eng_str))
        out.extend(_pack_str_u16(ocr_txt))
        out.extend(struct.pack("<H", len(entries_data)))
        for r, badge, wrapped in entries_data:
            out.extend(_pack_str_u16(r.matched_surface))
            out.extend(_pack_str_u16(r.context_text))
            out.extend(_pack_str_u16(r.headword))
            out.extend(_pack_str_u16(r.reading))
            out.extend(_pack_str_u16(r.pos or "Word"))
            out.extend(struct.pack("<BB", 1 if r.found else 0, 1 if r.ocr_disagreement else 0))
            out.extend(_pack_str_u8(badge))
            out.extend(_pack_str_u8(getattr(r, "rank_jlpt", "")))
            out.extend(struct.pack("<H", len(wrapped)))
            for ln in wrapped:
                out.extend(_pack_str_u16(ln))
    return bytes(out)


def build_screen_bundle(
    pipeline: JapaneseOCRDictionaryPipeline,
    screen_img: Image.Image,
    paragraphs_by_engine: Dict[str, List[StitchedParagraph]],
    source_screen_id: int,
    footer_keys: Optional[List[str]] = None,
) -> Tuple[bytes, List[List[OcrVariant]]]:
    """
    Build a zlib-compressed `SCREEN_BUNDLE` containing every clickable character hitbox,
    paragraph edge-snap box, deduplicated dictionary entries, OCR variants, and shared 4-bpp
    TTF glyph atlas for `source_screen_id`, so all single-word lookups run locally on the 3DS.
    """
    glyph_set, lbl_lookup, lbl_mode, lbl_anki = _init_bundle_glyph_set(footer_keys)

    raw_spots: List[Tuple[int, int, int, int, int, int]] = []
    for eng in OCR_ENGINE_NAMES:
        for p in paragraphs_by_engine.get(eng, []):
            for cs in p.chars:
                x1, y1, x2, y2 = cs.bbox
                cx = int(round((x1 + x2) * 0.5))
                cy = int(round((y1 + y2) * 0.5))
                if any(abs(cx - scx) <= 3 and abs(cy - scy) <= 3 for _, _, _, _, scx, scy in raw_spots):
                    continue
                raw_spots.append((int(round(x1)), int(round(y1)), int(round(x2)), int(round(y2)), cx, cy))

    def _nearest_spot_idx(tx: float, ty: float) -> int:
        best_i = 0
        best_d = float("inf")
        for idx, (_, _, _, _, scx, scy) in enumerate(raw_spots):
            d = (scx - tx) ** 2 + (scy - ty) ** 2
            if d < best_d:
                best_d = d
                best_i = idx
        return best_i

    seen_para_boxes: set = set()
    paragraphs_meta: List[Tuple[int, int, int, int, int, int]] = []
    if raw_spots:
        for eng in OCR_ENGINE_NAMES:
            for p in paragraphs_by_engine.get(eng, []):
                if not p.chars:
                    continue
                px1, py1, px2, py2 = p.bbox
                p_key = (px1, py1, px2, py2)
                if p_key in seen_para_boxes:
                    continue
                seen_para_boxes.add(p_key)
                c0 = p.chars[0].bbox
                c1 = p.chars[-1].bbox
                first_idx = _nearest_spot_idx((c0[0] + c0[2]) * 0.5, (c0[1] + c0[3]) * 0.5)
                last_idx = _nearest_spot_idx((c1[0] + c1[2]) * 0.5, (c1[1] + c1[3]) * 0.5)
                paragraphs_meta.append((int(px1), int(py1), int(px2), int(py2), first_idx, last_idx))

    context_map: Dict[str, int] = {}
    context_pool: List[str] = []
    entry_map: Dict[tuple, int] = {}
    entry_pool: List[Tuple[DictionaryResult, int, str, List[str]]] = []
    spot_variants_list: List[List[OcrVariant]] = []
    prepared_spots: List[Tuple[int, int, int, int, List[Tuple[str, str, List[int]]]]] = []

    for x1, y1, x2, y2, cx, cy in raw_spots:
        vars_ = pipeline.lookup_at_cursor_variants(
            screen_img,
            paragraphs_by_engine,
            cx,
            cy,
            allow_fallback_ocr=False,
            allow_short_tying_ocr=False,
        )
        spot_variants_list.append(vars_)
        v_encoded: List[Tuple[str, str, List[int]]] = []
        for v in vars_[:255]:
            eng_str = "+".join(v.engines) if v.engines else "All"
            _collect_glyph_text(glyph_set, 0, eng_str)
            _collect_glyph_text(glyph_set, 0, v.ocr_text[:40])
            e_indices: List[int] = []
            for r in v.results:
                ctx_txt = r.context_text or ""
                if ctx_txt not in context_map:
                    context_map[ctx_txt] = len(context_pool)
                    context_pool.append(ctx_txt)
                ctx_idx = context_map[ctx_txt]
                badge, wrapped = _collect_entry_glyphs(glyph_set, r)
                rank_jlpt = getattr(r, "rank_jlpt", "")
                ekey = (
                    r.matched_surface,
                    ctx_idx,
                    r.headword,
                    r.reading,
                    r.pos or "Word",
                    1 if r.found else 0,
                    1 if r.ocr_disagreement else 0,
                    badge,
                    rank_jlpt,
                    tuple(wrapped),
                )
                if ekey not in entry_map:
                    entry_map[ekey] = len(entry_pool)
                    entry_pool.append((r, ctx_idx, badge, rank_jlpt, wrapped))
                e_indices.append(entry_map[ekey])
            v_encoded.append((eng_str, v.ocr_text, e_indices))
        prepared_spots.append((x1, y1, x2, y2, v_encoded))

    out = bytearray()
    out.extend(struct.pack("<B", source_screen_id & 0xFF))
    out.extend(_pack_str_u8(lbl_lookup))
    out.extend(_pack_str_u8(lbl_mode))
    out.extend(_pack_str_u8(lbl_anki))
    out.extend(build_popup_glyphs(glyph_set))

    out.extend(struct.pack("<H", len(context_pool)))
    for ctx_txt in context_pool:
        out.extend(_pack_str_u16(ctx_txt))

    out.extend(struct.pack("<H", len(entry_pool)))
    for r, ctx_idx, badge, rank_jlpt, wrapped in entry_pool:
        out.extend(_pack_str_u16(r.matched_surface))
        out.extend(struct.pack("<H", ctx_idx))
        out.extend(_pack_str_u16(r.headword))
        out.extend(_pack_str_u16(r.reading))
        out.extend(_pack_str_u16(r.pos or "Word"))
        out.extend(struct.pack("<BB", 1 if r.found else 0, 1 if r.ocr_disagreement else 0))
        out.extend(_pack_str_u8(badge))
        out.extend(_pack_str_u8(rank_jlpt))
        out.extend(struct.pack("<H", len(wrapped)))
        for ln in wrapped:
            out.extend(_pack_str_u16(ln))

    out.extend(struct.pack("<H", len(prepared_spots)))
    for x1, y1, x2, y2, v_encoded in prepared_spots:
        out.extend(struct.pack("<hhhhB", x1, y1, x2, y2, len(v_encoded)))
        for eng_str, ocr_txt, e_indices in v_encoded:
            out.extend(_pack_str_u8(eng_str))
            out.extend(_pack_str_u16(ocr_txt))
            out.extend(struct.pack("<H", len(e_indices)))
            for ei in e_indices:
                out.extend(struct.pack("<H", ei))

    out.extend(struct.pack("<H", len(paragraphs_meta)))
    for px1, py1, px2, py2, f_idx, l_idx in paragraphs_meta:
        out.extend(struct.pack("<hhhhHH", px1, py1, px2, py2, f_idx, l_idx))

    raw = bytes(out)
    compressed = zlib.compress(raw, level=6)
    if len(compressed) < len(raw):
        return struct.pack("<BI", 1, len(raw)) + compressed, spot_variants_list
    return struct.pack("<BI", 0, len(raw)) + raw, spot_variants_list


def build_status_banner_packet(status_banner: str) -> bytes:
    """Pack a tiny footer status banner update with any missing small-font glyphs."""
    is_err = status_banner.startswith("Anki Error") or status_banner.startswith("Set AnkiWeb")
    is_pending = status_banner.startswith("Adding ") or status_banner.startswith("Translating ")
    level = 2 if is_err else (1 if is_pending else 0)
    glyph_set = {(0, ord(ch)) for ch in (status_banner or "") if ch != "\n"}
    glyph_set.add((0, ord("…")))
    return struct.pack("<B", level) + _pack_str_u16(status_banner) + build_popup_glyphs(glyph_set)


# Built-in 3DS Title ID -> Game Name table (indexed by 16-hex-char TitleID or 4-hex-char UniqueID)
BUILTIN_3DS_TITLES: Dict[str, str] = {
    "0004000000164800": "Pokémon Sun",
    "0004000000175E00": "Pokémon Moon",
    "00040000001B5000": "Pokémon Ultra Sun",
    "00040000001B5100": "Pokémon Ultra Moon",
    "0004000000055D00": "Pokémon X",
    "0004000000055E00": "Pokémon Y",
    "000400000011C400": "Pokémon Omega Ruby",
    "000400000011C500": "Pokémon Alpha Sapphire",
    "0004000000033400": "The Legend of Zelda: Ocarina of Time 3D",
    "0004000000033500": "The Legend of Zelda: Ocarina of Time 3D",
    "0004000000125500": "The Legend of Zelda: Majora's Mask 3D",
    "00040000000EC200": "The Legend of Zelda: A Link Between Worlds",
    "000400000009F100": "Fire Emblem Awakening",
    "0004000000179400": "Fire Emblem Fates",
    "00040000001B4000": "Fire Emblem Echoes: Shadows of Valentia",
    "00040000000B0F00": "Bravely Default",
    "00040000000FC500": "Bravely Default: For the Sequel",
    "0004000000086200": "Animal Crossing: New Leaf",
    "0004000000086300": "Animal Crossing: New Leaf",
    "0004000000084B00": "Dragon Quest VII",
    "0004000000136E00": "Dragon Quest VIII",
    "0004000000199A00": "Dragon Quest XI",
    "00040000000C9200": "Yo-kai Watch",
    "000400000008E500": "Shin Megami Tensei IV",
    "0004000000098800": "Fantasy Life",
    "0004000000126900": "Monster Hunter 4 Ultimate",
    "0004000000155400": "Monster Hunter Cross",
    "00040000001B7100": "The Great Ace Attorney 2",
    "0004000000132700": "The Great Ace Attorney",
}

_title_cache_lock = threading.Lock()
_title_cache_mem: Dict[str, str] = {}


def _load_anki_config() -> dict:
    if os.path.exists(ANKI_CONFIG_PATH):
        try:
            with open(ANKI_CONFIG_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
                if isinstance(data, dict):
                    return data
        except Exception as exc:
            logger.warning("Failed to parse %s: %s", ANKI_CONFIG_PATH, exc)
    return {}


def _load_anki_credentials() -> Tuple[str, str]:
    """Retrieve AnkiWeb username and password from environment or protected credentials file."""
    u = os.environ.get("ANKIWEB_USERNAME", "").strip()
    p = os.environ.get("ANKIWEB_PASSWORD", "").strip()
    if u and p:
        return u, p
    if os.path.exists(ANKI_CREDENTIALS_PATH):
        try:
            with open(ANKI_CREDENTIALS_PATH, "r", encoding="utf-8") as f:
                c_data = json.load(f)
                if isinstance(c_data, dict):
                    u = u or str(c_data.get("ankiweb_username", "") or "").strip()
                    p = p or str(c_data.get("ankiweb_password", "") or "").strip()
        except Exception as exc:
            logger.warning("Failed to load %s: %s", ANKI_CREDENTIALS_PATH, exc)
    # Fallback to anki_config.json for backwards compatibility
    if not u or not p:
        cfg = _load_anki_config()
        u = u or str(cfg.get("ankiweb_username", "") or "").strip()
        p = p or str(cfg.get("ankiweb_password", "") or "").strip()
    return u, p


def _load_gemini_api_keys() -> List[str]:
    """Retrieve ordered primary and fallback Gemini API keys from env or gitignored credentials files."""
    keys: List[str] = []

    def _add(val: object) -> None:
        s = str(val or "").strip()
        if s and s not in keys:
            keys.append(s)

    _add(os.environ.get("GEMINI_API_KEY", ""))
    _add(os.environ.get("GEMINI_FALLBACK_API_KEY", ""))
    for path in (GEMINI_CREDENTIALS_PATH, ANKI_CREDENTIALS_PATH, ANKI_CONFIG_PATH):
        if os.path.exists(path):
            try:
                with open(path, "r", encoding="utf-8") as f:
                    data = json.load(f)
                if isinstance(data, dict):
                    _add(data.get("gemini_api_key", ""))
                    _add(data.get("gemini_fallback_api_key", ""))
                    extra = data.get("gemini_fallback_api_keys", [])
                    if isinstance(extra, list):
                        for item in extra:
                            _add(item)
            except Exception as exc:
                logger.warning("Failed to load Gemini API key from %s: %s", path, exc)
    return keys


def validate_ankiconnect_url(url: str) -> Tuple[bool, str]:
    """Validate AnkiConnect URL to prevent SSRF and protocol smuggling."""
    try:
        parsed = urllib.parse.urlparse(url.strip())
        if parsed.scheme not in ("http", "https"):
            return False, "Invalid URL scheme: Must be http:// or https://"
        if not parsed.netloc:
            return False, "Invalid URL: Missing host"
        if not re.match(r"^[a-zA-Z0-9.\-_:]+$", parsed.netloc):
            return False, "Invalid characters in URL host"
        return True, ""
    except Exception as exc:
        return False, f"Malformed URL: {exc}"


def resolve_3ds_game_name(title_id: Union[int, str]) -> str:
    """
    Resolve a 64-bit 3DS Title ID (e.g. `0x0004000000164800`) or game title string to its game title for Anki deck naming.
    Checks:
      1. Direct game name string (e.g. from DS Streamer)
      2. `anki_config.json` custom `"title_names"` overrides
      3. Built-in 3DS title table (`BUILTIN_3DS_TITLES`)
      4. Local JSON cache (`anki_sync/title_cache.json`)
      5. Public 3DS TitleDB JSON lists (`3dsdb` US/JP/GB)
    """
    if isinstance(title_id, str):
        s = title_id.strip()
        if s.startswith("0x") or s.startswith("0X"):
            try:
                title_id = int(s, 16)
            except ValueError:
                return s
        elif len(s) == 16 and all(c in "0123456789abcdefABCDEF" for c in s):
            try:
                title_id = int(s, 16)
            except ValueError:
                return s
        else:
            return s if s else "Nintendo DS"

    if title_id == 0:
        return "3DS Game"

    tid_hex = f"{title_id:016X}"
    cfg = _load_anki_config()
    custom_map = cfg.get("title_names", {})
    if isinstance(custom_map, dict):
        for k, v in custom_map.items():
            if str(k).upper() == tid_hex and v:
                return str(v).strip()

    if tid_hex in BUILTIN_3DS_TITLES:
        return BUILTIN_3DS_TITLES[tid_hex]

    with _title_cache_lock:
        if not _title_cache_mem and os.path.exists(TITLE_CACHE_PATH):
            try:
                with open(TITLE_CACHE_PATH, "r", encoding="utf-8") as f:
                    loaded = json.load(f)
                    if isinstance(loaded, dict):
                        _title_cache_mem.update(loaded)
            except Exception:
                pass
        if tid_hex in _title_cache_mem:
            return _title_cache_mem[tid_hex]

    # Query public 3dsdb lists if not yet cached
    urls = [
        "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_US.json",
        "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_GB.json",
        "https://raw.githubusercontent.com/hax0kartik/3dsdb/master/jsons/list_JP.json",
    ]
    for url in urls:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "3DS-OCR-Server/1.0"})
            with urllib.request.urlopen(req, timeout=3.5) as resp:
                items = json.loads(resp.read().decode("utf-8", errors="replace"))
                if isinstance(items, list):
                    for item in items:
                        if str(item.get("TitleID", "")).upper() == tid_hex:
                            name = str(item.get("Name", "")).strip()
                            if name:
                                with _title_cache_lock:
                                    _title_cache_mem[tid_hex] = name
                                    os.makedirs(ANKI_SYNC_DIR, exist_ok=True)
                                    with open(TITLE_CACHE_PATH, "w", encoding="utf-8") as f:
                                        json.dump(_title_cache_mem, f, ensure_ascii=False, indent=2)
                                return name
        except Exception:
            continue

    return f"3DS ({tid_hex[-8:]})"


def is_pokemon_game(title: Union[int, str]) -> bool:
    """Check if title corresponds to a Pokémon game (for game-specific jargon/substitution)."""
    name = resolve_3ds_game_name(title)
    n = name.lower()
    return any(k in n for k in ["pokémon", "pokemon", "pocket monster", "ポケットモンスター", "ポケモン"])


@dataclass
class _AnkiQueueItem:
    result: DictionaryResult
    title_id: Union[int, str]
    screenshot_img: Optional[Image.Image]
    headword: str
    reading: str
    deck_name: str
    game_name: str
    anki_creds: Optional[Tuple[str, str]] = None
    account_key: str = "default"
    callbacks: List[Callable[[str, int], None]] = field(default_factory=list)
    done_events: List[threading.Event] = field(default_factory=list)
    status_result: str = ""


class AnkiExporter:
    """
    Thread-safe queued Anki card exporter supporting:
      1. Direct AnkiWeb Cloud Sync via the official `anki` Rust engine (`anki.collection.Collection`),
         using credentials from `anki_config.json` (or `ANKIWEB_USERNAME` / `ANKIWEB_PASSWORD` env vars).
      2. Automatic fallback/passthrough to local AnkiConnect (`http://127.0.0.1:8765`) if Anki Desktop
         is currently open with AnkiConnect installed.

    Rapid card additions are serialized through a background FIFO worker queue so
    AnkiConnect/AnkiWeb syncs never collide with back-to-back note creation, and
    syncing is coalesced until the queue drains.
    """

    SYNC_DEBOUNCE_SEC = 0.6
    ANKICONNECT_RETRY_BUDGET_SEC = 15.0
    ANKICONNECT_RETRY_DELAYS = (0.35, 0.5, 0.75, 1.0, 1.25, 1.5, 1.5, 2.0, 2.0)

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._cached_auths: Dict[Tuple[str, str], Any] = {}
        self._queue_lock = threading.Lock()
        self._queue_cv = threading.Condition(self._queue_lock)
        self._queue: deque[_AnkiQueueItem] = deque()
        self._active_item: Optional[_AnkiQueueItem] = None
        self._worker_thread: Optional[threading.Thread] = None
        self._pending_syncs: Dict[str, Tuple[str, Tuple[Any, ...]]] = {}

    @staticmethod
    def _account_key_for_creds(anki_creds: Optional[Tuple[str, str]]) -> str:
        if anki_creds and anki_creds[0] and anki_creds[1]:
            user_norm = anki_creds[0].strip().lower().encode("utf-8")
            return "user:" + hashlib.sha256(user_norm).hexdigest()[:16]
        return "default"

    @staticmethod
    def _sync_dir_for_creds(anki_creds: Optional[Tuple[str, str]]) -> str:
        if anki_creds and anki_creds[0] and anki_creds[1]:
            user_norm = anki_creds[0].strip().lower().encode("utf-8")
            user_hash = hashlib.sha256(user_norm).hexdigest()[:16]
            return os.path.join(ANKI_SYNC_DIR, "users", user_hash)
        return ANKI_SYNC_DIR

    @staticmethod
    def _auth_cache_key(username: str, password: str) -> Tuple[str, str]:
        return (
            username.strip().lower(),
            hashlib.sha256(password.encode("utf-8")).hexdigest(),
        )

    def _ensure_worker_locked(self) -> None:
        if self._worker_thread is None or not self._worker_thread.is_alive():
            self._worker_thread = threading.Thread(
                target=self._worker_loop,
                name="AnkiQueueWorker",
                daemon=True,
            )
            self._worker_thread.start()

    @staticmethod
    def _format_context_html(context_text: str, matched_surface: str) -> str:
        if not context_text:
            return ""
        if matched_surface and matched_surface in context_text:
            idx = context_text.find(matched_surface)
            before = html.escape(context_text[:idx])
            target = html.escape(matched_surface)
            after = html.escape(context_text[idx + len(matched_surface) :])
            return f'{before}<b style="color:#d97706;">{target}</b>{after}'
        return html.escape(context_text)

    @staticmethod
    def _encode_screenshot_jpeg(screenshot_img: Optional[Image.Image]) -> Optional[Tuple[str, bytes]]:
        if screenshot_img is None:
            return None
        buf = io.BytesIO()
        screenshot_img.convert("RGB").save(buf, format="JPEG", quality=90, optimize=True)
        jpeg_bytes = buf.getvalue()
        digest = hashlib.sha1(jpeg_bytes).hexdigest()[:12]
        return f"3ds_{digest}.jpg", jpeg_bytes

    @staticmethod
    def _strip_screenshot_html(front_html: str) -> str:
        """Remove trailing screenshot `<div style="margin-top:10px;"><img ...></div>` for duplicate comparison."""
        return re.sub(
            r'<div style="margin-top:10px;"><img [^>]*></div>\s*$',
            "",
            front_html or "",
        ).strip()

    @classmethod
    def _is_connection_refused(cls, exc: BaseException) -> bool:
        if isinstance(exc, ConnectionRefusedError):
            return True
        if getattr(exc, "winerror", None) == 10061 or getattr(exc, "errno", None) in (111, 10061):
            return True
        if isinstance(exc, urllib.error.URLError) and exc.reason is not None:
            if isinstance(exc.reason, BaseException):
                return cls._is_connection_refused(exc.reason)
            if "10061" in str(exc.reason) or "refused" in str(exc.reason).lower():
                return True
        return False

    @classmethod
    def _is_transient_ankiconnect_error(cls, exc: BaseException) -> bool:
        if cls._is_connection_refused(exc):
            return False
        if isinstance(
            exc,
            (
                ConnectionResetError,
                BrokenPipeError,
                ConnectionAbortedError,
                TimeoutError,
                socket.timeout,
                http.client.HTTPException,
            ),
        ):
            return True
        if isinstance(exc, urllib.error.URLError):
            reason = exc.reason
            if isinstance(reason, BaseException):
                return cls._is_transient_ankiconnect_error(reason)
            return True
        if isinstance(exc, OSError):
            return True
        if isinstance(exc, RuntimeError):
            msg = str(exc).lower()
            return any(
                marker in msg
                for marker in (
                    "lifecycle",
                    "busy",
                    "in progress",
                    "locked",
                    "try again",
                    "collection is closed",
                    "collection is not",
                    "syncing",
                )
            )
        return False

    @staticmethod
    def _ankiconnect_invoke(url: str, action: str, timeout: float = 5.0, **params: object) -> object:
        payload = json.dumps({"action": action, "version": 6, "params": params}).encode("utf-8")
        req = urllib.request.Request(
            url,
            data=payload,
            headers={"Content-Type": "application/json", "Connection": "close"},
        )
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            body = json.loads(resp.read().decode("utf-8"))
            if body.get("error"):
                raise RuntimeError(str(body["error"]))
            return body.get("result")

    def _try_ankiconnect(
        self,
        deck_name: str,
        headword: str,
        reading: str,
        front_html: str,
        back_html: str,
        tags: List[str],
        media_item: Optional[Tuple[str, bytes]] = None,
        url: str = "http://127.0.0.1:8765",
        defer_sync: bool = False,
    ) -> Tuple[Optional[str], bool]:
        """
        If Anki Desktop with AnkiConnect is running, check for duplicates and add note.
        Retries transient busy/sync/lifecycle errors within a bounded window.
        Returns `(status_msg_or_None, needs_sync)`.
        """
        valid, err_msg = validate_ankiconnect_url(url)
        if not valid:
            return f"Anki Error: {err_msg}", False

        hw_esc = html.escape(headword)
        rd_esc = html.escape(reading) if reading and reading != headword else ""
        hw_prefix = f'<div style="font-size:28px;font-weight:bold;">{hw_esc}</div>'
        hw_rd_prefix = (
            hw_prefix + f'<div style="font-size:18px;color:#888;">【{rd_esc}】</div>'
            if rd_esc
            else hw_prefix
        )
        base_front_clean = self._strip_screenshot_html(front_html)
        escaped_deck = deck_name.replace("\\", "\\\\").replace('"', '\\"')
        escaped_hw = headword.replace("\\", "\\\\").replace('"', '\\"')

        deadline = time.monotonic() + self.ANKICONNECT_RETRY_BUDGET_SEC
        verify_added_on_retry = False
        attempt = 0

        while True:
            add_note_attempted = False
            try:
                self._ankiconnect_invoke(url, "createDeck", timeout=5.0, deck=deck_name)

                existing_nids = (
                    self._ankiconnect_invoke(
                        url, "findNotes", timeout=5.0, query=f'deck:"{escaped_deck}" "{escaped_hw}"'
                    )
                    or []
                )
                if existing_nids:
                    notes_info = self._ankiconnect_invoke(url, "notesInfo", timeout=5.0, notes=existing_nids) or []
                    for ninfo in notes_info:
                        if not isinstance(ninfo, dict):
                            continue
                        flds = ninfo.get("fields", {})
                        existing_front = str((flds.get("Front") or {}).get("value", "") or "")
                        existing_clean = self._strip_screenshot_html(existing_front)
                        if (
                            existing_clean == base_front_clean
                            or existing_clean.startswith(hw_rd_prefix)
                            or existing_clean.startswith(hw_prefix)
                        ):
                            if verify_added_on_retry:
                                return f"Added to Anki: {deck_name}", True
                            return f"Already in Anki: {deck_name}", False

                final_front = front_html
                if media_item is not None:
                    fname, jpeg_bytes = media_item
                    fname = os.path.basename(fname)
                    b64_data = base64.b64encode(jpeg_bytes).decode("ascii")
                    saved_fname = (
                        self._ankiconnect_invoke(url, "storeMediaFile", timeout=8.0, filename=fname, data=b64_data)
                        or fname
                    )
                    final_front += (
                        f'<div style="margin-top:10px;">'
                        f'<img src="{html.escape(str(saved_fname))}" style="max-width:100%;height:auto;border-radius:6px;">'
                        f"</div>"
                    )

                note = {
                    "deckName": deck_name,
                    "modelName": "Basic",
                    "fields": {"Front": final_front, "Back": back_html},
                    "options": {"allowDuplicate": False, "duplicateScope": "deck"},
                    "tags": tags,
                }
                add_note_attempted = True
                self._ankiconnect_invoke(url, "addNote", timeout=8.0, note=note)
                if not defer_sync:
                    threading.Thread(
                        target=self._sync_ankiconnect,
                        args=(url,),
                        daemon=True,
                    ).start()
                return f"Added to Anki: {deck_name}", True

            except Exception as exc:
                msg = str(exc)
                if "duplicate" in msg.lower():
                    if verify_added_on_retry:
                        return f"Added to Anki: {deck_name}", True
                    return f"Already in Anki: {deck_name}", False

                if self._is_connection_refused(exc):
                    return None, False

                if self._is_transient_ankiconnect_error(exc):
                    if add_note_attempted and not isinstance(exc, RuntimeError):
                        verify_added_on_retry = True
                    now = time.monotonic()
                    delay = self.ANKICONNECT_RETRY_DELAYS[
                        min(attempt, len(self.ANKICONNECT_RETRY_DELAYS) - 1)
                    ]
                    if now + delay < deadline:
                        attempt += 1
                        logger.info(
                            "AnkiConnect busy while adding '%s' (%s); retrying in %.2fs (attempt %d)...",
                            _sanitize_log_str(headword),
                            _sanitize_log_str(msg[:80]),
                            delay,
                            attempt,
                        )
                        time.sleep(delay)
                        continue
                    return f"Anki Error: {msg[:30]}", False

                return f"Anki Error: {msg[:30]}", False

    def _sync_ankiconnect(self, url: str) -> None:
        """Run a serialized AnkiConnect `sync` with brief transient retry."""
        for attempt in range(3):
            try:
                self._ankiconnect_invoke(url, "sync", timeout=15.0)
                logger.info("AnkiConnect sync completed.")
                return
            except Exception as exc:
                if self._is_transient_ankiconnect_error(exc) and attempt < 2:
                    logger.info("AnkiConnect sync deferred (%s); retrying...", _sanitize_log_str(str(exc)[:80]))
                    time.sleep(1.0 * (attempt + 1))
                    continue
                logger.warning("AnkiConnect sync warning: %s", exc)
                return

    @staticmethod
    def _resolve_deck_for_game(title_id: Union[int, str], cfg: dict) -> Tuple[str, str]:
        game_name = resolve_3ds_game_name(title_id)
        prefix = str(cfg.get("deck_prefix", "") or "").strip()
        deck_name = f"{prefix}::{game_name}" if prefix else game_name
        return game_name, deck_name

    def _record_pending_sync_locked(self, sync_info: Tuple[str, Tuple[Any, ...]]) -> None:
        mode = sync_info[0]
        args = sync_info[1]
        if mode == "ankiconnect":
            ac_url = str(args[0])
            self._pending_syncs[f"ankiconnect:{ac_url}"] = ("ankiconnect", (ac_url,))
        elif mode == "ankiweb":
            username, password, has_media, sync_dir = args
            key = f"ankiweb:{sync_dir}"
            prev_media = False
            prev = self._pending_syncs.get(key)
            if prev is not None and prev[0] == "ankiweb" and len(prev[1]) >= 3:
                prev_media = bool(prev[1][2])
            self._pending_syncs[key] = (
                "ankiweb",
                (str(username), str(password), prev_media or bool(has_media), str(sync_dir)),
            )

    def _consume_pending_sync_locked(self) -> Optional[Tuple[str, Tuple[Any, ...]]]:
        if not self._pending_syncs:
            return None
        first_key = next(iter(self._pending_syncs))
        return self._pending_syncs.pop(first_key)

    def _run_pending_sync(self, sync_task: Tuple[str, Tuple[Any, ...]]) -> None:
        mode, args = sync_task
        if mode == "ankiconnect":
            self._sync_ankiconnect(str(args[0]))
        elif mode == "ankiweb":
            username, password, has_media, sync_dir = args
            self._sync_to_ankiweb_bg(str(username), str(password), bool(has_media), str(sync_dir))

    def _worker_loop(self) -> None:
        while True:
            item: Optional[_AnkiQueueItem] = None
            sync_task: Optional[Tuple[str, Tuple[Any, ...]]] = None

            with self._queue_lock:
                while not self._queue:
                    if self._pending_syncs:
                        self._queue_cv.wait(timeout=self.SYNC_DEBOUNCE_SEC)
                        if self._queue:
                            continue
                        sync_task = self._consume_pending_sync_locked()
                        break
                    self._queue_cv.wait()

                if sync_task is None and self._queue:
                    item = self._queue.popleft()
                    self._active_item = item

            if sync_task is not None:
                self._run_pending_sync(sync_task)
                continue

            if item is None:
                continue

            status_msg = "Anki Error: Unknown error"
            try:
                status_msg, sync_info = self._process_queue_item(item)
                if sync_info is not None:
                    with self._queue_lock:
                        self._record_pending_sync_locked(sync_info)
            except Exception as exc:
                logger.exception("Unhandled Anki queue worker error: %s", exc)
                status_msg = f"Anki Error: {str(exc)[:30]}"
            finally:
                with self._queue_lock:
                    self._active_item = None
                    remaining = len(self._queue)
                    item.status_result = status_msg
                    done_events = list(item.done_events)
                    callbacks = list(item.callbacks)

                for ev in done_events:
                    ev.set()
                for cb in callbacks:
                    try:
                        cb(status_msg, remaining)
                    except Exception as cb_exc:
                        logger.debug("Anki queue callback error: %s", cb_exc)

    def _process_queue_item(
        self, item: _AnkiQueueItem
    ) -> Tuple[str, Optional[Tuple[str, Tuple[Any, ...]]]]:
        result = item.result
        if not result.found or not result.headword:
            return "Anki Error: No valid dictionary match", None

        cfg = _load_anki_config()
        game_name, deck_name = self._resolve_deck_for_game(item.title_id, cfg)

        hw_esc = html.escape(result.headword)
        rd_esc = html.escape(result.reading) if result.reading and result.reading != result.headword else ""
        ctx_html = self._format_context_html(result.context_text, result.matched_surface)
        pos_esc = html.escape(result.pos)
        gloss_html = "<br>".join(html.escape(g) for g in result.glosses)

        base_front_html = f'<div style="font-size:28px;font-weight:bold;">{hw_esc}</div>'
        if rd_esc:
            base_front_html += f'<div style="font-size:18px;color:#888;">【{rd_esc}】</div>'
        if ctx_html:
            base_front_html += f'<hr><div style="font-size:16px;color:#666;margin-top:6px;">{ctx_html}</div>'

        back_html = f'<div style="font-size:13px;color:#4a9;margin-bottom:6px;">{pos_esc}</div><div>{gloss_html}</div>'
        tag_game = re.sub(r"\W+", "_", game_name).strip("_") or "3ds"
        tags = ["3ds_ocr", tag_game]
        media_item = self._encode_screenshot_jpeg(item.screenshot_img)

        with self._lock:
            has_session_creds = bool(
                item.anki_creds and item.anki_creds[0] and item.anki_creds[1]
            )
            if has_session_creds:
                username, password = item.anki_creds[0].strip(), item.anki_creds[1]
                sync_dir = self._sync_dir_for_creds(item.anki_creds)
            else:
                if bool(cfg.get("multi_user_mode", False)):
                    return "AnkiWeb login required (Multi-3DS mode)", None

                anki_mode = cfg.get("anki_mode")
                if not anki_mode:
                    if cfg.get("use_ankiconnect", True):
                        anki_mode = "ankiconnect"
                    elif _load_anki_credentials()[0]:
                        anki_mode = "ankiweb"
                    else:
                        anki_mode = "ankiconnect"

                if anki_mode == "disabled":
                    return "Anki disabled in settings", None

                if anki_mode == "ankiconnect":
                    ac_url = cfg.get("ankiconnect_url", "http://127.0.0.1:8765")
                    ac_status, needs_sync = self._try_ankiconnect(
                        deck_name,
                        result.headword,
                        result.reading,
                        base_front_html,
                        back_html,
                        tags,
                        media_item,
                        url=ac_url,
                        defer_sync=True,
                    )
                    if ac_status is not None:
                        sync_info = ("ankiconnect", (ac_url,)) if needs_sync else None
                        return ac_status, sync_info
                    return "Anki Error: Start Anki Desktop with AnkiConnect", None

                # 2. Direct AnkiWeb Cloud Sync via official `anki` library (server default)
                username, password = _load_anki_credentials()
                if not username or not password:
                    return "Set AnkiWeb login in GUI Anki Settings", None
                sync_dir = ANKI_SYNC_DIR

            try:
                from anki.collection import Collection
                from anki.sync import SyncAuth, SyncOutput
            except ImportError:
                return "Anki Error: pip install anki required", None

            os.makedirs(sync_dir, exist_ok=True)
            col_path = os.path.join(sync_dir, "collection.anki2")
            is_fresh_mirror = not os.path.exists(col_path)
            auth_key = self._auth_cache_key(username, password)

            col = None
            try:
                col = Collection(col_path)

                # Verify credentials before mutating mirror for new session accounts, and download initial mirror if empty
                if is_fresh_mirror or col.note_count() == 0 or (has_session_creds and auth_key not in self._cached_auths):
                    auth = self._cached_auths.get(auth_key)
                    if auth is None:
                        auth = col.sync_login(username, password, None)
                        self._cached_auths[auth_key] = auth
                    if is_fresh_mirror or col.note_count() == 0:
                        try:
                            sync_out = col.sync_collection(auth, sync_media=False)
                        except Exception as first_sync_exc:
                            if "sync again" in str(first_sync_exc).lower():
                                auth = col.sync_login(username, password, None)
                                self._cached_auths[auth_key] = auth
                                sync_out = col.sync_collection(auth, sync_media=False)
                            else:
                                raise
                        if sync_out.new_endpoint:
                            auth = SyncAuth(hkey=auth.hkey, endpoint=sync_out.new_endpoint)
                            self._cached_auths[auth_key] = auth
                        if sync_out.required in (SyncOutput.FULL_DOWNLOAD, SyncOutput.FULL_SYNC):
                            col.full_upload_or_download(
                                auth=auth, server_usn=sync_out.server_media_usn, upload=False
                            )
                            col.close()
                            col = Collection(col_path)

                did = col.decks.id(deck_name)
                model = col.models.by_name("Basic") or col.models.all()[0]

                # Check for existing duplicate headword/sentence in the target deck (ignoring screenshot filename hash)
                escaped_query_hw = result.headword.replace("\\", "\\\\").replace('"', '\\"')
                escaped_deck = deck_name.replace("\\", "\\\\").replace('"', '\\"')
                existing_nids = col.find_notes(f'deck:"{escaped_deck}" "{escaped_query_hw}"')
                hw_prefix = f'<div style="font-size:28px;font-weight:bold;">{hw_esc}</div>'
                base_front_clean = self._strip_screenshot_html(base_front_html)
                for nid in existing_nids:
                    existing_note = col.get_note(nid)
                    if existing_note.fields:
                        existing_clean = self._strip_screenshot_html(existing_note.fields[0])
                        if existing_clean == base_front_clean or existing_clean.startswith(hw_prefix):
                            return f"Already in Anki: {game_name}", None

                front_html = base_front_html
                if media_item is not None:
                    fname, jpeg_bytes = media_item
                    saved_fname = col.media.write_data(fname, jpeg_bytes)
                    front_html += (
                        f'<div style="margin-top:10px;">'
                        f'<img src="{html.escape(saved_fname)}" style="max-width:100%;height:auto;border-radius:6px;">'
                        f"</div>"
                    )

                action_label = "Added to Anki"
                note = col.new_note(model)
                note.fields[0] = front_html
                if len(note.fields) > 1:
                    note.fields[1] = back_html
                note.tags = tags
                col.add_note(note, did)
            except Exception as exc:
                self._cached_auths.pop(auth_key, None)
                logger.exception("Local Anki note write failed: %s", exc)
                return f"Anki Error: {str(exc)[:30]}", None
            finally:
                if col is not None:
                    try:
                        col.close()
                    except Exception:
                        pass

        return f"{action_label}: {game_name}", ("ankiweb", (username, password, media_item is not None, sync_dir))

    def _enqueue_item(
        self,
        result: DictionaryResult,
        title_id: Union[int, str],
        screenshot_img: Optional[Image.Image] = None,
        on_done: Optional[Callable[[str, int], None]] = None,
        done_event: Optional[threading.Event] = None,
        anki_creds: Optional[Tuple[str, str]] = None,
    ) -> Tuple[int, str, Optional[_AnkiQueueItem]]:
        if not result.found or not result.headword:
            return 0, "Anki Error: No valid dictionary match", None

        cfg = _load_anki_config()
        game_name, deck_name = self._resolve_deck_for_game(title_id, cfg)
        account_key = self._account_key_for_creds(anki_creds)

        with self._queue_lock:
            existing: Optional[_AnkiQueueItem] = None
            for q_item in self._queue:
                if (
                    q_item.account_key == account_key
                    and q_item.deck_name == deck_name
                    and q_item.headword == result.headword
                    and q_item.reading == result.reading
                ):
                    existing = q_item
                    break

            if existing is not None:
                if on_done is not None:
                    existing.callbacks.append(on_done)
                if done_event is not None:
                    existing.done_events.append(done_event)
                target_item = existing
            else:
                target_item = _AnkiQueueItem(
                    result=result,
                    title_id=title_id,
                    screenshot_img=screenshot_img,
                    headword=result.headword,
                    reading=result.reading,
                    deck_name=deck_name,
                    game_name=game_name,
                    anki_creds=anki_creds,
                    account_key=account_key,
                    callbacks=[on_done] if on_done is not None else [],
                    done_events=[done_event] if done_event is not None else [],
                )
                self._queue.append(target_item)
                self._ensure_worker_locked()
                self._queue_cv.notify_all()

            q_len = len(self._queue) + (1 if self._active_item is not None else 0)

        hw_short = result.headword if len(result.headword) <= 10 else result.headword[:9] + "…"
        if q_len <= 1:
            immediate_banner = f"Adding '{hw_short}' to Anki..."
        else:
            immediate_banner = f"Adding '{hw_short}' ({q_len} queued)..."
        return q_len, immediate_banner, target_item

    def enqueue_card_for_game(
        self,
        result: DictionaryResult,
        title_id: Union[int, str],
        screenshot_img: Optional[Image.Image] = None,
        on_done: Optional[Callable[[str, int], None]] = None,
        anki_creds: Optional[Tuple[str, str]] = None,
    ) -> Tuple[int, str]:
        """
        Enqueue a card add without blocking the caller.
        Returns `(queue_depth, immediate_status_banner)`.
        """
        q_len, immediate_banner, _ = self._enqueue_item(
            result,
            title_id,
            screenshot_img=screenshot_img,
            on_done=on_done,
            anki_creds=anki_creds,
        )
        return q_len, immediate_banner

    def add_card_for_game(
        self,
        result: DictionaryResult,
        title_id: Union[int, str],
        screenshot_img: Optional[Image.Image] = None,
        anki_creds: Optional[Tuple[str, str]] = None,
    ) -> str:
        """
        Add `result` (along with the full OCR context sentence and 3DS screenshot)
        to the Anki deck for `title_id` via the serialized queue and wait for completion.
        Returns a short status message to render on the 3DS popup footer.
        """
        done_evt = threading.Event()
        q_len, immediate_banner, target_item = self._enqueue_item(
            result,
            title_id,
            screenshot_img=screenshot_img,
            done_event=done_evt,
            anki_creds=anki_creds,
        )
        if q_len == 0 or target_item is None:
            return immediate_banner
        done_evt.wait()
        return target_item.status_result

    def _sync_to_ankiweb_bg(
        self, username: str, password: str, has_media: bool, sync_dir: str = ANKI_SYNC_DIR
    ) -> None:
        """Background worker that syncs `collection.anki2` and newly written screenshots to AnkiWeb."""
        with self._lock:
            col = None
            auth_key = self._auth_cache_key(username, password)
            try:
                from anki.collection import Collection
                from anki.sync import SyncAuth, SyncOutput

                os.makedirs(sync_dir, exist_ok=True)
                col_path = os.path.join(sync_dir, "collection.anki2")
                col = Collection(col_path)

                auth = self._cached_auths.get(auth_key)
                if auth is None:
                    auth = col.sync_login(username, password, None)
                    self._cached_auths[auth_key] = auth

                try:
                    sync_out = col.sync_collection(auth, sync_media=False)
                except Exception as first_sync_exc:
                    if "sync again" in str(first_sync_exc).lower():
                        auth = col.sync_login(username, password, None)
                        self._cached_auths[auth_key] = auth
                        sync_out = col.sync_collection(auth, sync_media=False)
                    else:
                        raise
                if sync_out.new_endpoint:
                    auth = SyncAuth(hkey=auth.hkey, endpoint=sync_out.new_endpoint)
                    self._cached_auths[auth_key] = auth

                if sync_out.required in (SyncOutput.FULL_UPLOAD, SyncOutput.FULL_SYNC):
                    col.full_upload_or_download(
                        auth=auth, server_usn=sync_out.server_media_usn, upload=True
                    )

                if has_media:
                    upstream_endpoint = auth.endpoint
                    if not upstream_endpoint:
                        host_num = getattr(sync_out, "host_number", 0)
                        upstream_endpoint = (
                            f"https://sync{host_num}.ankiweb.net/"
                            if host_num
                            else "https://sync.ankiweb.net/"
                        )

                    with _upload_only_media_proxy(upstream_endpoint) as proxy_url:
                        proxy_auth = SyncAuth(hkey=auth.hkey, endpoint=proxy_url)
                        col.sync_media(proxy_auth)
                        deadline = time.monotonic() + 30.0
                        still_active = False
                        try:
                            while time.monotonic() < deadline:
                                st = col.media_sync_status()
                                if not st.active:
                                    break
                                time.sleep(0.05)
                            else:
                                still_active = True
                        finally:
                            if still_active:
                                try:
                                    col.abort_media_sync()
                                except Exception:
                                    pass

                logger.info("Background AnkiWeb sync completed.")
            except Exception as exc:
                self._cached_auths.pop(auth_key, None)
                logger.warning("Background AnkiWeb sync error: %s", exc)
            finally:
                if col is not None:
                    try:
                        col.close()
                    except Exception:
                        pass


def _zstd_raw_frame(data: bytes) -> bytes:
    """Wrap raw bytes in a Zstandard single-segment raw-block frame (no external deps)."""
    out = bytearray(b"\x28\xb5\x2f\xfd\xa0") + bytearray(struct.pack("<I", len(data)))
    max_blk = 128 * 1024
    offset = 0
    while True:
        chunk = data[offset : offset + max_blk]
        offset += len(chunk)
        is_last = 1 if offset >= len(data) else 0
        hdr = (len(chunk) << 3) | is_last
        out.extend(hdr.to_bytes(3, "little"))
        out.extend(chunk)
        if is_last:
            break
    return bytes(out)


def _read_http_body(handler: http.server.BaseHTTPRequestHandler) -> bytes:
    """Read an HTTP request body, supporting both chunked transfer encoding and Content-Length."""
    te = handler.headers.get("Transfer-Encoding", "").lower()
    if "chunked" in te:
        chunks = []
        while True:
            line = handler.rfile.readline().strip()
            if not line:
                break
            size = int(line.split(b";", 1)[0], 16)
            if size == 0:
                handler.rfile.readline()
                break
            chunks.append(handler.rfile.read(size))
            handler.rfile.readline()
        return b"".join(chunks)
    length = int(handler.headers.get("Content-Length", 0))
    return handler.rfile.read(length) if length > 0 else b""


@contextmanager
def _upload_only_media_proxy(upstream_endpoint: str):
    """
    Local HTTP proxy that turns `col.sync_media()` into an upload-only sync:
    intercepts `msync/mediaChanges` (empty) and `msync/mediaSanity` ("OK")
    while forwarding `msync/begin` and `msync/uploadChanges` to AnkiWeb.
    """
    import requests

    upstream = upstream_endpoint.rstrip("/")
    sess = requests.Session()

    class ProxyHandler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def handle(self) -> None:
            try:
                super().handle()
            except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
                pass

        def _send_zstd_json(self, raw_json: bytes) -> None:
            resp_body = _zstd_raw_frame(raw_json)
            self.send_response(200)
            self.send_header("anki-original-size", str(len(raw_json)))
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(resp_body)))
            self.end_headers()
            self.wfile.write(resp_body)

        def do_POST(self) -> None:
            body = _read_http_body(self)
            path = self.path
            if path.endswith("/msync/mediaChanges"):
                self._send_zstd_json(b'{"data":[]}')
                return
            if path.endswith("/msync/mediaSanity"):
                self._send_zstd_json(b'{"data":"OK"}')
                return
            fwd_headers = {
                k: v
                for k, v in self.headers.items()
                if k.lower() not in ("host", "content-length", "connection", "transfer-encoding")
            }
            r = sess.post(upstream + path, data=body, headers=fwd_headers, timeout=30)
            resp_body = r.content
            self.send_response(r.status_code)
            for k, v in r.headers.items():
                if k.lower() not in (
                    "transfer-encoding",
                    "connection",
                    "content-length",
                    "content-encoding",
                ):
                    self.send_header(k, v)
            self.send_header("Content-Length", str(len(resp_body)))
            self.end_headers()
            self.wfile.write(resp_body)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), ProxyHandler)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    try:
        yield f"http://127.0.0.1:{srv.server_address[1]}/"
    finally:
        srv.shutdown()
        srv.server_close()
        sess.close()


_anki_exporter = AnkiExporter()


X25519_KEY_SIZE = 32
STREAM_MAC_SIZE = 16
STREAM_DIR_C2S = 0x01
STREAM_DIR_S2C = 0x02

_X25519_P = (1 << 255) - 19
_X25519_A24 = 121665


def x25519_scalarmult(scalar_bytes: bytes, point_bytes: bytes) -> bytes:
    """
    RFC 7748 X25519 Elliptic Curve Diffie-Hellman over GF(2^255 - 19).
    Matches `x25519_scalarmult` in `sha256.h` bit-for-bit using only standard library Python.
    """
    s = bytearray(scalar_bytes[:32])
    s[0] &= 248
    s[31] = (s[31] & 127) | 64
    k = int.from_bytes(s, "little")

    u_arr = bytearray(point_bytes[:32])
    u_arr[31] &= 127
    u = int.from_bytes(u_arr, "little")

    x_1 = u
    x_2, z_2 = 1, 0
    x_3, z_3 = u, 1
    swap = 0

    for t in range(254, -1, -1):
        k_t = (k >> t) & 1
        swap ^= k_t
        mask = -swap
        dummy_x = mask & (x_2 ^ x_3)
        x_2 ^= dummy_x
        x_3 ^= dummy_x
        dummy_z = mask & (z_2 ^ z_3)
        z_2 ^= dummy_z
        z_3 ^= dummy_z
        swap = k_t

        a = (x_2 + z_2) % _X25519_P
        aa = (a * a) % _X25519_P
        b = (x_2 - z_2) % _X25519_P
        bb = (b * b) % _X25519_P
        e = (aa - bb) % _X25519_P
        c = (x_3 + z_3) % _X25519_P
        d = (x_3 - z_3) % _X25519_P
        da = (d * a) % _X25519_P
        cb = (c * b) % _X25519_P
        x_3 = pow(da + cb, 2, _X25519_P)
        z_3 = (x_1 * pow(da - cb, 2, _X25519_P)) % _X25519_P
        x_2 = (aa * bb) % _X25519_P
        z_2 = (e * (aa + _X25519_A24 * e)) % _X25519_P

    mask = -swap
    dummy_x = mask & (x_2 ^ x_3)
    x_2 ^= dummy_x
    x_3 ^= dummy_x
    dummy_z = mask & (z_2 ^ z_3)
    z_2 ^= dummy_z
    z_3 ^= dummy_z

    res = (x_2 * pow(z_2, _X25519_P - 2, _X25519_P)) % _X25519_P
    return res.to_bytes(32, "little")


def x25519_basepoint(secret_bytes: bytes) -> bytes:
    """Compute X25519 public key from 32-byte secret key using basepoint u = 9."""
    return x25519_scalarmult(secret_bytes, b"\x09" + b"\x00" * 31)


class SHA256Stream:
    """
    Directional SHA-256-CTR stream cipher + Encrypt-then-MAC (Trunc16 HMAC-SHA256)
    derived from X25519 `shared_secret` + ephemeral public keys (`s_pub`, `c_pub`) + `psk`,
    matching `sha256_stream_t` in `sha256.h`.
    """

    def __init__(
        self,
        shared_secret: bytes,
        s_pub: bytes,
        c_pub: bytes,
        psk: bytes,
        direction: int,
    ) -> None:
        self.enc_key = hashlib.sha256(shared_secret + s_pub + c_pub + psk + b"ENC").digest()
        self.mac_key = hashlib.sha256(shared_secret + s_pub + c_pub + psk + b"MAC").digest()
        self.dir_byte = bytes([direction])
        self.counter = 0
        self.seq = 0
        self._ks_buf = b""
        self._ks_pos = 0

    def _xor_crypt(self, data: bytes) -> bytes:
        n = len(data)
        if n == 0:
            return b""

        rem = self._ks_buf[self._ks_pos :]
        needed = n - len(rem)
        if needed <= 0:
            self._ks_pos += n
            ks = rem[:n]
        else:
            num_blocks = (needed + 31) // 32
            prefix = self.enc_key + self.dir_byte
            blocks = [
                hashlib.sha256(prefix + struct.pack("!Q", self.counter + i)).digest()
                for i in range(num_blocks)
            ]
            self.counter += num_blocks
            full_ks = rem + b"".join(blocks)
            ks = full_ks[:n]
            self._ks_buf = full_ks[n:]
            self._ks_pos = 0

        arr_data = np.frombuffer(data, dtype=np.uint8)
        arr_ks = np.frombuffer(ks, dtype=np.uint8)
        return np.bitwise_xor(arr_data, arr_ks).tobytes()

    def send_frame(self, sock: socket.socket, plaintext: bytes) -> None:
        """Encrypt `plaintext` and send `ciphertext || Trunc16(HMAC-SHA256)`."""
        aad = self.dir_byte + struct.pack("!Q", self.seq)
        self.seq += 1
        ciphertext = self._xor_crypt(plaintext)
        mac = hmac.new(self.mac_key, aad + ciphertext, hashlib.sha256).digest()[:STREAM_MAC_SIZE]
        sock.sendall(ciphertext + mac)

    def recv_frame(
        self,
        sock: socket.socket,
        length: int,
        max_total_sec: Optional[float] = None,
    ) -> Optional[bytes]:
        """Receive `length` encrypted bytes + 16B MAC, verify MAC in constant time, and decrypt."""
        frame_bytes = recv_exact(sock, length + STREAM_MAC_SIZE, max_total_sec=max_total_sec)
        if frame_bytes is None:
            return None
        ciphertext = frame_bytes[:length]
        recv_mac = frame_bytes[length:]

        aad = self.dir_byte + struct.pack("!Q", self.seq)
        self.seq += 1
        expected_mac = hmac.new(self.mac_key, aad + ciphertext, hashlib.sha256).digest()[:STREAM_MAC_SIZE]
        if not hmac.compare_digest(recv_mac, expected_mac):
            raise ValueError("Stream HMAC verification failed (tampered or corrupt frame)")

        return self._xor_crypt(ciphertext)


# Per-IP failed authentication tracking & anti-exhaustion pre-auth management
MAX_TRACKED_AUTH_IPS = 2048

_auth_lock = threading.Lock()
_auth_failures: Dict[str, Tuple[List[float], float]] = {}  # ip -> (failure_timestamps, ban_until_epoch)
_session_semaphore = threading.BoundedSemaphore(MAX_CONCURRENT_CONNECTIONS)
_active_sessions_lock = threading.Lock()
_active_session_socks: List[Tuple[socket.socket, str]] = []

_preauth_lock = threading.Lock()
_preauth_active: List[Tuple[socket.socket, str, float]] = []  # [(conn, ip, start_time), ...]
_preauth_per_ip: Dict[str, int] = {}


def _try_register_preauth(conn: socket.socket, ip: str) -> bool:
    """
    Register an incoming socket for pre-auth handshake:
    1. Rejects if the same IP already has >= MAX_PREAUTH_PER_IP connections in pre-auth.
    2. If global MAX_PREAUTH_CONNECTIONS is reached, evicts the OLDEST incomplete pre-auth
       connection (closing its socket) rather than denying the new connection.
    Returns True if admitted, False if rejected.
    """
    now = time.monotonic()
    with _preauth_lock:
        ip_cnt = _preauth_per_ip.get(ip, 0)
        if ip_cnt >= MAX_PREAUTH_PER_IP:
            return False

        if len(_preauth_active) >= MAX_PREAUTH_CONNECTIONS:
            stale_sock, stale_ip, _ = _preauth_active.pop(0)
            _preauth_per_ip[stale_ip] = max(0, _preauth_per_ip.get(stale_ip, 1) - 1)
            try:
                stale_sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                stale_sock.close()
            except OSError:
                pass
            logger.debug("Evicted stalled pre-auth connection from %s to admit %s", stale_ip, ip)

        _preauth_active.append((conn, ip, now))
        _preauth_per_ip[ip] = ip_cnt + 1
        return True


def _unregister_preauth(conn: socket.socket, ip: str) -> None:
    with _preauth_lock:
        orig_len = len(_preauth_active)
        _preauth_active[:] = [(s, i, t) for (s, i, t) in _preauth_active if s is not conn]
        if len(_preauth_active) < orig_len:
            if ip in _preauth_per_ip:
                new_cnt = _preauth_per_ip[ip] - 1
                if new_cnt <= 0:
                    del _preauth_per_ip[ip]
                else:
                    _preauth_per_ip[ip] = new_cnt


LOOPBACK_IPS = {"127.0.0.1", "::1", "localhost"}
AUTH_FAILURE_WINDOW_SEC = 300.0


def _prune_auth_failures_locked(now: float) -> None:
    """Prune expired ban entries and old failure records; evict oldest if at capacity."""
    expired = [
        k for k, (fails, ban_until) in _auth_failures.items()
        if (ban_until > 0.0 and now >= ban_until) or
           (ban_until == 0.0 and not any((now - t) < AUTH_FAILURE_WINDOW_SEC for t in fails))
    ]
    for k in expired:
        _auth_failures.pop(k, None)

    while len(_auth_failures) >= MAX_TRACKED_AUTH_IPS:
        oldest_k = next(iter(_auth_failures))
        _auth_failures.pop(oldest_k, None)


def _instant_ban_scanner(ip: str) -> None:
    if ip in LOOPBACK_IPS:
        return
    now = time.monotonic()
    with _auth_lock:
        if ip not in _auth_failures and len(_auth_failures) >= MAX_TRACKED_AUTH_IPS:
            _prune_auth_failures_locked(now)
        _auth_failures[ip] = ([now] * MAX_AUTH_FAILURES_PER_IP, now + SCANNER_BAN_DURATION_SEC)


def _sanitize_log_str(text: str) -> str:
    """Strip ASCII/Unicode control characters to prevent terminal/log escape injection."""
    return "".join(ch for ch in text if ch.isprintable())


def _is_private_ip(ip: str) -> bool:
    if ip in LOOPBACK_IPS:
        return True
    try:
        import ipaddress
        return ipaddress.ip_address(ip).is_private
    except Exception:
        return False


def _is_ip_banned(ip: str) -> bool:
    if _is_private_ip(ip):
        return False
    now = time.monotonic()
    with _auth_lock:
        rec = _auth_failures.get(ip)
        if rec is None:
            return False
        fails, ban_until = rec
        if now < ban_until:
            return True
        if ban_until > 0.0 and now >= ban_until:
            del _auth_failures[ip]
            return False
        # Prune old failures outside sliding window
        recent_fails = [t for t in fails if (now - t) < AUTH_FAILURE_WINDOW_SEC]
        if not recent_fails:
            del _auth_failures[ip]
            return False
        _auth_failures[ip] = (recent_fails, 0.0)
        return False


def _record_auth_result(ip: str, success: bool) -> None:
    if _is_private_ip(ip):
        return
    now = time.monotonic()
    with _auth_lock:
        if success:
            _auth_failures.pop(ip, None)
            return

        if ip not in _auth_failures and len(_auth_failures) >= MAX_TRACKED_AUTH_IPS:
            _prune_auth_failures_locked(now)

        rec = _auth_failures.get(ip, ([], 0.0))
        fails, _ = rec
        recent = [t for t in fails if (now - t) < AUTH_FAILURE_WINDOW_SEC]
        recent.append(now)

        ban_until = (now + AUTH_BAN_DURATION_SEC) if len(recent) >= MAX_AUTH_FAILURES_PER_IP else 0.0
        _auth_failures[ip] = (recent, ban_until)
        if ban_until > 0.0:
            logger.warning("Banned IP %s for %d seconds (%d auth failures in %d-sec window)", ip, int(AUTH_BAN_DURATION_SEC), len(recent), int(AUTH_FAILURE_WINDOW_SEC))


def authenticate_client(
    conn: socket.socket, addr: Tuple[str, int]
) -> Optional[Tuple[SHA256Stream, SHA256Stream, str, str]]:
    """
    Execute the Ephemeral X25519 ECDH + Mutual PSK SHA-256 Handshake (Perfect Forward Secrecy):
    1. Generate ephemeral server keypair (`s_priv`, `s_pub = X25519(s_priv, 9)`) and send `s_pub` (32B).
    2. Enforce cumulative deadline to receive `c_pub (32B) || client_digest (32B)`.
    3. Filter probe payloads (HTTP, TLS ClientHello, SSH) and instantly ban scanner IPs for 1 hour.
    4. Compute `shared_secret = X25519(s_priv, c_pub)` (rejecting all-zero low-order points).
    5. Verify `client_digest` against all active per-3DS PSKs and the pending pairing PSK.
    6. Send `server_digest = SHA256(shared_secret || s_pub || c_pub || matched_psk || "SERVER")` (32B).
    7. Derive directional `(rx_stream, tx_stream)`.
    """
    ip = addr[0]
    candidates = psk_store.get_all_valid_psks(APP_DIR)
    if not candidates:
        candidates = [(load_ocr_psk(), "legacy", None)]
    conn.settimeout(AUTH_TIMEOUT_SEC)
    s_priv = secrets.token_bytes(X25519_KEY_SIZE)
    s_pub = x25519_basepoint(s_priv)

    try:
        conn.sendall(s_pub)
        client_reply = recv_exact(
            conn,
            X25519_KEY_SIZE + SHA256_SIZE,
            max_total_sec=AUTH_TIMEOUT_SEC,
        )
    except (socket.timeout, OSError):
        _record_auth_result(ip, False)
        logger.warning("Authentication timed out or failed (>%.1fs) for %s:%d", AUTH_TIMEOUT_SEC, *addr)
        return None

    if client_reply is None:
        _record_auth_result(ip, False)
        logger.warning("Authentication failed (incomplete handshake) from %s:%d", *addr)
        return None

    # Immediate port scanner / crawler probe filter
    probe_prefixes = (b"GET ", b"POST ", b"HEAD ", b"CONNECT ", b"OPTIONS ", b"\x16\x03", b"SSH-")
    if client_reply.startswith(probe_prefixes):
        _instant_ban_scanner(ip)
        probe_data = client_reply
        try:
            conn.settimeout(0.05)
            extra = conn.recv(512)
            if extra:
                probe_data += extra
        except OSError:
            pass
        logger.warning(
            "Instant 1-hr ban on scanner/crawler from %s:%d (probe detected: %r)",
            *addr,
            probe_data[:512],
        )
        return None

    if len(client_reply) != (X25519_KEY_SIZE + SHA256_SIZE):
        _record_auth_result(ip, False)
        logger.warning("Authentication failed (incomplete handshake: %d bytes) from %s:%d", len(client_reply), *addr)
        return None

    c_pub = client_reply[:X25519_KEY_SIZE]
    client_hash = client_reply[X25519_KEY_SIZE:]
    shared_secret = x25519_scalarmult(s_priv, c_pub)

    # Reject low-order / all-zero X25519 curve points in constant time
    if hmac.compare_digest(shared_secret, b"\x00" * X25519_KEY_SIZE):
        _record_auth_result(ip, False)
        logger.warning("Authentication rejected (low-order X25519 point) from %s:%d", *addr)
        return None

    matched_psk: Optional[bytes] = None
    matched_role = ""
    matched_dev_id: Optional[str] = None
    for cand_psk, role, dev_id in candidates:
        expected_client_hash = hashlib.sha256(
            shared_secret + s_pub + c_pub + cand_psk + b"CLIENT"
        ).digest()
        if hmac.compare_digest(client_hash, expected_client_hash) and matched_psk is None:
            matched_psk = cand_psk
            matched_role = role
            matched_dev_id = dev_id

    if matched_psk is None:
        _record_auth_result(ip, False)
        logger.warning("Authentication rejected (hash mismatch) from %s:%d", *addr)
        return None

    # Prove server authenticity to the 3DS before it uploads any screen pixels
    server_hash = hashlib.sha256(
        shared_secret + s_pub + c_pub + matched_psk + b"SERVER"
    ).digest()
    try:
        conn.sendall(server_hash)
    except OSError:
        return None

    _record_auth_result(ip, True)
    logger.info(
        "Client %s:%d mutually authenticated (role=%s%s, X25519 PFS encrypted stream active).",
        *addr,
        matched_role,
        f", id={matched_dev_id}" if matched_dev_id else "",
    )
    rx_stream = SHA256Stream(shared_secret, s_pub, c_pub, matched_psk, STREAM_DIR_C2S)
    tx_stream = SHA256Stream(shared_secret, s_pub, c_pub, matched_psk, STREAM_DIR_S2C)
    return rx_stream, tx_stream, matched_psk.decode("utf-8", errors="replace"), matched_role


def export_latest_ocr_state(
    dumps_dir: str = "",
    top_lines: Optional[List[DetectedLine]] = None,
    bot_lines: Optional[List[DetectedLine]] = None,
    last_click: Optional[Dict[str, Any]] = None,
    active_title_id: Optional[int] = None,
    active_game_name: Optional[str] = None,
) -> None:
    """Export the latest 3DS screen lines, bboxes, and active cursor lookup to dumps/latest_ocr.json."""
    try:
        dumps_dir = dumps_dir or os.path.join(APP_DIR, "dumps")
        os.makedirs(dumps_dir, exist_ok=True)
        out_path = os.path.join(dumps_dir, "latest_ocr.json")
        tmp_path = out_path + ".tmp"

        prev_data: Dict[str, Any] = {}
        if os.path.isfile(out_path):
            try:
                with open(out_path, "r", encoding="utf-8") as f:
                    prev_data = json.load(f)
            except Exception:
                prev_data = {}

        top_data = []
        if top_lines is not None:
            for ln in top_lines:
                top_data.append({
                    "bbox": list(ln.bbox),
                    "text": ln.text,
                    "engines": ln.engine_texts,
                    "is_vertical": ln.is_vertical,
                })
        elif "screens" in prev_data and "top" in prev_data["screens"]:
            top_data = prev_data["screens"]["top"].get("lines", [])

        bot_data = []
        if bot_lines is not None:
            for ln in bot_lines:
                bot_data.append({
                    "bbox": list(ln.bbox),
                    "text": ln.text,
                    "engines": ln.engine_texts,
                    "is_vertical": ln.is_vertical,
                })
        elif "screens" in prev_data and "bot" in prev_data["screens"]:
            bot_data = prev_data["screens"]["bot"].get("lines", [])

        click_data = last_click if last_click is not None else prev_data.get("last_click", None)

        payload = {
            "timestamp": time.time(),
            "title_id": f"0x{active_title_id:016X}" if active_title_id is not None else prev_data.get("title_id", ""),
            "game_name": active_game_name if active_game_name is not None else prev_data.get("game_name", ""),
            "screens": {
                "top": {
                    "width": 400,
                    "height": 240,
                    "lines": top_data,
                },
                "bot": {
                    "width": 320,
                    "height": 240,
                    "lines": bot_data,
                },
            },
            "last_click": click_data,
        }

        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2)
        os.replace(tmp_path, out_path)
    except Exception as e:
        logger.debug("Failed exporting latest OCR state: %s", e)


def handle_3ds_session(
    conn: socket.socket,
    addr: Tuple[str, int],
    pipeline: JapaneseOCRDictionaryPipeline,
) -> None:
    """Handle a single 3DS OCR client lifecycle on its own worker thread."""
    ip = addr[0]
    session_acquired = False
    try:
        with conn:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

            try:
                streams = authenticate_client(conn, addr)
            finally:
                _unregister_preauth(conn, ip)

            if streams is None:
                return

            # If all session slots are occupied by stale half-open sockets (e.g., dropped mobile hotspot),
            # evict the oldest active session (preferring same IP first) so a newly authenticated 3DS never locks itself out.
            if not _session_semaphore.acquire(blocking=False):
                with _active_sessions_lock:
                    if _active_session_socks:
                        evict_idx = 0
                        for idx, (_, s_ip) in enumerate(_active_session_socks):
                            if s_ip == ip:
                                evict_idx = idx
                                break
                        stale_sock, _ = _active_session_socks.pop(evict_idx)
                        try:
                            stale_sock.shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                        try:
                            stale_sock.close()
                        except OSError:
                            pass
                if not _session_semaphore.acquire(timeout=2.0):
                    logger.warning("Rejected authenticated %s:%d: max active sessions (%d) reached.", *addr, MAX_CONCURRENT_CONNECTIONS)
                    return

            session_acquired = True
            with _active_sessions_lock:
                _active_session_socks.append((conn, ip))

            rx_stream, tx_stream, matched_psk_str, matched_role = streams

            title_raw = rx_stream.recv_frame(conn, TITLE_ID_SIZE, max_total_sec=FB_TRANSFER_TIMEOUT_SEC)
            if title_raw is None:
                logger.warning("Connection closed while receiving Title_ID from %s:%d.", *addr)
                return

            if title_raw == PAIRING_PING_MAGIC:
                len_raw = rx_stream.recv_frame(conn, 2, max_total_sec=5.0)
                if len_raw is None:
                    logger.warning("Connection closed while receiving pairing info length from %s:%d.", *addr)
                    return
                (info_len,) = struct.unpack("!H", len_raw)
                if info_len <= 0 or info_len > MAX_DEVICE_INFO_LEN:
                    logger.warning("Invalid pairing info length %d from %s:%d.", info_len, *addr)
                    return
                info_raw = rx_stream.recv_frame(conn, info_len, max_total_sec=5.0)
                if info_raw is None:
                    logger.warning("Connection closed while receiving pairing info from %s:%d.", *addr)
                    return
                lines = info_raw.decode("utf-8", errors="replace").split("\n")
                dev_id = lines[0].strip() if len(lines) > 0 else ""
                dev_name = lines[1].strip() if len(lines) > 1 else "3DS"
                dev_model = lines[2].strip() if len(lines) > 2 else "3DS"
                rotated, _, dev_rec = psk_store.claim_pairing_psk(
                    APP_DIR, matched_psk_str, dev_id, dev_name, dev_model
                )
                logger.info(
                    "[PAIRING] Paired 3DS '%s' (%s, ID=%s) from %s:%d%s",
                    _sanitize_log_str(dev_rec.get("name", dev_name)),
                    _sanitize_log_str(dev_rec.get("model", dev_model)),
                    _sanitize_log_str(dev_rec.get("device_id", dev_id)),
                    *addr,
                    " — generated new QR pairing key." if rotated else ".",
                )
                tx_stream.send_frame(conn, b"\x01")
                return

            psk_store.touch_device_last_seen(APP_DIR, matched_psk_str, fallback_ip=ip)
            (active_title_id,) = struct.unpack(TITLE_ID_FMT, title_raw)
            active_game_name = resolve_3ds_game_name(active_title_id)
            logger.info(
                "Active 3DS Title_ID=0x%016X (%s) from %s:%d.",
                active_title_id,
                _sanitize_log_str(active_game_name),
                *addr,
            )

            # Enforce cumulative transfer deadlines for the 518 KB encrypted dual-screen upload over WAN
            logger.info("Receiving encrypted Top Screen framebuffer (%d bytes) from %s:%d...", TOP_FB_BYTES, *addr)
            top_raw = rx_stream.recv_frame(conn, TOP_FB_BYTES, max_total_sec=FB_TRANSFER_TIMEOUT_SEC)
            if top_raw is None:
                logger.warning("Connection closed while receiving Top Screen from %s:%d.", *addr)
                return

            logger.info("Receiving encrypted Bottom Screen framebuffer (%d bytes) from %s:%d...", BOT_FB_BYTES, *addr)
            bot_raw = rx_stream.recv_frame(conn, BOT_FB_BYTES, max_total_sec=FB_TRANSFER_TIMEOUT_SEC)
            if bot_raw is None:
                logger.warning("Connection closed while receiving Bottom Screen from %s:%d.", *addr)
                return

            top_img = Image.frombytes("RGB", TOP_DIM, top_raw)
            bot_img = Image.frombytes("RGB", BOT_DIM, bot_raw)
            logger.info("Decrypted & verified Top (%dx%d) and Bottom (%dx%d) PIL Images.", *top_img.size, *bot_img.size)
            try:
                dumps_dir = os.path.join(APP_DIR, "dumps")
                os.makedirs(dumps_dir, exist_ok=True)
                top_img.save(os.path.join(dumps_dir, "latest_top.png"))
                bot_img.save(os.path.join(dumps_dir, "latest_bot.png"))
                both_img = Image.new("RGB", (400, 484), color=(18, 20, 26))
                both_img.paste(top_img, (0, 0))
                both_img.paste(bot_img, (40, 244))
                both_img.save(os.path.join(dumps_dir, "latest_both.png"))
            except Exception as dump_err:
                logger.debug("Failed saving debug screen frames: %s", dump_err)

            export_latest_ocr_state(
                top_lines=[],
                bot_lines=[],
                last_click=None,
                active_title_id=active_title_id,
                active_game_name=active_game_name,
            )

            paragraph_cache: Dict[Tuple[int, str], Dict[str, List[StitchedParagraph]]] = {}
            p_cache_lock = threading.Lock()
            tx_lock = threading.Lock()
            bundle_lock = threading.Lock()
            anki_state_lock = threading.Lock()
            caps_ready_evt = threading.Event()
            session_closed = threading.Event()
            anki_req_seq: int = 0
            session_anki_pending: int = 0
            screen_bundle_spots: Dict[int, List[List[OcrVariant]]] = {}
            ocr_variants: List[OcrVariant] = []
            ocr_variant_idx: int = 0
            active_results: List[DictionaryResult] = []
            entry_idx: int = 0
            gloss_scroll_y: int = 0
            pos_scroll_x: int = 0
            last_screen_id: int = SCREEN_ID_TOP
            last_target_screen_id: int = SCREEN_ID_BOTTOM
            last_x: int = 0
            last_y: int = 0
            last_kanji_char: str = ""
            last_kanji_target_scr: int = SCREEN_ID_TOP
            last_kanji_hotspots: List[Tuple[int, int, int, int, int]] = []
            status_banner: str = ""
            last_ocr_time: float = 0.0
            active_footer_keys = ["A", "SEL", "X"]
            client_supports_bundle: bool = False
            client_supports_screen_bundle: bool = False
            session_anki_creds: Optional[Tuple[str, str]] = None

            def _safe_send_pair(hdr: bytes, payload: bytes) -> bool:
                with tx_lock:
                    if session_closed.is_set():
                        return False
                    tx_stream.send_frame(conn, hdr)
                    tx_stream.send_frame(conn, payload)
                    return True

            def _get_or_build_paragraphs(
                scr_id: int, scr_img: Image.Image, ocr_mode: Optional[str] = None
            ) -> Dict[str, List[StitchedParagraph]]:
                cur_mode = pipeline._resolve_ocr_mode(ocr_mode)
                if cur_mode == "hybrid_cached":
                    if pipeline.has_cached_screen_lines(scr_img, "gemini_cached"):
                        eff_mode = "gemini_cached"
                    elif pipeline.has_cached_screen_lines(scr_img, "local_cached"):
                        eff_mode = "local_cached"
                    else:
                        _, eff_mode = pipeline.detect_screen_lines_with_source(
                            scr_img, ocr_mode="hybrid_cached"
                        )
                else:
                    eff_mode = cur_mode

                key = (scr_id, eff_mode)
                with p_cache_lock:
                    cached = paragraph_cache.get(key)
                if cached is not None:
                    return cached
                logger.info(
                    "Building stitched multi-line OCR layout for Screen_ID=%d (mode=%s)...",
                    scr_id,
                    eff_mode,
                )
                built = pipeline.build_stitched_paragraphs_by_engine(scr_img, ocr_mode=eff_mode)
                with p_cache_lock:
                    paragraph_cache[key] = built
                return built

            def _wait_evt_or_closed(evt: threading.Event) -> bool:
                while not evt.is_set():
                    if session_closed.is_set():
                        return False
                    evt.wait(timeout=0.2)
                return not session_closed.is_set()

            def _bg_precache() -> None:
                try:
                    t_lines: List[DetectedLine] = []
                    b_lines: List[DetectedLine] = []
                    precache_mode = pipeline._resolve_ocr_mode()

                    def _push_screen_bundles(
                        mode_to_push: str,
                        tag: str = "",
                        sent_screens: Optional[set] = None,
                        skip_if_sent: Optional[set] = None,
                        abort_check: Optional[Callable[[], bool]] = None,
                    ) -> None:
                        nonlocal t_lines, b_lines
                        suffix = f" [{tag}]" if tag else ""
                        for scr_id, scr_img in ((SCREEN_ID_TOP, top_img), (SCREEN_ID_BOTTOM, bot_img)):
                            if session_closed.is_set() or (abort_check is not None and abort_check()):
                                return
                            if skip_if_sent is not None and scr_id in skip_if_sent:
                                continue
                            p_by_eng = _get_or_build_paragraphs(scr_id, scr_img, ocr_mode=mode_to_push)
                            det_lines = pipeline.detect_screen_lines(scr_img, ocr_mode=mode_to_push)
                            if scr_id == SCREEN_ID_TOP:
                                t_lines = det_lines
                            else:
                                b_lines = det_lines
                            export_latest_ocr_state(
                                top_lines=t_lines,
                                bot_lines=b_lines,
                                active_title_id=active_title_id,
                                active_game_name=active_game_name,
                            )
                            caps_ready_evt.wait(timeout=1.5)
                            if session_closed.is_set() or (abort_check is not None and abort_check()):
                                return
                            if client_supports_screen_bundle:
                                payload, spots_vars = build_screen_bundle(
                                    pipeline,
                                    scr_img,
                                    p_by_eng,
                                    scr_id,
                                    footer_keys=list(active_footer_keys),
                                )
                                if session_closed.is_set() or (abort_check is not None and abort_check()):
                                    return
                                with bundle_lock:
                                    screen_bundle_spots[scr_id] = spots_vars
                                hdr = struct.pack(
                                    POPUP_HEADER_FMT,
                                    scr_id | POPUP_SCREEN_BUNDLE_FLAG,
                                    len(payload),
                                )
                                if _safe_send_pair(hdr, payload):
                                    if sent_screens is not None:
                                        sent_screens.add(scr_id)
                                    logger.info(
                                        "Sent encrypted %d-byte screen bundle (%d spots) for Screen_ID=%d%s.",
                                        len(payload),
                                        len(spots_vars),
                                        scr_id,
                                        suffix,
                                    )

                    if precache_mode == "hybrid_cached" and not session_closed.is_set():
                        logger.info(
                            "Running parallel Hybrid OCR (Local + AI) for Screen_ID=0 and Screen_ID=1..."
                        )
                        hybrid = pipeline.start_hybrid_screens_batch(
                            [top_img, bot_img], cancel_event=session_closed
                        )
                        pushed_local_screens: set = set()
                        if not _wait_evt_or_closed(hybrid.any_batch_ready_evt):
                            return
                        if not hybrid.gemini_ready_evt.is_set():
                            logger.info(
                                "Hybrid OCR: Local ready first — pushing Local screen bundles while awaiting AI upgrade..."
                            )
                            _push_screen_bundles(
                                "local_cached",
                                tag="local",
                                sent_screens=pushed_local_screens,
                                abort_check=lambda: hybrid.gemini_ready_evt.is_set() and hybrid.gemini_succeeded,
                            )
                            if not _wait_evt_or_closed(hybrid.gemini_ready_evt):
                                return

                        if hybrid.gemini_succeeded:
                            logger.info(
                                "Hybrid OCR: AI results ready — replacing 3DS screen bundles with AI OCR."
                            )
                            _push_screen_bundles("gemini_cached", tag="AI upgrade")
                        else:
                            if not _wait_evt_or_closed(hybrid.local_all_done_evt):
                                return
                            if len(pushed_local_screens) < 2:
                                logger.info(
                                    "Hybrid OCR: AI unavailable — pushing remaining Local screen bundles as fallback."
                                )
                                _push_screen_bundles(
                                    "local_cached",
                                    tag="local fallback",
                                    sent_screens=pushed_local_screens,
                                    skip_if_sent=pushed_local_screens,
                                )
                    else:
                        if precache_mode == "gemini_cached" and not session_closed.is_set():
                            logger.info(
                                "Running combined dual-screen batch OCR for Screen_ID=0 and Screen_ID=1 (mode=%s)...",
                                precache_mode,
                            )
                            pipeline.precache_screens_batch([top_img, bot_img], ocr_mode=precache_mode)
                        _push_screen_bundles(precache_mode)
                except Exception as ex:
                    logger.warning("Background screen precache/bundle error: %s", ex, exc_info=True)
            threading.Thread(target=_bg_precache, daemon=True).start()

            def _send_active_popup(banner_override: str = "") -> None:
                if client_supports_bundle and ocr_variants:
                    bundle = build_popup_bundle(
                        ocr_variants,
                        last_screen_id,
                        last_x,
                        last_y,
                        footer_keys=active_footer_keys,
                        status_banner=banner_override or status_banner,
                    )
                    hdr = struct.pack(POPUP_HEADER_FMT, last_target_screen_id | POPUP_BUNDLE_FLAG, len(bundle))
                    if _safe_send_pair(hdr, bundle):
                        logger.info(
                            "Sent encrypted %d-byte popup bundle (%d variants) to Target_Screen_ID=%d.",
                            len(bundle),
                            len(ocr_variants),
                            last_target_screen_id,
                        )
                else:
                    popup_rgb = render_dictionary_popup(
                        active_results[entry_idx],
                        last_screen_id,
                        last_x,
                        last_y,
                        entry_idx=entry_idx,
                        total_entries=len(active_results),
                        gloss_scroll_y=gloss_scroll_y,
                        pos_scroll_x=pos_scroll_x,
                        status_banner=banner_override or status_banner,
                        footer_keys=active_footer_keys,
                    )
                    header = struct.pack(POPUP_HEADER_FMT, last_target_screen_id, len(popup_rgb))
                    if _safe_send_pair(header, popup_rgb):
                        logger.info(
                            "Sent encrypted %d-byte dictionary popup to Target_Screen_ID=%d.",
                            len(popup_rgb),
                            last_target_screen_id,
                        )

            def _send_status_update(
                banner_txt: str,
                target_scr_override: Optional[int] = None,
                extra_flags: int = 0,
            ) -> None:
                t_scr = last_target_screen_id if target_scr_override is None else target_scr_override
                if client_supports_bundle:
                    pkt = build_status_banner_packet(banner_txt)
                    hdr = struct.pack(POPUP_HEADER_FMT, t_scr | POPUP_STATUS_FLAG | extra_flags, len(pkt))
                    _safe_send_pair(hdr, pkt)
                else:
                    prog_rgb = render_dictionary_popup(
                        active_results[entry_idx],
                        last_screen_id,
                        last_x,
                        last_y,
                        entry_idx=entry_idx,
                        total_entries=len(active_results),
                        gloss_scroll_y=gloss_scroll_y,
                        pos_scroll_x=pos_scroll_x,
                        status_banner=banner_txt,
                        footer_keys=active_footer_keys,
                    )
                    prog_hdr = struct.pack(POPUP_HEADER_FMT, t_scr, len(prog_rgb))
                    _safe_send_pair(prog_hdr, prog_rgb)

            def _build_anki_screenshot() -> Optional[Image.Image]:
                ss_mode = str(_load_anki_config().get("screenshot_mode", "clicked") or "clicked").strip().lower()
                if ss_mode == "none":
                    return None
                if ss_mode == "both":
                    combined = Image.new("RGB", (400, 484), color=(18, 20, 26))
                    combined.paste(top_img, (0, 0))
                    combined.paste(bot_img, (40, 244))
                    return combined
                return top_img if last_screen_id == SCREEN_ID_TOP else bot_img

            while True:
                conn.settimeout(IDLE_SESSION_TIMEOUT_SEC)
                click_data = rx_stream.recv_frame(conn, CLICK_PACKET_SIZE)
                if click_data is None:
                    logger.info("3DS client %s:%d closed the session.", *addr)
                    break

                if not psk_store.is_psk_valid(APP_DIR, matched_psk_str):
                    logger.warning("Terminating session for %s:%d: PSK was invalidated.", *addr)
                    break

                cmd_or_screen, x, y = struct.unpack(CLICK_PACKET_FMT, click_data)

                if cmd_or_screen in (SCREEN_ID_TOP, SCREEN_ID_BOTTOM):
                    now_ts = time.monotonic()
                    if (now_ts - last_ocr_time) < MIN_OCR_INTERVAL_SEC:
                        logger.info("Rate limit: skipped rapid click from %s:%d (%.3fs)", *addr, now_ts - last_ocr_time)
                        continue
                    last_ocr_time = now_ts

                    screen_id = cmd_or_screen
                    logger.info(
                        "Received cursor click from %s:%d: Screen_ID=%d, X=%d, Y=%d",
                        *addr,
                        screen_id,
                        x,
                        y,
                    )
                    if screen_id == SCREEN_ID_TOP:
                        source_img = top_img
                        target_screen_id = SCREEN_ID_BOTTOM
                    else:
                        source_img = bot_img
                        target_screen_id = SCREEN_ID_TOP

                    max_w, max_h = source_img.size
                    if not (0 <= x < max_w and 0 <= y < max_h):
                        logger.warning(
                            "Out-of-bounds coordinates (%d, %d) for Screen_ID=%d; ignoring.",
                            x,
                            y,
                            screen_id,
                        )
                        continue

                    p_by_eng = _get_or_build_paragraphs(screen_id, source_img)
                    ocr_variants = pipeline.lookup_at_cursor_variants(
                        source_img, p_by_eng, x, y
                    )
                    ocr_variant_idx = 0
                    active_results = ocr_variants[0].results
                    entry_idx = 0
                    gloss_scroll_y = 0
                    pos_scroll_x = 0
                    status_banner = ""
                    last_screen_id = screen_id
                    last_target_screen_id = target_screen_id
                    last_x = x
                    last_y = y

                    result = active_results[entry_idx]
                    click_info = {
                        "screen_id": screen_id,
                        "screen_name": "top" if screen_id == SCREEN_ID_TOP else "bot",
                        "x": x,
                        "y": y,
                        "matched_surface": result.matched_surface,
                        "headword": result.headword,
                        "reading": result.reading,
                        "pos": result.pos,
                        "glosses": result.glosses,
                        "word_bboxes": [list(b) for b in result.word_bboxes] if result.word_bboxes else [],
                        "engines": result.ocr_engines,
                        "found": result.found,
                    }
                    export_latest_ocr_state(
                        last_click=click_info,
                        active_title_id=active_title_id,
                        active_game_name=active_game_name,
                    )
                    logger.info(
                        "Lookup result [1/%d] (OCR variants=%d, engines=%s): surface='%s' -> headword='%s' (found=%s)",
                        len(active_results),
                        len(ocr_variants),
                        "+".join(ocr_variants[0].engines),
                        _sanitize_log_str(result.matched_surface),
                        _sanitize_log_str(result.headword),
                        result.found,
                    )
                elif cmd_or_screen in (CMD_TRANSLATE_TOP, CMD_TRANSLATE_BOT):
                    now_ts = time.monotonic()
                    if (now_ts - last_ocr_time) < MIN_OCR_INTERVAL_SEC:
                        logger.info("Rate limit: skipped rapid selection translate from %s:%d (%.3fs)", *addr, now_ts - last_ocr_time)
                        # Still drain the second corner packet so stream framing stays in sync
                        rx_stream.recv_frame(conn, CLICK_PACKET_SIZE, max_total_sec=2.0)
                        continue
                    last_ocr_time = now_ts

                    pkt2_data = rx_stream.recv_frame(conn, CLICK_PACKET_SIZE, max_total_sec=2.0)
                    if pkt2_data is None:
                        logger.warning("Connection closed while receiving second corner from %s:%d.", *addr)
                        break
                    cmd2, x2, y2 = struct.unpack(CLICK_PACKET_FMT, pkt2_data)
                    if cmd2 != cmd_or_screen:
                        logger.warning("Framing desync from %s:%d: expected corner cmd 0x%02x, got 0x%02x", *addr, cmd_or_screen, cmd2)
                        break
                    screen_id = SCREEN_ID_TOP if cmd_or_screen == CMD_TRANSLATE_TOP else SCREEN_ID_BOTTOM
                    if screen_id == SCREEN_ID_TOP:
                        source_img = top_img
                        target_screen_id = SCREEN_ID_BOTTOM
                    else:
                        source_img = bot_img
                        target_screen_id = SCREEN_ID_TOP

                    max_w, max_h = source_img.size
                    bx1 = max(0, min(max_w - 1, min(x, x2)))
                    bx2 = max(0, min(max_w, max(x, x2)))
                    by1 = max(0, min(max_h - 1, min(y, y2)))
                    by2 = max(0, min(max_h, max(y, y2)))

                    logger.info(
                        "Received selection box from %s:%d: Screen_ID=%d, (%d,%d) -> (%d,%d)",
                        *addr,
                        screen_id,
                        bx1,
                        by1,
                        bx2,
                        by2,
                    )

                    # Send immediate in-progress feedback popup while OCR + translation runs
                    pending_card = DictionaryResult(
                        matched_surface=f"{bx1},{by1}..{bx2},{by2}",
                        context_text="Translating selection...",
                        headword="Translating...",
                        reading="",
                        pos=f"[Region ({bx1},{by1}) -> ({bx2},{by2})]",
                        glosses=["Reading Japanese text & translating selection..."],
                        word_bboxes=[],
                        found=True,
                    )
                    if client_supports_bundle:
                        prog_bundle = build_popup_bundle(
                            [OcrVariant(engines=[], ocr_text="", results=[pending_card])],
                            screen_id,
                            bx1,
                            by1,
                            footer_keys=active_footer_keys,
                            status_banner="Translating selected region...",
                        )
                        prog_hdr = struct.pack(POPUP_HEADER_FMT, target_screen_id | POPUP_BUNDLE_FLAG, len(prog_bundle))
                        _safe_send_pair(prog_hdr, prog_bundle)
                    else:
                        prog_rgb = render_dictionary_popup(
                            pending_card,
                            screen_id,
                            bx1,
                            by1,
                            entry_idx=0,
                            total_entries=1,
                            status_banner="Translating selected region...",
                            footer_keys=active_footer_keys,
                        )
                        prog_hdr = struct.pack(POPUP_HEADER_FMT, target_screen_id, len(prog_rgb))
                        _safe_send_pair(prog_hdr, prog_rgb)

                    ocr_variants = pipeline.translate_region_variants(
                        source_img, (bx1, by1, bx2, by2), title=active_title_id
                    )
                    ocr_variant_idx = 0
                    active_results = ocr_variants[0].results
                    entry_idx = 0
                    gloss_scroll_y = 0
                    pos_scroll_x = 0
                    status_banner = ""
                    last_screen_id = screen_id
                    last_target_screen_id = target_screen_id
                    last_x = bx1
                    last_y = by1

                    result = active_results[entry_idx]
                    click_info = {
                        "screen_id": screen_id,
                        "screen_name": "top" if screen_id == SCREEN_ID_TOP else "bot",
                        "x": bx1,
                        "y": by1,
                        "bbox": [bx1, by1, bx2, by2],
                        "is_region": True,
                        "matched_surface": result.matched_surface,
                        "headword": result.headword,
                        "reading": result.reading,
                        "pos": result.pos,
                        "glosses": result.glosses,
                        "word_bboxes": [[bx1, by1, bx2, by2]],
                        "engines": result.ocr_engines,
                        "found": result.found,
                    }
                    export_latest_ocr_state(
                        last_click=click_info,
                        active_title_id=active_title_id,
                        active_game_name=active_game_name,
                    )
                    logger.info(
                        "Region translation result [1/%d] (OCR variants=%d, engines=%s): '%s' -> %s",
                        len(active_results),
                        len(ocr_variants),
                        "+".join(ocr_variants[0].engines),
                        _sanitize_log_str(result.matched_surface[:40]),
                        _sanitize_log_str(result.glosses[0][:60] if result.glosses else ""),
                    )
                elif cmd_or_screen in (CMD_PREV_OCR, CMD_NEXT_OCR):
                    if not ocr_variants or not active_results:
                        continue
                    if len(ocr_variants) <= 1:
                        eng_str = "+".join(ocr_variants[0].engines) if ocr_variants[0].engines else "All"
                        status_banner = (
                            "OCR performed by Gemini"
                            if ocr_variants[0].engines == ["Gemini"]
                            else f"All 4 OCR engines agree ({eng_str})"
                        )
                        result = active_results[entry_idx]
                    else:
                        if cmd_or_screen == CMD_PREV_OCR:
                            ocr_variant_idx = (ocr_variant_idx - 1) % len(ocr_variants)
                        else:
                            ocr_variant_idx = (ocr_variant_idx + 1) % len(ocr_variants)
                        cur_var = ocr_variants[ocr_variant_idx]
                        active_results = cur_var.results
                        entry_idx = 0
                        gloss_scroll_y = 0
                        pos_scroll_x = 0
                        eng_str = "+".join(cur_var.engines)
                        status_banner = (
                            f"OCR [{ocr_variant_idx + 1}/{len(ocr_variants)}] ({eng_str}): {cur_var.ocr_text[:20]}"
                        )
                        result = active_results[entry_idx]
                    click_info = {
                        "screen_id": last_screen_id,
                        "screen_name": "top" if last_screen_id == SCREEN_ID_TOP else "bot",
                        "x": last_x,
                        "y": last_y,
                        "matched_surface": result.matched_surface,
                        "headword": result.headword,
                        "reading": result.reading,
                        "pos": result.pos,
                        "glosses": result.glosses,
                        "word_bboxes": [list(b) for b in result.word_bboxes] if result.word_bboxes else [],
                        "engines": cur_var.engines if 'cur_var' in locals() else result.ocr_engines,
                        "ocr_variant_idx": ocr_variant_idx,
                        "ocr_total_variants": len(ocr_variants),
                        "found": result.found,
                    }
                    export_latest_ocr_state(
                        last_click=click_info,
                        active_title_id=active_title_id,
                        active_game_name=active_game_name,
                    )
                    logger.info(
                        "OCR variant cycle cmd=0x%02X -> variant [%d/%d] (%s): '%s'",
                        cmd_or_screen,
                        ocr_variant_idx + 1,
                        len(ocr_variants),
                        "+".join(ocr_variants[ocr_variant_idx].engines),
                        _sanitize_log_str(ocr_variants[ocr_variant_idx].ocr_text[:40]),
                    )
                elif cmd_or_screen == CMD_SET_KEY_LABELS:
                    k_lookup = _char_code_to_key_str(click_data[1])
                    k_mode = _char_code_to_key_str(click_data[2])
                    k_anki = _char_code_to_key_str(click_data[3])
                    active_footer_keys = [k_lookup, k_mode, k_anki]
                    logger.info("Synchronized dynamic footer keybindings from 3DS: Lookup=[%s], Mode=[%s], Anki=[%s]", k_lookup, k_mode, k_anki)
                    continue
                elif cmd_or_screen == CMD_SET_CLIENT_CAPS:
                    client_supports_bundle = bool(x & 1)
                    client_supports_screen_bundle = bool(x & 2)
                    caps_ready_evt.set()
                    logger.info(
                        "Synchronized client capabilities from %s:%d: popup_bundle=%s, screen_bundle=%s",
                        *addr,
                        client_supports_bundle,
                        client_supports_screen_bundle,
                    )
                    continue
                elif cmd_or_screen == CMD_SET_MODE:
                    is_trans = (x != 0 or y != 0 or click_data[1] != 0 or click_data[2] != 0)
                    k_lookup = active_footer_keys[0] if active_footer_keys else "A"
                    status_banner = (
                        f"Mode: Selection Translate ([{k_lookup}] 2 Corners)"
                        if is_trans
                        else f"Mode: Dictionary Lookup ([{k_lookup}] Single Word)"
                    )
                    logger.info("Client %s:%d toggled mode -> %s", *addr, status_banner)
                    if client_supports_bundle or not active_results:
                        continue
                    result = active_results[entry_idx]
                elif cmd_or_screen in (
                    CMD_PREV_ENTRY,
                    CMD_NEXT_ENTRY,
                    CMD_SCROLL_UP,
                    CMD_SCROLL_DOWN,
                    CMD_POS_LEFT,
                    CMD_POS_RIGHT,
                ):
                    if not active_results:
                        continue
                    total_entries = len(active_results)
                    status_banner = ""
                    if cmd_or_screen == CMD_PREV_ENTRY:
                        entry_idx = (entry_idx - 1) % total_entries
                        gloss_scroll_y = 0
                        pos_scroll_x = 0
                    elif cmd_or_screen == CMD_NEXT_ENTRY:
                        entry_idx = (entry_idx + 1) % total_entries
                        gloss_scroll_y = 0
                        pos_scroll_x = 0
                    else:
                        cur_res = active_results[entry_idx]
                        max_gloss_y, max_pos_x = compute_popup_scroll_limits(cur_res)
                        if cmd_or_screen == CMD_SCROLL_UP:
                            gloss_scroll_y = max(0, gloss_scroll_y - GLOSS_SCROLL_STEP)
                        elif cmd_or_screen == CMD_SCROLL_DOWN:
                            gloss_scroll_y = min(max_gloss_y, gloss_scroll_y + GLOSS_SCROLL_STEP)
                        elif cmd_or_screen == CMD_POS_LEFT:
                            pos_scroll_x = max(0, pos_scroll_x - POS_SCROLL_STEP)
                        elif cmd_or_screen == CMD_POS_RIGHT:
                            pos_scroll_x = min(max_pos_x, pos_scroll_x + POS_SCROLL_STEP)

                    result = active_results[entry_idx]
                    click_info = {
                        "screen_id": last_screen_id,
                        "screen_name": "top" if last_screen_id == SCREEN_ID_TOP else "bot",
                        "x": last_x,
                        "y": last_y,
                        "matched_surface": result.matched_surface,
                        "headword": result.headword,
                        "reading": result.reading,
                        "pos": result.pos,
                        "glosses": result.glosses,
                        "word_bboxes": [list(b) for b in result.word_bboxes] if result.word_bboxes else [],
                        "engines": result.ocr_engines,
                        "entry_idx": entry_idx,
                        "total_entries": total_entries,
                        "found": result.found,
                    }
                    export_latest_ocr_state(
                        last_click=click_info,
                        active_title_id=active_title_id,
                        active_game_name=active_game_name,
                    )
                    logger.info(
                        "Nav cmd=0x%02X -> match [%d/%d] '%s' (scroll_y=%d, pos_x=%d)",
                        cmd_or_screen,
                        entry_idx + 1,
                        total_entries,
                        _sanitize_log_str(result.headword),
                        gloss_scroll_y,
                        pos_scroll_x,
                    )
                elif cmd_or_screen in (CMD_ADD_ANKI, CMD_ADD_ANKI_TOP, CMD_ADD_ANKI_BOT):
                    if cmd_or_screen in (CMD_ADD_ANKI_TOP, CMD_ADD_ANKI_BOT):
                        src_scr = SCREEN_ID_TOP if cmd_or_screen == CMD_ADD_ANKI_TOP else SCREEN_ID_BOTTOM
                        t_scr = SCREEN_ID_BOTTOM if src_scr == SCREEN_ID_TOP else SCREEN_ID_TOP
                        v_i = (y >> 8) & 0xFF
                        e_i = y & 0xFF
                        with bundle_lock:
                            spots_vars = screen_bundle_spots.get(src_scr)
                        if not spots_vars or not (0 <= x < len(spots_vars)):
                            continue
                        spot_vars = spots_vars[x]
                        if not spot_vars or not (0 <= v_i < len(spot_vars)):
                            continue
                        var_res = spot_vars[v_i].results
                        if not var_res or not (0 <= e_i < len(var_res)):
                            continue
                        result = var_res[e_i]
                        last_screen_id = src_scr
                        last_target_screen_id = t_scr
                    else:
                        if not ocr_variants or not active_results:
                            continue
                        if client_supports_bundle and 0 <= x < len(ocr_variants):
                            ocr_variant_idx = x
                            active_results = ocr_variants[ocr_variant_idx].results
                        if client_supports_bundle and 0 <= y < len(active_results):
                            entry_idx = y
                        result = active_results[entry_idx]

                    with anki_state_lock:
                        creds_snap = session_anki_creds

                    if not (creds_snap and creds_snap[0] and creds_snap[1]) and bool(_load_anki_config().get("multi_user_mode", False)):
                        prompt_msg = "AnkiWeb login required (Multi-3DS mode)"
                        logger.info(
                            "Multi-3DS mode: prompting %s:%d to set AnkiWeb login.",
                            *addr,
                        )
                        _send_status_update(
                            prompt_msg,
                            target_scr_override=last_target_screen_id,
                            extra_flags=POPUP_ANKI_LOGIN_PROMPT_FLAG,
                        )
                        continue

                    ss_img = _build_anki_screenshot()

                    if not client_supports_bundle:
                        # Legacy non-bundle client expects a full rendered popup image response
                        progress_banner = f"Adding card to {active_game_name} deck..."
                        _send_status_update(progress_banner)
                        status_banner = _anki_exporter.add_card_for_game(
                            result, active_title_id, screenshot_img=ss_img, anki_creds=creds_snap
                        )
                        logger.info(
                            "Anki export for '%s' (Title_ID=0x%016X): %s",
                            _sanitize_log_str(result.headword),
                            active_title_id,
                            _sanitize_log_str(status_banner),
                        )
                    else:
                        if not result.found or not result.headword:
                            _send_status_update("Anki Error: No valid dictionary match")
                            continue

                        hw_snap = result.headword
                        title_snap = active_title_id
                        target_scr_snap = last_target_screen_id

                        with anki_state_lock:
                            anki_req_seq += 1
                            req_id = anki_req_seq
                            session_anki_pending += 1

                            def _on_anki_done(
                                done_banner: str,
                                _global_remaining: int,
                                rid: int = req_id,
                                hw: str = hw_snap,
                                tid: int = title_snap,
                                t_scr: int = target_scr_snap,
                            ) -> None:
                                logger.info(
                                    "Anki export for '%s' (Title_ID=0x%016X): %s",
                                    _sanitize_log_str(hw),
                                    tid,
                                    _sanitize_log_str(done_banner),
                                )
                                with anki_state_lock:
                                    nonlocal session_anki_pending
                                    session_anki_pending = max(0, session_anki_pending - 1)
                                    rem = session_anki_pending
                                    is_latest = (rid == anki_req_seq)

                                    if session_closed.is_set():
                                        return

                                    is_err = done_banner.startswith("Anki Error") or done_banner.startswith("Set AnkiWeb")
                                    if is_err:
                                        banner_to_send = done_banner
                                    elif is_latest and rem == 0:
                                        banner_to_send = done_banner
                                    elif rem > 0:
                                        banner_to_send = f"Adding to Anki ({rem} left)..."
                                    else:
                                        return

                                    try:
                                        _send_status_update(banner_to_send, target_scr_override=t_scr)
                                    except OSError:
                                        pass

                            q_len, immediate_banner = _anki_exporter.enqueue_card_for_game(
                                result,
                                active_title_id,
                                screenshot_img=ss_img,
                                on_done=_on_anki_done,
                                anki_creds=creds_snap,
                            )
                            if q_len == 0:
                                session_anki_pending = max(0, session_anki_pending - 1)
                            _send_status_update(immediate_banner, target_scr_override=target_scr_snap)
                        continue
                elif cmd_or_screen == CMD_LOOKUP_KANJI:
                    scr_code = (x >> 8) & 0xFF
                    cp_val = ((x & 0xFF) << 16) | (y & 0xFFFF)
                    scr_code = scr_code & 0x03  # ignore any scroll bits from old clients
                    try:
                        kanji_char = chr(cp_val)
                    except (ValueError, OverflowError):
                        continue
                    last_kanji_char = kanji_char
                    if scr_code == 1:
                        target_kanji_scr = SCREEN_ID_TOP
                    elif scr_code == 2:
                        target_kanji_scr = SCREEN_ID_BOTTOM
                    else:
                        target_kanji_scr = SCREEN_ID_BOTTOM if last_target_screen_id == SCREEN_ID_TOP else SCREEN_ID_TOP
                    last_kanji_target_scr = target_kanji_scr
                    last_kanji_hotspots = []
                    logger.info("Lookup Kanji '%s' (U+%04X) -> target screen %d", _sanitize_log_str(kanji_char), cp_val, target_kanji_scr)
                    kanji_rgb = render_kanji_popup(
                        kanji_char,
                        target_kanji_scr,
                        jamdict=pipeline.jamdict,
                        word_meta_mgr=pipeline.word_meta_mgr,
                        out_hotspots=last_kanji_hotspots,
                    )
                    hdr = struct.pack(POPUP_HEADER_FMT, target_kanji_scr | POPUP_KANJI_FLAG, len(kanji_rgb))
                    _safe_send_pair(hdr, kanji_rgb)
                    continue
                elif cmd_or_screen == CMD_CLICK_KANJI_WIN:
                    pop_x, tall_y = x, y
                    hit_cp = 0
                    for hx1, hy1, hx2, hy2, hcp in last_kanji_hotspots:
                        if hx1 <= pop_x <= hx2 and hy1 <= tall_y <= hy2:
                            hit_cp = hcp
                            break
                    if hit_cp == 0:
                        for hx1, hy1, hx2, hy2, hcp in last_kanji_hotspots:
                            if (hx1 - 3) <= pop_x <= (hx2 + 3) and (hy1 - 3) <= tall_y <= (hy2 + 3):
                                hit_cp = hcp
                                break
                    if hit_cp != 0:
                        try:
                            next_kanji = chr(hit_cp)
                        except (ValueError, OverflowError):
                            continue
                        if next_kanji != last_kanji_char:
                            last_kanji_char = next_kanji
                            last_kanji_hotspots = []
                            logger.info(
                                "Lookup Kanji (in-window) '%s' (U+%04X) -> target screen %d",
                                _sanitize_log_str(next_kanji),
                                hit_cp,
                                last_kanji_target_scr,
                            )
                            kanji_rgb = render_kanji_popup(
                                next_kanji,
                                last_kanji_target_scr,
                                jamdict=pipeline.jamdict,
                                word_meta_mgr=pipeline.word_meta_mgr,
                                out_hotspots=last_kanji_hotspots,
                            )
                            hdr = struct.pack(
                                POPUP_HEADER_FMT, last_kanji_target_scr | POPUP_KANJI_FLAG, len(kanji_rgb)
                            )
                            _safe_send_pair(hdr, kanji_rgb)
                    continue
                elif cmd_or_screen == CMD_ADD_ANKI_KANJI:
                    if not last_kanji_char:
                        continue
                    kanji_res = pipeline.word_meta_mgr.get_kanji_details(last_kanji_char, pipeline.jamdict)
                    target_scr_snap = last_target_screen_id
                    ss_img = _build_anki_screenshot()

                    with anki_state_lock:
                        creds_snap = session_anki_creds
                        if not (creds_snap and creds_snap[0] and creds_snap[1]) and bool(_load_anki_config().get("multi_user_mode", False)):
                            prompt_msg = "AnkiWeb login required (Multi-3DS mode)"
                            logger.info(
                                "Multi-3DS mode: prompting %s:%d to set AnkiWeb login (kanji).",
                                *addr,
                            )
                            _send_status_update(
                                prompt_msg,
                                target_scr_override=target_scr_snap,
                                extra_flags=POPUP_ANKI_LOGIN_PROMPT_FLAG,
                            )
                            continue
                        anki_req_seq += 1
                        req_id = anki_req_seq
                        session_anki_pending += 1

                        def _on_anki_kanji_done(
                            done_banner: str,
                            _global_remaining: int,
                            rid: int = req_id,
                            kch: str = last_kanji_char,
                            tid: int = active_title_id,
                            t_scr: int = target_scr_snap,
                        ) -> None:
                            logger.info(
                                "Anki export for kanji '%s' (Title_ID=0x%016X): %s",
                                _sanitize_log_str(kch),
                                tid,
                                _sanitize_log_str(done_banner),
                            )
                            with anki_state_lock:
                                nonlocal session_anki_pending
                                session_anki_pending = max(0, session_anki_pending - 1)
                                if session_closed.is_set():
                                    return
                                try:
                                    _send_status_update(done_banner, target_scr_override=t_scr)
                                except OSError:
                                    pass

                        q_len, immediate_banner = _anki_exporter.enqueue_card_for_game(
                            kanji_res,
                            active_title_id,
                            screenshot_img=ss_img,
                            on_done=_on_anki_kanji_done,
                            anki_creds=creds_snap,
                        )
                        if q_len == 0:
                            session_anki_pending = max(0, session_anki_pending - 1)
                        _send_status_update(immediate_banner, target_scr_override=target_scr_snap)
                    continue
                elif cmd_or_screen == CMD_SET_ANKI_CREDS:
                    u_len, p_len = x, y
                    if u_len > MAX_ANKI_CRED_FIELD_LEN or p_len > MAX_ANKI_CRED_FIELD_LEN:
                        logger.warning(
                            "Invalid Anki credential lengths (%d, %d) from %s:%d; closing session.",
                            u_len,
                            p_len,
                            *addr,
                        )
                        break
                    if u_len == 0 and p_len == 0:
                        with anki_state_lock:
                            session_anki_creds = None
                        logger.info(
                            "Client %s:%d cleared session AnkiWeb credentials (using server default).",
                            *addr,
                        )
                        continue
                    if u_len == 0 or p_len == 0:
                        if rx_stream.recv_frame(conn, u_len + p_len, max_total_sec=5.0) is None:
                            break
                        with anki_state_lock:
                            session_anki_creds = None
                        logger.warning(
                            "Incomplete AnkiWeb credentials (%d, %d) from %s:%d; cleared session override.",
                            u_len,
                            p_len,
                            *addr,
                        )
                        continue
                    cred_raw = rx_stream.recv_frame(conn, u_len + p_len, max_total_sec=5.0)
                    if cred_raw is None:
                        logger.warning("Connection closed while receiving Anki credentials from %s:%d.", *addr)
                        break
                    u_str = cred_raw[:u_len].decode("utf-8", errors="replace").strip()
                    p_str = cred_raw[u_len:].decode("utf-8", errors="replace")
                    with anki_state_lock:
                        session_anki_creds = (u_str, p_str) if (u_str and p_str) else None
                    if u_str and p_str:
                        masked_u = (u_str[:3] + "***") if len(u_str) > 3 else "***"
                        logger.info(
                            "Client %s:%d set session AnkiWeb credentials for user '%s'.",
                            *addr,
                            _sanitize_log_str(masked_u),
                        )
                    else:
                        logger.info(
                            "Client %s:%d cleared session AnkiWeb credentials (empty after strip).",
                            *addr,
                        )
                    continue
                elif cmd_or_screen == CMD_SET_DEVICE_INFO:
                    info_len = x
                    if info_len <= 0 or info_len > MAX_DEVICE_INFO_LEN:
                        logger.warning("Invalid device info length %d from %s:%d; closing session.", info_len, *addr)
                        break
                    info_raw = rx_stream.recv_frame(conn, info_len, max_total_sec=5.0)
                    if info_raw is None:
                        logger.warning("Connection closed while receiving device info from %s:%d.", *addr)
                        break
                    lines = info_raw.decode("utf-8", errors="replace").split("\n")
                    dev_id = lines[0].strip() if len(lines) > 0 else ""
                    dev_name = lines[1].strip() if len(lines) > 1 else "3DS"
                    dev_model = lines[2].strip() if len(lines) > 2 else "3DS"
                    rotated, _, dev_rec = psk_store.claim_pairing_psk(
                        APP_DIR, matched_psk_str, dev_id, dev_name, dev_model
                    )
                    logger.info(
                        "[PAIRING] Updated device info for '%s' (%s, ID=%s) from %s:%d%s",
                        _sanitize_log_str(dev_rec.get("name", dev_name)),
                        _sanitize_log_str(dev_rec.get("model", dev_model)),
                        _sanitize_log_str(dev_rec.get("device_id", dev_id)),
                        *addr,
                        " — generated new QR pairing key." if rotated else ".",
                    )
                    continue
                else:
                    logger.warning("Unknown Screen_ID/Cmd=0x%02X; ignoring packet.", cmd_or_screen)
                    continue

                _send_active_popup()
    except socket.timeout:
        logger.warning("Session timed out for %s:%d.", *addr)
    except Exception as exc:
        logger.exception("Error handling client %s:%d: %s", *addr, exc)
    finally:
        try:
            session_closed.set()
        except Exception:
            pass
        if session_acquired:
            with _active_sessions_lock:
                _active_session_socks[:] = [
                    (s, s_ip) for (s, s_ip) in _active_session_socks if s is not conn
                ]
            _session_semaphore.release()


# =============================================================================
# Server Manager (Lifecycle & Graceful Start/Stop)
# =============================================================================

class ServerManager:
    """Manages the background TCP server lifecycle, model pipeline, and client threads."""

    def __init__(self, host: str = HOST, port: int = PORT, status_callback=None) -> None:
        self.host = host
        self.port = port
        self.status_callback = status_callback
        self.is_running = False
        self.pipeline: Optional[JapaneseOCRDictionaryPipeline] = None
        self.server_sock: Optional[socket.socket] = None
        self.thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()

    def start(self, host: Optional[str] = None, port: Optional[int] = None) -> bool:
        with self._lock:
            if self.is_running:
                return True
            if host:
                self.host = host
            if port:
                self.port = port
            self.is_running = True

        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        return True

    def _run(self) -> None:
        try:
            if self.status_callback:
                self.status_callback("starting", self.port)

            store = psk_store.load_or_init_store(APP_DIR)
            logger.info(
                "Loaded per-3DS PSK store (%d paired 3DS(es), pending pairing key ready) from %s.",
                len(store.get("devices", [])),
                psk_store.get_store_path(APP_DIR),
            )
            u, _ = _load_anki_credentials()
            if u:
                masked_u = (u[:3] + "***") if len(u) > 3 else "***"
                logger.info("Loaded AnkiWeb credentials for user '%s'.", masked_u)
            else:
                logger.info("No AnkiWeb credentials configured (Anki sync disabled).")

            if self.pipeline is None:
                logger.info("Loading OCR & Japanese Dictionary models...")
                self.pipeline = JapaneseOCRDictionaryPipeline()
                logger.info("OCR Pipeline initialized successfully.")

            self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.server_sock.bind((self.host, self.port))
            self.server_sock.listen(32)
            self.server_sock.settimeout(0.5)
            logger.info("3DS OCR Server active & listening on %s:%d", self.host, self.port)

            if self.status_callback:
                self.status_callback("running", self.port)

            while self.is_running:
                try:
                    conn, addr = self.server_sock.accept()
                except socket.timeout:
                    continue
                except OSError:
                    break

                ip = addr[0]
                if _is_ip_banned(ip):
                    logger.debug("Dropped connection from banned IP %s", ip)
                    try:
                        conn.close()
                    except Exception:
                        pass
                    continue

                if not _try_register_preauth(conn, ip):
                    logger.warning("Rejected %s: pre-auth IP connection limit reached.", ip)
                    try:
                        conn.close()
                    except Exception:
                        pass
                    continue

                logger.info("Accepted connection from %s:%d", *addr)
                worker = threading.Thread(
                    target=handle_3ds_session,
                    args=(conn, addr, self.pipeline),
                    daemon=True,
                )
                worker.start()

        except Exception as exc:
            logger.exception("Server runtime error: %s", exc)
        finally:
            self._cleanup_socket()
            self.is_running = False
            if self.status_callback:
                self.status_callback("stopped", self.port)

    def _cleanup_socket(self) -> None:
        if self.server_sock:
            try:
                self.server_sock.close()
            except Exception:
                pass
            self.server_sock = None

    def stop(self) -> None:
        with self._lock:
            if not self.is_running:
                return
            self.is_running = False

        self._cleanup_socket()
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=2.0)
        logger.info("3DS OCR Server stopped.")
        if self.status_callback:
            self.status_callback("stopped", self.port)


def run_server(host: str = HOST, port: int = PORT) -> None:
    """CLI headless runner."""
    for idx, arg in enumerate(sys.argv):
        if arg == "--port" and idx + 1 < len(sys.argv):
            try:
                port = int(sys.argv[idx + 1])
            except ValueError:
                pass
        elif arg == "--host" and idx + 1 < len(sys.argv):
            host = sys.argv[idx + 1]

    mgr = ServerManager(host=host, port=port)
    mgr.start()
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        logger.info("Shutdown requested.")
        mgr.stop()


def main() -> None:
    run_server()


if __name__ == "__main__":
    main()
