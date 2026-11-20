#!/usr/bin/env python3
"""
3DS OCR & Translation Server - Desktop GUI Launcher
====================================================
Dark-mode desktop frontend that manages the external OCR server,
dictionaries, 3DS QR pairing, live dual-screen OCR preview, and logs.
"""

import json
import os
import queue
import re
import secrets
import shutil
import socket
import subprocess
import sys
import threading
import time
import tkinter as tk
import urllib.request
from tkinter import messagebox, ttk
from typing import Any, Callable, Dict, Optional, Tuple

import qrcode
from PIL import Image, ImageDraw, ImageFont, ImageTk

from anki_dialog import (
    AI_MODEL_OPTIONS,
    AnkiSettingsDialog,
    OCR_MODE_OPTIONS,
    _load_anki_config,
    ai_model_to_label,
    get_anki_summary,
    label_to_ai_models,
    label_to_ocr_mode,
    ocr_mode_to_label,
    save_anki_config,
)
from ui_theme import (
    ACCENT,
    BG_CANVAS,
    BG_ELEVATED,
    BG_ROOT,
    BG_SURFACE,
    BORDER,
    BORDER_BTN,
    CYAN,
    DANGER,
    DANGER_BG,
    FG_MUTED,
    FG_PRIMARY,
    FG_SECONDARY,
    MONO_FONT,
    ModernTabBar,
    SUCCESS,
    SUCCESS_BG,
    UI_FONT,
    WARNING,
    WARNING_BG,
    apply_dark_theme,
    apply_dark_titlebar,
    create_app_icon,
    make_action_button,
    make_card,
    make_secondary_button,
)

# Base directory resolution
if getattr(sys, "frozen", False):
    APP_DIR = os.path.dirname(os.path.abspath(sys.executable))
else:
    APP_DIR = os.path.dirname(os.path.abspath(__file__))

DICTIONARIES_DIR = os.path.join(APP_DIR, "dictionaries")
OCR_MODELS_DIR = os.path.join(APP_DIR, "ocr_models")
OCR_SERVER_SCRIPT = os.path.join(APP_DIR, "ocr_server.py")
OCR_PSK_PATH = os.path.join(APP_DIR, "ocr_psk.txt")
ANKI_CONFIG_PATH = os.path.join(APP_DIR, "anki_config.json")
ANKI_CREDENTIALS_PATH = os.path.join(APP_DIR, "anki_credentials.json")
DUMPS_DIR = os.path.join(APP_DIR, "dumps")
DEFAULT_PORT = 8888
MAX_LOG_LINES = 5000


def find_system_python() -> str:
    """Locate a valid Python 3 interpreter with required dependencies."""
    candidates = [
        sys.executable if not getattr(sys, "frozen", False) else None,
        os.environ.get("PYTHON_EXECUTABLE", ""),
        shutil.which("python"),
        shutil.which("python3"),
        shutil.which("py"),
    ]
    local_appdata = os.environ.get("LOCALAPPDATA", "")
    if local_appdata:
        py_ver = f"Python{sys.version_info.major}{sys.version_info.minor}"
        candidates.append(os.path.join(local_appdata, "Programs", "Python", py_ver, "python.exe"))
        candidates.append(os.path.join(local_appdata, "Programs", "Python", "Python310", "python.exe"))
    for c in candidates:
        if c and os.path.isfile(c) and not c.lower().endswith("ocr_server.exe"):
            return os.path.abspath(c)
    return "python"


def get_local_lan_ip() -> str:
    """Detect LAN IPv4 address."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = "127.0.0.1"
    finally:
        s.close()
    return ip


def get_public_wan_ip() -> str:
    """Detect external WAN IPv4 address."""
    for service in ("https://api.ipify.org", "https://ifconfig.me/ip", "https://icanhazip.com"):
        try:
            req = urllib.request.Request(service, headers={"User-Agent": "3DS-OCR-Server/1.0"})
            with urllib.request.urlopen(req, timeout=3.0) as resp:
                ip = resp.read().decode("utf-8").strip()
                if re.match(r"^\d{1,3}(?:\.\d{1,3}){3}$", ip):
                    return ip
        except Exception:
            continue
    return ""


def load_ocr_psk() -> str:
    """Load or generate the 256-bit OCR PSK."""
    if os.path.isfile(OCR_PSK_PATH):
        try:
            with open(OCR_PSK_PATH, "r", encoding="utf-8") as f:
                key = f.read().strip()
                if key:
                    return key
        except Exception:
            pass
    new_psk = secrets.token_urlsafe(32)
    try:
        with open(OCR_PSK_PATH, "w", encoding="utf-8") as f:
            f.write(new_psk + "\n")
    except Exception:
        pass
    return new_psk


def save_ocr_psk(psk: str) -> None:
    """Save PSK to disk."""
    with open(OCR_PSK_PATH, "w", encoding="utf-8") as f:
        f.write(psk + "\n")


class ScreenPreviewWidget(ttk.Frame):
    """
    Interactive preview viewer for sent 3DS screenshots with live OCR
    bounding boxes, text overlays, and word inspection.
    """

    def __init__(
        self,
        parent: tk.Widget,
        dumps_dir: str,
        on_popout: Optional[Callable[[], None]] = None,
    ) -> None:
        super().__init__(parent)
        self.dumps_dir = dumps_dir
        self.on_popout = on_popout

        self.show_overlay_var = tk.BooleanVar(value=True)
        self.view_mode_var = tk.StringVar(value="Both")
        self.scale_var = tk.StringVar(value="1.5x")
        self.auto_refresh_var = tk.BooleanVar(value=True)

        self.last_img_mtime: float = 0.0
        self.last_json_mtime: float = 0.0
        self.current_tk_img: Optional[ImageTk.PhotoImage] = None

        self._load_font()
        self._build_ui()
        self.refresh()
        self._poll_auto_refresh()

    def _load_font(self) -> None:
        font_candidates = [
            r"C:\Windows\Fonts\msgothic.ttc",
            r"C:\Windows\Fonts\YuGothM.ttc",
            r"C:\Windows\Fonts\meiryo.ttc",
            r"C:\Windows\Fonts\arial.ttf",
        ]
        self.jp_font_path = next((f for f in font_candidates if os.path.isfile(f)), None)
        try:
            if self.jp_font_path:
                self.jp_font = ImageFont.truetype(self.jp_font_path, 11)
                self.jp_font_bold = ImageFont.truetype(self.jp_font_path, 12)
            else:
                self.jp_font = ImageFont.load_default()
                self.jp_font_bold = ImageFont.load_default()
        except Exception:
            self.jp_font = ImageFont.load_default()
            self.jp_font_bold = ImageFont.load_default()

    def _build_ui(self) -> None:
        # Top Controls Toolbar Card
        toolbar_card, toolbar = make_card(self, padding=(10, 8))
        toolbar_card.pack(fill="x", pady=(0, 8))

        ttk.Checkbutton(
            toolbar,
            text="Overlay OCR Results",
            variable=self.show_overlay_var,
            command=self.refresh,
        ).pack(side="left", padx=(0, 14))

        ttk.Label(toolbar, text="Screen:", style="SurfaceBold.TLabel").pack(side="left", padx=(0, 5))
        mode_menu = ttk.Combobox(
            toolbar,
            textvariable=self.view_mode_var,
            values=["Both", "Top", "Bottom"],
            width=8,
            state="readonly",
        )
        mode_menu.pack(side="left", padx=(0, 14))
        mode_menu.bind("<<ComboboxSelected>>", lambda _e: self.refresh())

        ttk.Label(toolbar, text="Scale:", style="SurfaceBold.TLabel").pack(side="left", padx=(0, 5))
        scale_menu = ttk.Combobox(
            toolbar,
            textvariable=self.scale_var,
            values=["1.0x", "1.5x", "2.0x"],
            width=6,
            state="readonly",
        )
        scale_menu.pack(side="left", padx=(0, 14))
        scale_menu.bind("<<ComboboxSelected>>", lambda _e: self.refresh())

        ttk.Checkbutton(
            toolbar,
            text="Auto-Refresh",
            variable=self.auto_refresh_var,
        ).pack(side="left", padx=(0, 10))

        make_secondary_button(toolbar, text="Refresh", width=9, command=self.refresh, padx=8, pady=4).pack(
            side="left"
        )

        if self.on_popout:
            make_secondary_button(
                toolbar, text="Pop Out Window", command=self.on_popout, padx=12, pady=4
            ).pack(side="right")

        # Main Canvas Area with Dark Border & Scrollbars
        canvas_border = tk.Frame(self, bg=BORDER, bd=0, highlightthickness=0)
        canvas_border.pack(fill="both", expand=True, pady=(0, 8))

        canvas_container = tk.Frame(canvas_border, bg=BG_CANVAS, bd=0, highlightthickness=0)
        canvas_container.pack(fill="both", expand=True, padx=1, pady=1)

        self.h_scroll = ttk.Scrollbar(canvas_container, orient="horizontal")
        self.v_scroll = ttk.Scrollbar(canvas_container, orient="vertical")

        self.canvas = tk.Canvas(
            canvas_container,
            bg=BG_CANVAS,
            highlightthickness=0,
            bd=0,
            xscrollcommand=self.h_scroll.set,
            yscrollcommand=self.v_scroll.set,
        )

        self.h_scroll.config(command=self.canvas.xview)
        self.v_scroll.config(command=self.canvas.yview)

        self.h_scroll.pack(side="bottom", fill="x")
        self.v_scroll.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)

        self.canvas.bind("<MouseWheel>", self._on_mousewheel)

        # Bottom Inspector Card
        info_card, info_frame = make_card(self, title="OCR & Cursor Inspector", padding=(12, 10))
        info_card.pack(fill="x")

        self.title_var = tk.StringVar(value="Title: (No active 3DS session)")
        ttk.Label(info_frame, textvariable=self.title_var, style="SurfaceBold.TLabel").pack(anchor="w")

        self.word_var = tk.StringVar(value="Word: —")
        ttk.Label(info_frame, textvariable=self.word_var, style="Accent.TLabel").pack(anchor="w", pady=(3, 1))

        self.gloss_var = tk.StringVar(value="Definition: —")
        self.gloss_lbl = ttk.Label(
            info_frame,
            textvariable=self.gloss_var,
            style="Surface.TLabel",
            wraplength=760,
        )
        self.gloss_lbl.pack(anchor="w", fill="x")
        info_frame.bind(
            "<Configure>",
            lambda e: self.gloss_lbl.configure(wraplength=max(300, e.width - 28)),
        )

        self.meta_var = tk.StringVar(value="Engines: —  •  Cursor: —")
        ttk.Label(info_frame, textvariable=self.meta_var, style="SurfaceMuted.TLabel").pack(anchor="w", pady=(4, 0))

    def _on_mousewheel(self, event: tk.Event) -> None:
        if self.canvas.yview() != (0.0, 1.0):
            self.canvas.yview_scroll(int(-1 * (event.delta / 120)), "units")

    def _draw_pill(
        self,
        draw: ImageDraw.ImageDraw,
        x: int,
        y: int,
        text: str,
        bg_color: Tuple[int, int, int, int],
        text_color: Tuple[int, int, int, int],
        max_w: int,
    ) -> None:
        """Draw a compact, anti-aliased text pill banner above or near a bounding box."""
        pad = 2
        bbox = draw.textbbox((x, y), text, font=self.jp_font)
        px1, py1, px2, py2 = bbox[0] - pad, bbox[1] - pad, bbox[2] + pad, bbox[3] + pad

        if px2 > max_w - 2:
            dx = (max_w - 2) - px2
            px1 += dx
            px2 += dx
            x += dx
        if px1 < 2:
            dx = 2 - px1
            px1 += dx
            px2 += dx
            x += dx
        if py1 < 2:
            dy = 2 - py1
            py1 += dy
            py2 += dy
            y += dy

        draw.rounded_rectangle(
            [px1, py1, px2, py2],
            radius=3,
            fill=bg_color,
            outline=(255, 255, 255, 140),
            width=1,
        )
        draw.text((x, y), text, font=self.jp_font, fill=text_color)

    def refresh(self) -> None:
        """Reload screenshot image and overlay latest OCR bounding boxes and results."""
        mode = self.view_mode_var.get()
        if mode == "Top":
            img_name = "latest_top.png"
        elif mode == "Bottom":
            img_name = "latest_bot.png"
        else:
            img_name = "latest_both.png"

        img_path = os.path.join(self.dumps_dir, img_name)
        json_path = os.path.join(self.dumps_dir, "latest_ocr.json")

        if not os.path.isfile(img_path):
            self.canvas.delete("all")
            cw = max(self.canvas.winfo_width(), 560)
            ch = max(self.canvas.winfo_height(), 320)
            self.canvas.create_text(
                cw // 2,
                ch // 2,
                text="No 3DS screenshots captured yet.\n\nConnect your 3DS and press the OCR hotkey to capture.",
                fill=FG_MUTED,
                font=(UI_FONT, 11),
                justify="center",
            )
            return

        try:
            self.last_img_mtime = os.path.getmtime(img_path)
            base_img = Image.open(img_path).convert("RGBA")
        except Exception:
            return

        ocr_data: Optional[Dict[str, Any]] = None
        if os.path.isfile(json_path):
            try:
                self.last_json_mtime = os.path.getmtime(json_path)
                with open(json_path, "r", encoding="utf-8") as f:
                    ocr_data = json.load(f)
            except Exception:
                ocr_data = None

        if ocr_data:
            game_str = ocr_data.get("game_name") or "Unknown Title"
            tid_str = ocr_data.get("title_id") or ""
            ts = ocr_data.get("timestamp", 0)
            time_str = time.strftime("%H:%M:%S", time.localtime(ts)) if ts else ""
            self.title_var.set(f"{game_str} {tid_str}  [{time_str}]".strip())

            lc = ocr_data.get("last_click")
            if lc:
                head = lc.get("headword", "")
                surf = lc.get("matched_surface", "")
                read = lc.get("reading", "")
                pos = lc.get("pos", "")
                glosses = lc.get("glosses", [])
                sc_name = "Top" if lc.get("screen_id") == 0 else "Bottom"
                cx, cy = lc.get("x", 0), lc.get("y", 0)
                engs = "+".join(lc.get("engines", []))

                read_str = f" 【{read}】" if (read and read != head) else ""
                self.word_var.set(f"{head}{read_str}  •  Matched: '{surf}'  •  {pos}")
                self.gloss_var.set("Definition: " + (glosses[0] if glosses else "No gloss found"))
                self.meta_var.set(f"Engines: [{engs}]  •  Cursor: {sc_name} Screen ({cx}, {cy})")
            else:
                self.word_var.set("Word: (No word tapped yet — tap a word on 3DS)")
                self.gloss_var.set("Definition: —")
                self.meta_var.set("Engines: —  •  Cursor: —")
        else:
            self.title_var.set("Title: Standby")
            self.word_var.set("Word: —")
            self.gloss_var.set("Definition: —")
            self.meta_var.set("Engines: —  •  Cursor: —")

        if self.show_overlay_var.get() and ocr_data and "screens" in ocr_data:
            overlay = Image.new("RGBA", base_img.size, (0, 0, 0, 0))
            draw = ImageDraw.Draw(overlay)
            img_w, _img_h = base_img.size

            top_lines = ocr_data["screens"].get("top", {}).get("lines", [])
            bot_lines = ocr_data["screens"].get("bot", {}).get("lines", [])

            if mode in ("Both", "Top"):
                for item in top_lines:
                    x1, y1, x2, y2 = item["bbox"]
                    draw.rectangle([x1, y1, x2, y2], outline=(0, 229, 255, 230), width=2)
                    lbl_y = y1 - 15 if y1 >= 15 else y2 + 2
                    self._draw_pill(
                        draw,
                        x1,
                        lbl_y,
                        item.get("text", ""),
                        bg_color=(0, 50, 80, 220),
                        text_color=(200, 250, 255, 255),
                        max_w=img_w,
                    )

            if mode in ("Both", "Bottom"):
                ox = 40 if mode == "Both" else 0
                oy = 244 if mode == "Both" else 0
                for item in bot_lines:
                    x1, y1, x2, y2 = item["bbox"]
                    bx1, by1, bx2, by2 = x1 + ox, y1 + oy, x2 + ox, y2 + oy
                    draw.rectangle([bx1, by1, bx2, by2], outline=(0, 230, 118, 230), width=2)
                    lbl_y = by1 - 15 if (by1 >= (oy + 15)) else by2 + 2
                    self._draw_pill(
                        draw,
                        bx1,
                        lbl_y,
                        item.get("text", ""),
                        bg_color=(0, 60, 30, 220),
                        text_color=(200, 255, 220, 255),
                        max_w=img_w,
                    )

            lc = ocr_data.get("last_click")
            if lc:
                sc = lc.get("screen_id", 0)
                should_draw_click = (
                    (mode == "Both")
                    or (mode == "Top" and sc == 0)
                    or (mode == "Bottom" and sc == 1)
                )
                if should_draw_click:
                    ox = 40 if (mode == "Both" and sc == 1) else 0
                    oy = 244 if (mode == "Both" and sc == 1) else 0

                    for b in lc.get("word_bboxes", []):
                        wx1, wy1, wx2, wy2 = b[0] + ox, b[1] + oy, b[2] + ox, b[3] + oy
                        draw.rectangle(
                            [wx1 - 1, wy1 - 1, wx2 + 1, wy2 + 1],
                            outline=(255, 215, 0, 255),
                            width=2,
                        )

                    cx, cy = lc.get("x", 0) + ox, lc.get("y", 0) + oy
                    r = 6
                    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=(255, 50, 50, 255), width=2)
                    draw.line([cx - r - 2, cy, cx + r + 2, cy], fill=(255, 50, 50, 255), width=2)
                    draw.line([cx, cy - r - 2, cx, cy + r + 2], fill=(255, 50, 50, 255), width=2)

            base_img = Image.alpha_composite(base_img, overlay)

        scale_val = self.scale_var.get()
        scale = 1.5 if scale_val == "1.5x" else (2.0 if scale_val == "2.0x" else 1.0)
        final_img = base_img.convert("RGB")
        if scale != 1.0:
            nw = int(final_img.width * scale)
            nh = int(final_img.height * scale)
            final_img = final_img.resize((nw, nh), Image.Resampling.NEAREST)

        self.current_tk_img = ImageTk.PhotoImage(final_img)
        self.canvas.delete("all")

        cw = max(self.canvas.winfo_width(), final_img.width)
        ch = max(self.canvas.winfo_height(), final_img.height)
        self.canvas.config(scrollregion=(0, 0, max(cw, final_img.width), max(ch, final_img.height)))
        self.canvas.create_image(
            max(0, (cw - final_img.width) // 2),
            max(0, (ch - final_img.height) // 2),
            anchor="nw",
            image=self.current_tk_img,
        )

    def _poll_auto_refresh(self) -> None:
        """Check for file mtime changes every 400ms when Auto-Refresh is enabled."""
        if self.auto_refresh_var.get():
            mode = self.view_mode_var.get()
            img_name = (
                "latest_top.png"
                if mode == "Top"
                else ("latest_bot.png" if mode == "Bottom" else "latest_both.png")
            )
            img_path = os.path.join(self.dumps_dir, img_name)
            json_path = os.path.join(self.dumps_dir, "latest_ocr.json")

            needs_reload = False
            if os.path.isfile(img_path):
                try:
                    mtime = os.path.getmtime(img_path)
                    if mtime != self.last_img_mtime:
                        needs_reload = True
                except Exception:
                    pass

            if os.path.isfile(json_path):
                try:
                    jmtime = os.path.getmtime(json_path)
                    if jmtime != self.last_json_mtime:
                        needs_reload = True
                except Exception:
                    pass

            if needs_reload:
                self.refresh()

        self.after(400, self._poll_auto_refresh)


class OcrServerLauncherGui:
    """Dark-mode GUI front-end that controls the external OCR server backend."""

    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("3DS OCR & Translation Server")
        self.root.geometry("880x750")
        self.root.minsize(780, 700)

        apply_dark_theme(self.root)
        apply_dark_titlebar(self.root)
        try:
            self._app_icon = create_app_icon()
            self.root.iconphoto(True, self._app_icon)
        except Exception:
            self._app_icon = None

        self.python_bin = find_system_python()
        self.current_psk = load_ocr_psk()
        self.server_proc: Optional[subprocess.Popen] = None
        self.log_queue: queue.Queue = queue.Queue()
        self.reader_thread: Optional[threading.Thread] = None
        self.qr_image_tk = None
        self.qr_placeholder_tk = None
        self.qr_revealed = False
        self.psk_revealed = False
        self.popout_window: Optional[tk.Toplevel] = None
        self.log_line_count = 0

        self._build_ui()
        self._refresh_qr_code()
        self._detect_wan_async()
        self._poll_log_queue()

        # Auto-start server on launch
        self.root.after(300, self._start_server)

    def _build_ui(self) -> None:
        # Top Header Bar
        header = ttk.Frame(self.root, padding=(16, 14, 16, 10))
        header.pack(fill="x")

        title_col = ttk.Frame(header)
        title_col.pack(side="left", fill="x", expand=True)

        ttk.Label(title_col, text="3DS OCR & Translation Server", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            title_col,
            text="Modular Frontend  •  External OCR Engines, Dictionaries & Anki Sync",
            style="Muted.TLabel",
        ).pack(anchor="w", pady=(2, 0))

        ctrl_col = ttk.Frame(header)
        ctrl_col.pack(side="right")

        # Status Badge Pill
        self.status_badge_border = tk.Frame(ctrl_col, bg=DANGER, bd=0, highlightthickness=0)
        self.status_badge_border.pack(side="left", padx=(0, 10))
        self.status_var = tk.StringVar(value="○  Server Stopped")
        self.status_lbl = tk.Label(
            self.status_badge_border,
            textvariable=self.status_var,
            font=(UI_FONT, 9, "bold"),
            bg=DANGER_BG,
            fg=DANGER,
            padx=12,
            pady=6,
        )
        self.status_lbl.pack(padx=1, pady=1)

        self.server_btn = make_action_button(
            ctrl_col,
            text="▶  Start Server",
            command=self._toggle_server,
            variant="success",
            padx=16,
            pady=6,
            font_size=9,
        )
        self.server_btn.pack(side="left")

        # Sleek Underline Tab Bar & Content Area (replaces Win98 ttk.Notebook)
        tab_bar_container = tk.Frame(self.root, bg=BG_ROOT, bd=0, highlightthickness=0)
        tab_bar_container.pack(fill="x", padx=16, pady=(0, 6))

        self.tab_content = ttk.Frame(self.root)
        self.tab_content.pack(fill="both", expand=True, padx=16, pady=(0, 14))

        self.tab_bar = ModernTabBar(tab_bar_container, self.tab_content)
        self.tab_bar.pack(fill="x")

        # Tab 1: Server & Pairing
        self.tab_server = ttk.Frame(self.tab_content, padding=(0, 6, 0, 0))
        self.tab_bar.add_tab("server", "Server & Pairing", self.tab_server)

        # Tab 2: Screen Preview
        self.tab_preview = ttk.Frame(self.tab_content, padding=(0, 6, 0, 0))
        self.tab_bar.add_tab("preview", "Screen Preview", self.tab_preview)

        # Tab 3: Server Logs
        self.tab_logs = ttk.Frame(self.tab_content, padding=(0, 6, 0, 0))
        self.tab_bar.add_tab("logs", "Server Logs", self.tab_logs)

        self._build_server_tab()
        self.preview_widget = ScreenPreviewWidget(
            self.tab_preview,
            dumps_dir=DUMPS_DIR,
            on_popout=self._open_preview_popout,
        )
        self.preview_widget.pack(fill="both", expand=True)
        self._build_logs_tab()

    def _build_server_tab(self) -> None:
        parent = self.tab_server
        parent.columnconfigure(0, weight=5, uniform="server_cols")
        parent.columnconfigure(1, weight=4, uniform="server_cols")
        parent.rowconfigure(0, weight=1)

        left_col = ttk.Frame(parent)
        left_col.grid(row=0, column=0, sticky="nsew", padx=(0, 8))

        right_col = ttk.Frame(parent)
        right_col.grid(row=0, column=1, sticky="nsew", padx=(4, 0))

        # --- Left Column Card 1: Backend & OCR Engine ---
        ocr_card, ocr_body = make_card(left_col, title="Backend & OCR Engine", padding=(12, 10))
        ocr_card.pack(fill="x", pady=(0, 8))
        ocr_body.columnconfigure(0, minsize=126)
        ocr_body.columnconfigure(1, weight=1)

        backend_exists = os.path.isfile(OCR_SERVER_SCRIPT)
        b_text = "ocr_server.py  •  Ready" if backend_exists else "ocr_server.py  •  MISSING"
        b_color = SUCCESS if backend_exists else DANGER

        ttk.Label(ocr_body, text="Backend Script:", style="SurfaceBold.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 8)
        )
        tk.Label(
            ocr_body,
            text=b_text,
            font=(UI_FONT, 9, "bold"),
            bg=BG_SURFACE,
            fg=b_color,
        ).grid(row=0, column=1, sticky="w", padx=(10, 0), pady=(0, 8))

        ttk.Label(ocr_body, text="OCR Mode:", style="SurfaceBold.TLabel").grid(
            row=1, column=0, sticky="w", pady=(0, 8)
        )
        self.ocr_mode_var = tk.StringVar(
            value=ocr_mode_to_label(_load_anki_config().get("ocr_mode", "local_cached"))
        )
        ocr_mode_menu = ttk.Combobox(
            ocr_body,
            textvariable=self.ocr_mode_var,
            values=[lbl for lbl, _ in OCR_MODE_OPTIONS],
            state="readonly",
        )
        ocr_mode_menu.grid(row=1, column=1, sticky="ew", padx=(10, 0), pady=(0, 8))
        ocr_mode_menu.bind("<<ComboboxSelected>>", self._on_ocr_mode_changed)

        ttk.Label(ocr_body, text="AI Model:", style="SurfaceBold.TLabel").grid(
            row=2, column=0, sticky="w", pady=(0, 8)
        )
        self.ai_model_var = tk.StringVar(
            value=ai_model_to_label(_load_anki_config().get("gemini_model", "gemini-3.5-flash-lite"))
        )
        ai_model_menu = ttk.Combobox(
            ocr_body,
            textvariable=self.ai_model_var,
            values=[lbl for lbl, _, _ in AI_MODEL_OPTIONS],
            state="readonly",
        )
        ai_model_menu.grid(row=2, column=1, sticky="ew", padx=(10, 0), pady=(0, 8))
        ai_model_menu.bind("<<ComboboxSelected>>", self._on_ai_model_changed)

        self.gemini_vision_var = tk.BooleanVar(
            value=bool(_load_anki_config().get("gemini_use_vision", True))
        )
        ttk.Checkbutton(
            ocr_body,
            text="Pass 3DS screenshot to Gemini (visual context)",
            variable=self.gemini_vision_var,
            command=self._on_gemini_vision_toggled,
        ).grid(row=3, column=0, columnspan=2, sticky="w", pady=(2, 0))

        # --- Left Column Card 2: Dictionaries & Anki Sync ---
        res_card, res_body = make_card(left_col, title="Dictionaries & Anki Sync", padding=(12, 10))
        res_card.pack(fill="x", pady=(0, 8))
        res_body.columnconfigure(0, minsize=126)
        res_body.columnconfigure(1, weight=1)

        self.dict_summary_var = tk.StringVar(value=self._get_dictionary_summary())
        ttk.Label(res_body, text="Dictionaries:", style="SurfaceBold.TLabel").grid(
            row=0, column=0, sticky="w", pady=(0, 6)
        )
        ttk.Label(
            res_body,
            textvariable=self.dict_summary_var,
            style="Surface.TLabel",
            wraplength=260,
        ).grid(row=0, column=1, sticky="w", padx=(10, 0), pady=(0, 6))

        dict_btns = ttk.Frame(res_body, style="Surface.TFrame")
        dict_btns.grid(row=1, column=0, columnspan=2, sticky="w", pady=(0, 10))
        make_secondary_button(
            dict_btns, text="Reload Dictionaries", command=self._reload_resources, padx=12, pady=4
        ).pack(side="left", padx=(0, 8))
        make_secondary_button(
            dict_btns, text="Open Folder", command=self._open_dict_folder, padx=12, pady=4
        ).pack(side="left")

        tk.Frame(res_body, bg=BORDER, height=1).grid(row=2, column=0, columnspan=2, sticky="ew", pady=(0, 10))

        ttk.Label(res_body, text="Anki Sync:", style="SurfaceBold.TLabel").grid(
            row=3, column=0, sticky="w", pady=(0, 6)
        )
        self.anki_summary_var = tk.StringVar(value=get_anki_summary())
        ttk.Label(res_body, textvariable=self.anki_summary_var, style="Surface.TLabel").grid(
            row=3, column=1, sticky="w", padx=(10, 0), pady=(0, 6)
        )
        make_secondary_button(
            res_body, text="Configure Anki...", command=self._open_anki_dialog, padx=12, pady=4
        ).grid(row=4, column=0, columnspan=2, sticky="w")

        # --- Left Column Card 3: Network Configuration ---
        net_card, net_body = make_card(left_col, title="Network Configuration", padding=(12, 10))
        net_card.pack(fill="both", expand=True)
        net_body.columnconfigure(0, minsize=126)
        net_body.columnconfigure(1, weight=1)

        ttk.Label(net_body, text="Primary IP (LAN):", style="SurfaceBold.TLabel").grid(
            row=0, column=0, sticky="w", pady=5
        )
        self.lan_var = tk.StringVar(value=get_local_lan_ip())
        self.lan_entry = ttk.Entry(net_body, textvariable=self.lan_var)
        self.lan_entry.grid(row=0, column=1, columnspan=2, sticky="ew", padx=(10, 0), pady=5)
        self.lan_entry.bind("<KeyRelease>", lambda _e: self._refresh_qr_code())

        ttk.Label(net_body, text="Secondary IP (WAN):", style="SurfaceBold.TLabel").grid(
            row=1, column=0, sticky="w", pady=5
        )
        self.wan_var = tk.StringVar(value="")
        self.wan_entry = ttk.Entry(net_body, textvariable=self.wan_var)
        self.wan_entry.grid(row=1, column=1, sticky="ew", padx=(10, 8), pady=5)
        self.wan_entry.bind("<KeyRelease>", lambda _e: self._refresh_qr_code())

        make_secondary_button(
            net_body, text="Detect", width=7, command=self._detect_wan_async, padx=8, pady=4
        ).grid(row=1, column=2, pady=5)

        ttk.Label(net_body, text="Server Port:", style="SurfaceBold.TLabel").grid(
            row=2, column=0, sticky="w", pady=5
        )
        self.port_var = tk.StringVar(value=str(DEFAULT_PORT))
        self.port_entry = ttk.Entry(net_body, textvariable=self.port_var, width=10)
        self.port_entry.grid(row=2, column=1, sticky="w", padx=(10, 0), pady=5)
        self.port_entry.bind("<KeyRelease>", lambda _e: self._refresh_qr_code())

        self.endpoint_preview_var = tk.StringVar(value="")
        ttk.Label(net_body, textvariable=self.endpoint_preview_var, style="SurfaceMuted.TLabel").grid(
            row=3, column=0, columnspan=3, sticky="w", pady=(8, 0)
        )

        # --- Right Column Card: 3DS Pairing & Authentication ---
        pair_card, pair_body = make_card(right_col, title="3DS Camera Pairing & PSK", padding=(14, 12))
        pair_card.pack(fill="both", expand=True)

        ttk.Label(
            pair_body,
            text="In-Game OCR: Press [Start] on your 3DS and scan this QR code to pair your endpoints and key.",
            style="SurfaceMuted.TLabel",
            wraplength=290,
            justify="center",
        ).pack(pady=(0, 10))

        # QR code frame with toggleable reveal
        qr_outer = tk.Frame(pair_body, bg=BORDER, bd=0, highlightthickness=0)
        qr_outer.pack(pady=(0, 8))
        self.qr_pad = tk.Frame(qr_outer, bg=BG_SURFACE, padx=10, pady=10)
        self.qr_pad.pack(padx=1, pady=1)
        self.qr_canvas = tk.Label(self.qr_pad, bg=BG_SURFACE, bd=0, cursor="hand2")
        self.qr_canvas.pack()
        self.qr_canvas.bind("<Button-1>", lambda _e: self._toggle_qr_visibility())

        self.toggle_qr_btn = make_secondary_button(
            pair_body,
            text="Show QR Code",
            command=self._toggle_qr_visibility,
            padx=14,
            pady=4,
        )
        self.toggle_qr_btn.pack(pady=(0, 10))

        tk.Frame(pair_body, bg=BORDER, height=1).pack(fill="x", pady=(0, 10))

        ttk.Label(pair_body, text="Pre-Shared Key (256-bit PSK)", style="SurfaceBold.TLabel").pack(
            anchor="w", pady=(0, 6)
        )

        psk_box = ttk.Frame(pair_body, style="Surface.TFrame")
        psk_box.pack(fill="x", pady=(0, 10))

        self.psk_var = tk.StringVar(value=self.current_psk)
        self.psk_entry = ttk.Entry(
            psk_box,
            textvariable=self.psk_var,
            font=(MONO_FONT, 9),
            state="readonly",
            show="•",
        )
        self.psk_entry.pack(side="left", fill="x", expand=True, padx=(0, 6))

        self.toggle_psk_btn = make_secondary_button(
            psk_box, text="Show", width=5, command=self._toggle_psk_visibility, padx=6, pady=4
        )
        self.toggle_psk_btn.pack(side="left", padx=(0, 4))

        self.copy_psk_btn = make_secondary_button(
            psk_box, text="Copy", width=5, command=self._copy_psk, padx=6, pady=4
        )
        self.copy_psk_btn.pack(side="left")

        gen_btn = make_action_button(
            pair_body,
            text="Generate New Key & QR Code",
            command=self._generate_new_psk,
            variant="primary",
            padx=12,
            pady=7,
        )
        gen_btn.pack(fill="x", pady=(2, 0))

    def _build_logs_tab(self) -> None:
        parent = self.tab_logs

        # Top Log Toolbar Card
        bar_card, bar = make_card(parent, padding=(12, 8))
        bar_card.pack(fill="x", pady=(0, 8))

        ttk.Label(bar, text="Live Server & OCR Activity", style="CardHeader.TLabel").pack(side="left")

        self.log_count_var = tk.StringVar(value="0 lines")
        ttk.Label(bar, textvariable=self.log_count_var, style="SurfaceMuted.TLabel").pack(
            side="left", padx=(12, 0)
        )

        make_secondary_button(bar, text="Clear", width=7, command=self._clear_logs, padx=8, pady=4).pack(
            side="right"
        )
        self.copy_log_btn = make_secondary_button(
            bar, text="Copy All", width=8, command=self._copy_logs, padx=8, pady=4
        )
        self.copy_log_btn.pack(side="right", padx=(0, 8))

        self.auto_scroll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            bar,
            text="Auto-scroll",
            variable=self.auto_scroll_var,
        ).pack(side="right", padx=(0, 14))

        # Terminal Container with 1px dark border and dark ttk scrollbar
        log_border = tk.Frame(parent, bg=BORDER, bd=0, highlightthickness=0)
        log_border.pack(fill="both", expand=True)

        log_inner = tk.Frame(log_border, bg=BG_CANVAS, bd=0, highlightthickness=0)
        log_inner.pack(fill="both", expand=True, padx=1, pady=1)

        self.log_scroll = ttk.Scrollbar(log_inner, orient="vertical")
        self.log_scroll.pack(side="right", fill="y")

        self.log_text = tk.Text(
            log_inner,
            wrap="word",
            font=(MONO_FONT, 9),
            bg=BG_CANVAS,
            fg="#CBD5E1",
            insertbackground=FG_PRIMARY,
            selectbackground=ACCENT,
            selectforeground="#FFFFFF",
            relief="flat",
            bd=0,
            padx=12,
            pady=10,
            yscrollcommand=self.log_scroll.set,
            state="disabled",
        )
        self.log_text.pack(side="left", fill="both", expand=True)
        self.log_scroll.config(command=self.log_text.yview)

        # Syntax / severity color tags
        self.log_text.tag_configure("error", foreground="#F87171")
        self.log_text.tag_configure("warn", foreground="#FBBF24")
        self.log_text.tag_configure("info", foreground="#60A5FA")
        self.log_text.tag_configure("success", foreground="#34D399")
        self.log_text.tag_configure("dim", foreground=FG_MUTED)

    def _open_preview_popout(self) -> None:
        """Open or refocus a separate, floating Screen Preview window."""
        if self.popout_window and self.popout_window.winfo_exists():
            self.popout_window.lift()
            self.popout_window.focus_force()
            return

        self.popout_window = tk.Toplevel(self.root)
        self.popout_window.title("3DS Screen Preview & OCR Overlay")
        self.popout_window.geometry("720x880")
        self.popout_window.minsize(520, 620)
        self.popout_window.configure(bg=BG_ROOT)
        apply_dark_titlebar(self.popout_window)

        wrapper = ttk.Frame(self.popout_window, padding=10)
        wrapper.pack(fill="both", expand=True)

        popout_widget = ScreenPreviewWidget(
            wrapper,
            dumps_dir=DUMPS_DIR,
            on_popout=None,
        )
        popout_widget.pack(fill="both", expand=True)

    def _get_dictionary_summary(self) -> str:
        if not os.path.isdir(DICTIONARIES_DIR):
            return "dictionaries/ folder not found"
        files = [f for f in os.listdir(DICTIONARIES_DIR) if f.endswith((".db", ".zip", ".json"))]
        if not files:
            return "0 files in dictionaries/"
        preview = ", ".join(files[:2]) + ("..." if len(files) > 2 else "")
        return f"{len(files)} file(s) ({preview})"

    def _open_dict_folder(self) -> None:
        os.makedirs(DICTIONARIES_DIR, exist_ok=True)
        if sys.platform == "win32":
            os.startfile(DICTIONARIES_DIR)
        else:
            subprocess.Popen(["xdg-open", DICTIONARIES_DIR])

    def _reload_resources(self) -> None:
        self.dict_summary_var.set(self._get_dictionary_summary())
        if self.server_proc and self.server_proc.poll() is None:
            self._log_local("[INFO] Restarting server to apply updated dictionaries & resources...")
            self._stop_server()
            self.root.after(1000, self._start_server)
        else:
            messagebox.showinfo("Dictionaries", "Dictionaries refreshed. Start the server to load them.")

    def _on_gemini_vision_toggled(self) -> None:
        enabled = bool(self.gemini_vision_var.get())
        save_anki_config({"gemini_use_vision": enabled})
        self._log_local(
            f"[INFO] Gemini screenshot context {'enabled' if enabled else 'disabled'} (gemini_use_vision={enabled})."
        )

    def _on_ocr_mode_changed(self, _event=None) -> None:
        mode = label_to_ocr_mode(self.ocr_mode_var.get())
        save_anki_config({"ocr_mode": mode})
        self._log_local(
            f"[INFO] OCR mode switched -> {self.ocr_mode_var.get()} (ocr_mode={mode})."
        )

    def _on_ai_model_changed(self, _event=None) -> None:
        primary, fallback = label_to_ai_models(self.ai_model_var.get())
        save_anki_config({"gemini_model": primary, "gemini_fallback_model": fallback})
        fb_note = f", fallback={fallback}" if fallback else ""
        self._log_local(
            f"[INFO] AI model switched -> {self.ai_model_var.get()} (gemini_model={primary}{fb_note})."
        )

    def _open_anki_dialog(self) -> None:
        def on_saved():
            cfg = _load_anki_config()
            self.anki_summary_var.set(get_anki_summary())
            self.gemini_vision_var.set(bool(cfg.get("gemini_use_vision", True)))
            self.ocr_mode_var.set(ocr_mode_to_label(cfg.get("ocr_mode", "local_cached")))
            self.ai_model_var.set(ai_model_to_label(cfg.get("gemini_model", "gemini-3.5-flash-lite")))

        AnkiSettingsDialog(self.root, on_save_callback=on_saved)

    def _classify_log_tag(self, line: str) -> Optional[str]:
        upper = line.upper()
        if any(tok in upper for tok in ("[ERROR]", "ERROR:", "TRACEBACK", "EXCEPTION", "FAILED")):
            return "error"
        if any(tok in upper for tok in ("[WARN]", "WARNING")):
            return "warn"
        if any(tok in upper for tok in ("[ANKI]", "[OCR]", "SUCCESS", "READY", "RUNNING ON", "✔")):
            return "success"
        if "[INFO]" in upper or "INFO:" in upper:
            return "info"
        return None

    def _append_log_line(self, line: str) -> None:
        text = line if line.endswith("\n") else (line + "\n")
        tag = self._classify_log_tag(text)

        self.log_text.configure(state="normal")
        if tag:
            self.log_text.insert("end", text, tag)
        else:
            self.log_text.insert("end", text)
        self.log_line_count += text.count("\n")

        if self.log_line_count > MAX_LOG_LINES:
            excess = self.log_line_count - MAX_LOG_LINES
            self.log_text.delete("1.0", f"{excess + 1}.0")
            self.log_line_count = MAX_LOG_LINES

        if self.auto_scroll_var.get():
            self.log_text.see("end")
        self.log_text.configure(state="disabled")
        self.log_count_var.set(f"{self.log_line_count} lines")

    def _clear_logs(self) -> None:
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.configure(state="disabled")
        self.log_line_count = 0
        self.log_count_var.set("0 lines")

    def _copy_logs(self) -> None:
        content = self.log_text.get("1.0", "end-1c")
        self.root.clipboard_clear()
        self.root.clipboard_append(content)
        self.copy_log_btn.configure(text="Copied!")
        self.root.after(1500, lambda: self.copy_log_btn.configure(text="Copy All"))

    def _log_local(self, msg: str) -> None:
        self.log_queue.put(msg)

    def _poll_log_queue(self) -> None:
        processed = 0
        while processed < 100:
            try:
                line = self.log_queue.get_nowait()
            except queue.Empty:
                break
            else:
                self._append_log_line(line)
                processed += 1
        self.root.after(100, self._poll_log_queue)

    def _set_status_badge(self, text: str, fg: str, bg: str) -> None:
        self.status_var.set(text)
        self.status_lbl.configure(fg=fg, bg=bg)
        self.status_badge_border.configure(bg=fg)

    def _set_server_button(self, text: str, variant: str, state: str = "normal") -> None:
        self.server_btn.configure(text=text, variant=variant, state=state)

    def _toggle_server(self) -> None:
        if self.server_proc and self.server_proc.poll() is None:
            self._stop_server()
        else:
            self._start_server()

    def _start_server(self) -> None:
        if self.server_proc and self.server_proc.poll() is None:
            return

        if not os.path.isfile(OCR_SERVER_SCRIPT):
            messagebox.showerror("Error", f"Backend script not found:\n{OCR_SERVER_SCRIPT}")
            return

        try:
            port = int(self.port_var.get().strip())
        except ValueError:
            port = DEFAULT_PORT

        self._set_server_button("⏳  Starting...", "secondary", state="disabled")
        self._set_status_badge(f"⏳  Starting on :{port}...", WARNING, WARNING_BG)

        def run_proc():
            cmd = [self.python_bin, "-u", OCR_SERVER_SCRIPT, "--cli", "--port", str(port)]
            self._log_local(f"[INFO] Executing external server backend: {' '.join(cmd)}")
            try:
                creation_flags = 0x08000000 if sys.platform == "win32" else 0
                self.server_proc = subprocess.Popen(
                    cmd,
                    cwd=APP_DIR,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    bufsize=1,
                    creationflags=creation_flags,
                )
                pid = self.server_proc.pid
                self.root.after(
                    0,
                    lambda: [
                        self._set_status_badge(f"●  Running :{port} (PID {pid})", SUCCESS, SUCCESS_BG),
                        self._set_server_button("■  Stop Server", "danger", state="normal"),
                    ],
                )

                for raw_line in iter(self.server_proc.stdout.readline, ""):
                    if raw_line:
                        self.log_queue.put(raw_line)

                self.server_proc.stdout.close()
                self.server_proc.wait()
            except Exception as e:
                self._log_local(f"[ERROR] Failed to start server: {e}")
            finally:
                self.root.after(
                    0,
                    lambda: [
                        self._set_status_badge("○  Server Stopped", DANGER, DANGER_BG),
                        self._set_server_button("▶  Start Server", "success", state="normal"),
                    ],
                )

        self.reader_thread = threading.Thread(target=run_proc, daemon=True)
        self.reader_thread.start()

    def _stop_server(self) -> None:
        if self.server_proc and self.server_proc.poll() is None:
            self._set_server_button("⏳  Stopping...", "secondary", state="disabled")
            self._log_local("[INFO] Stopping external OCR server process...")
            try:
                self.server_proc.terminate()
            except Exception:
                pass

            def killer():
                time.sleep(2.0)
                if self.server_proc and self.server_proc.poll() is None:
                    try:
                        self.server_proc.kill()
                    except Exception:
                        pass

            threading.Thread(target=killer, daemon=True).start()

    def _copy_psk(self) -> None:
        self.root.clipboard_clear()
        self.root.clipboard_append(self.current_psk)
        self.copy_psk_btn.configure(text="Copied!")
        self.root.after(1500, lambda: self.copy_psk_btn.configure(text="Copy"))

    def _toggle_psk_visibility(self) -> None:
        """Toggle PSK text entry masking (hidden by default)."""
        self.psk_revealed = not self.psk_revealed
        if self.psk_revealed:
            self.psk_entry.configure(show="")
            self.toggle_psk_btn.configure(text="Hide")
        else:
            self.psk_entry.configure(show="•")
            self.toggle_psk_btn.configure(text="Show")

    def _toggle_qr_visibility(self) -> None:
        """Toggle QR code visibility (hidden by default to protect key from shoulder-surfing)."""
        self.qr_revealed = not self.qr_revealed
        self._update_qr_display()

    def _make_qr_placeholder(self, width: int = 180, height: int = 180) -> Image.Image:
        """Generate a sleek theme-consistent placeholder card when the QR code is hidden."""
        img = Image.new("RGB", (width, height), color=BG_SURFACE)
        draw = ImageDraw.Draw(img)
        draw.rectangle([(0, 0), (width - 1, height - 1)], outline=BORDER, width=1)

        cx = width // 2
        # Padlock shackle
        draw.arc([(cx - 18, 38), (cx + 18, 76)], start=180, end=0, fill=FG_SECONDARY, width=4)
        # Padlock body
        draw.rounded_rectangle([(cx - 24, 60), (cx + 24, 96)], radius=6, fill=BG_ELEVATED, outline=BORDER_BTN, width=2)
        # Keyhole
        draw.ellipse([(cx - 3, 72), (cx + 3, 78)], fill=FG_SECONDARY)
        draw.polygon([(cx - 2, 77), (cx + 2, 77), (cx + 4, 85), (cx - 4, 85)], fill=FG_SECONDARY)

        draw.text((cx, 120), "Pairing QR Hidden", fill=FG_PRIMARY, anchor="mm")
        draw.text((cx, 142), "Click to Reveal", fill=CYAN, anchor="mm")
        return img

    def _update_qr_display(self) -> None:
        """Update QR canvas and button state based on reveal toggle."""
        if not hasattr(self, "qr_canvas"):
            return
        if self.qr_revealed:
            if self.qr_image_tk is not None:
                self.qr_pad.configure(bg="#FFFFFF")
                self.qr_canvas.configure(image=self.qr_image_tk, bg="#FFFFFF")
            if hasattr(self, "toggle_qr_btn"):
                self.toggle_qr_btn.configure(text="Hide QR Code")
        else:
            if self.qr_placeholder_tk is not None:
                self.qr_pad.configure(bg=BG_SURFACE)
                self.qr_canvas.configure(image=self.qr_placeholder_tk, bg=BG_SURFACE)
            if hasattr(self, "toggle_qr_btn"):
                self.toggle_qr_btn.configure(text="Show QR Code")

    def _generate_new_psk(self) -> None:
        new_psk = secrets.token_urlsafe(32)
        self.current_psk = new_psk
        self.psk_var.set(new_psk)
        save_ocr_psk(new_psk)
        self._refresh_qr_code()
        self._log_local("[INFO] Generated fresh 256-bit PSK and updated QR pairing code.")
        messagebox.showinfo(
            "New Key Generated",
            "A fresh 256-bit PSK has been generated!\n\n"
            "Click 'Show QR Code' (or the placeholder) to view the updated QR code and pair with your 3DS.",
        )

    def _detect_wan_async(self) -> None:
        def worker():
            ip = get_public_wan_ip()
            self.root.after(0, lambda: [self.wan_var.set(ip), self._refresh_qr_code()])

        threading.Thread(target=worker, daemon=True).start()

    def _refresh_qr_code(self) -> None:
        lan = self.lan_var.get().strip() or get_local_lan_ip()
        wan = self.wan_var.get().strip()
        try:
            port = int(self.port_var.get().strip())
        except ValueError:
            port = DEFAULT_PORT

        payload = f"3DSOCR:lan={lan}:{port}"
        if wan:
            payload += f";wan={wan}:{port}"
        payload += f";psk={self.current_psk}"

        wan_note = f"  •  WAN: {wan}:{port}" if wan else ""
        self.endpoint_preview_var.set(f"Endpoint: {lan}:{port}{wan_note}")

        qr = qrcode.QRCode(box_size=4, border=2)
        qr.add_data(payload)
        qr.make(fit=True)

        img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
        self.qr_image_tk = ImageTk.PhotoImage(img)

        # Generate matching-sized placeholder for hidden state
        qr_w, qr_h = img.size
        placeholder_img = self._make_qr_placeholder(qr_w, qr_h)
        self.qr_placeholder_tk = ImageTk.PhotoImage(placeholder_img)

        self._update_qr_display()


def main() -> None:
    root = tk.Tk()
    app = OcrServerLauncherGui(root)
    try:
        root.mainloop()
    finally:
        if app.server_proc and app.server_proc.poll() is None:
            try:
                app.server_proc.terminate()
            except Exception:
                pass


if __name__ == "__main__":
    main()
