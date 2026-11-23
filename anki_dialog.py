#!/usr/bin/env python3
"""
anki_dialog.py - Anki Integration Settings Dialog for 3DS OCR Launcher
"""

import json
import logging
import os
import sys
import threading
import urllib.error
import urllib.parse
import urllib.request
from typing import Tuple
import tkinter as tk
from tkinter import ttk, messagebox

from ui_theme import (
    BG_ROOT,
    CYAN,
    DANGER,
    SUCCESS,
    UI_FONT,
    apply_dark_titlebar,
    center_window,
    make_action_button,
    make_card,
    make_secondary_button,
)

logger = logging.getLogger("3ds_ocr_anki")

if getattr(sys, "frozen", False):
    APP_DIR = os.path.dirname(os.path.abspath(sys.executable))
else:
    APP_DIR = os.path.dirname(os.path.abspath(__file__))

ANKI_CONFIG_PATH = os.path.join(APP_DIR, "anki_config.json")
ANKI_CREDENTIALS_PATH = os.path.join(APP_DIR, "anki_credentials.json")

OCR_MODE_OPTIONS = [
    ("Local 4-Engine (Cached)", "local_cached"),
    ("Hybrid: Local + AI (Parallel)", "hybrid_cached"),
    ("Gemini Batch OCR (Cached)", "gemini_cached"),
]

AI_MODEL_OPTIONS = [
    ("Gemma 4 26B (14.4K RPD)", "gemma-4-26b-a4b-it", ""),
    ("Gemini Flash (3.5 -> 3.1 Lite)", "gemini-3.5-flash-lite", "gemini-3.1-flash-lite"),
]


def ocr_mode_to_label(mode: str) -> str:
    m = str(mode or "local_cached").strip().lower()
    for label, key in OCR_MODE_OPTIONS:
        if key == m:
            return label
    return OCR_MODE_OPTIONS[0][0]


def label_to_ocr_mode(label: str) -> str:
    for opt_label, key in OCR_MODE_OPTIONS:
        if opt_label == label:
            return key
    return "local_cached"


def ai_model_to_label(model_name: str) -> str:
    m = str(model_name or "").strip().lower()
    if "gemma" in m:
        return AI_MODEL_OPTIONS[0][0]
    for label, primary, _ in AI_MODEL_OPTIONS:
        if primary.lower() == m:
            return label
    return AI_MODEL_OPTIONS[1][0]


def label_to_ai_models(label: str) -> Tuple[str, str]:
    for opt_label, primary, fallback in AI_MODEL_OPTIONS:
        if opt_label == label:
            return primary, fallback
    return AI_MODEL_OPTIONS[1][1], AI_MODEL_OPTIONS[1][2]


def _load_anki_config() -> dict:
    if os.path.exists(ANKI_CONFIG_PATH):
        try:
            with open(ANKI_CONFIG_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
                if isinstance(data, dict):
                    return data
        except Exception as exc:
            logger.warning("Failed reading %s: %s", ANKI_CONFIG_PATH, exc)
    return {
        "use_ankiconnect": True,
        "anki_mode": "ankiconnect",
        "ankiconnect_url": "http://127.0.0.1:8765",
        "deck_prefix": "3DS::",
        "subdeck_by_title": True,
        "model_name": "Japanese (recognition)",
        "player_name": "",
        "auto_sync_interval_sec": 300,
        "screenshot_mode": "clicked",
        "multi_user_mode": False,
    }


def save_anki_config(cfg_updates: dict) -> None:
    current = _load_anki_config()
    current.update(cfg_updates)
    try:
        with open(ANKI_CONFIG_PATH, "w", encoding="utf-8") as f:
            json.dump(current, f, indent=2, ensure_ascii=False)
        logger.info("Saved Anki config to %s", ANKI_CONFIG_PATH)
    except Exception as exc:
        logger.error("Failed writing %s: %s", ANKI_CONFIG_PATH, exc)


def _load_anki_credentials() -> Tuple[str, str]:
    u = os.environ.get("ANKIWEB_USERNAME", "").strip()
    p = os.environ.get("ANKIWEB_PASSWORD", "").strip()
    if u and p:
        return u, p
    if os.path.exists(ANKI_CREDENTIALS_PATH):
        try:
            with open(ANKI_CREDENTIALS_PATH, "r", encoding="utf-8") as f:
                data = json.load(f)
                return data.get("username", "").strip(), data.get("password", "").strip()
        except Exception:
            pass
    return "", ""


def save_anki_credentials(username: str, password: str) -> None:
    try:
        with open(ANKI_CREDENTIALS_PATH, "w", encoding="utf-8") as f:
            json.dump({"username": username, "password": password}, f, indent=2)
        logger.info("Saved AnkiWeb credentials.")
    except Exception as exc:
        logger.error("Failed saving credentials: %s", exc)


def validate_ankiconnect_url(url: str) -> Tuple[bool, str]:
    if not url:
        return False, "URL cannot be empty."
    p = urllib.parse.urlparse(url)
    if p.scheme not in ("http", "https"):
        return False, "URL scheme must be http or https."
    if not p.hostname:
        return False, "Invalid host in URL."
    return True, ""


def test_ankiconnect_connection(url: str = "http://127.0.0.1:8765") -> Tuple[bool, str]:
    valid, err_msg = validate_ankiconnect_url(url)
    if not valid:
        return False, err_msg
    try:
        payload = json.dumps({"action": "version", "version": 6}).encode("utf-8")
        req = urllib.request.Request(url, data=payload, headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=2.0) as resp:
            data = json.loads(resp.read().decode("utf-8"))
            if data.get("error"):
                return False, f"AnkiConnect error: {data['error']}"
            ver = data.get("result", "?")

        deck_count = 0
        try:
            p_deck = json.dumps({"action": "deckNames", "version": 6}).encode("utf-8")
            r_deck = urllib.request.Request(url, data=p_deck, headers={"Content-Type": "application/json"})
            with urllib.request.urlopen(r_deck, timeout=2.0) as resp:
                d_data = json.loads(resp.read().decode("utf-8"))
                decks = d_data.get("result", [])
                if isinstance(decks, list):
                    deck_count = len(decks)
        except Exception:
            pass

        return True, f"Connected to Anki Desktop! (API v{ver}, {deck_count} decks found)"
    except urllib.error.URLError:
        return False, "Could not reach Anki Desktop on 127.0.0.1:8765.\n\nMake sure:\n1. Anki Desktop is running.\n2. AnkiConnect add-on is installed (Code: 2055492159)."
    except Exception as exc:
        return False, f"Connection failed: {exc}"


def test_ankiweb_login(username: str, password: str) -> Tuple[bool, str]:
    if not username or not password:
        return False, "Username and password cannot be empty."
    import tempfile
    try:
        from anki.collection import Collection
        with tempfile.TemporaryDirectory() as td:
            col_path = os.path.join(td, "test_auth.anki2")
            col = Collection(col_path)
            try:
                auth = col.sync_login(username, password, None)
                col.close()
                if auth and getattr(auth, "hkey", None):
                    return True, f"AnkiWeb login successful for '{username}'!"
                return False, "Authentication succeeded but received empty sync key."
            except Exception as auth_exc:
                col.close()
                return False, f"AnkiWeb authentication failed: {auth_exc}"
    except ImportError:
        return False, "Python 'anki' package is not installed."
    except Exception as exc:
        return False, f"Error validating AnkiWeb login: {exc}"


def get_anki_summary() -> str:
    cfg = _load_anki_config()
    mode = cfg.get("anki_mode")
    if not mode:
        mode = "ankiconnect" if cfg.get("use_ankiconnect", True) else "disabled"
    if mode == "ankiconnect":
        return f"AnkiConnect ({cfg.get('ankiconnect_url', '127.0.0.1:8765')})"
    elif mode == "ankiweb":
        u, _ = _load_anki_credentials()
        return f"AnkiWeb Cloud ({u or 'Not configured'})"
    return "Disabled"


class AnkiSettingsDialog(tk.Toplevel):
    """Configuration dialog for AnkiConnect, AnkiWeb sync, deck prefixes, and player name."""

    def __init__(self, parent: tk.Tk, on_save_callback=None) -> None:
        super().__init__(parent)
        self.title("Anki Integration Settings")
        self.resizable(False, False)
        self.configure(bg=BG_ROOT)
        self.transient(parent)
        self.grab_set()
        apply_dark_titlebar(self)

        self.on_save_callback = on_save_callback
        self.cfg = _load_anki_config()
        self.orig_username, self.orig_password = _load_anki_credentials()

        self._build_dialog()
        self.update_idletasks()
        req_h = self.winfo_reqheight()
        center_window(self, 580, req_h, parent=parent)

    def _build_dialog(self) -> None:
        main_frame = ttk.Frame(self, padding=(16, 12))
        main_frame.pack(fill="both", expand=True)

        title_lbl = ttk.Label(main_frame, text="Anki Settings & Connection", style="DialogTitle.TLabel")
        title_lbl.pack(anchor="w")
        ttk.Label(
            main_frame,
            text="Configure how mined vocabulary cards and screenshots are exported",
            style="Muted.TLabel",
            font=(UI_FONT, 9),
        ).pack(anchor="w", pady=(0, 8))

        # Mode Selection
        mode_card, mode_frame = make_card(main_frame, title="Connection Mode", padding=(12, 8))
        mode_card.pack(fill="x", pady=(0, 8))

        current_mode = self.cfg.get("anki_mode")
        if not current_mode:
            current_mode = "ankiconnect" if self.cfg.get("use_ankiconnect", True) else "ankiweb"
        self.mode_var = tk.StringVar(value=current_mode)

        r_ac = ttk.Radiobutton(
            mode_frame,
            text="Anki Desktop via AnkiConnect (Recommended)",
            value="ankiconnect",
            variable=self.mode_var,
            command=self._on_mode_changed,
        )
        r_ac.pack(anchor="w", pady=(0, 1))
        ttk.Label(
            mode_frame,
            text="Fast, automatic local export directly into Anki Desktop. No login required.",
            style="SurfaceMuted.TLabel",
        ).pack(anchor="w", padx=(24, 0), pady=(0, 5))

        r_web = ttk.Radiobutton(
            mode_frame,
            text="Direct AnkiWeb Cloud Sync (Standalone)",
            value="ankiweb",
            variable=self.mode_var,
            command=self._on_mode_changed,
        )
        r_web.pack(anchor="w", pady=(0, 1))
        ttk.Label(
            mode_frame,
            text="Syncs directly to your AnkiWeb cloud account in background. Desktop Anki not needed.",
            style="SurfaceMuted.TLabel",
        ).pack(anchor="w", padx=(24, 0), pady=(0, 5))

        r_dis = ttk.Radiobutton(
            mode_frame,
            text="Disable Anki Integration",
            value="disabled",
            variable=self.mode_var,
            command=self._on_mode_changed,
        )
        r_dis.pack(anchor="w", pady=(0, 1))
        ttk.Label(
            mode_frame,
            text="Pressing [X] on the 3DS will do nothing.",
            style="SurfaceMuted.TLabel",
        ).pack(anchor="w", padx=(24, 0))

        # AnkiConnect Frame
        self.ac_card, self.ac_frame = make_card(main_frame, title="AnkiConnect Configuration", padding=(14, 10))
        self.ac_card.pack(fill="x", pady=(0, 10))

        ac_grid = ttk.Frame(self.ac_frame, style="Surface.TFrame")
        ac_grid.pack(fill="x")
        ac_grid.columnconfigure(0, minsize=132)
        ac_grid.columnconfigure(1, weight=1)

        ttk.Label(ac_grid, text="Endpoint URL:", style="SurfaceBold.TLabel").grid(
            row=0, column=0, sticky="w", padx=(0, 10), pady=2
        )
        self.ac_url_var = tk.StringVar(value=self.cfg.get("ankiconnect_url", "http://127.0.0.1:8765"))
        self.ac_url_entry = ttk.Entry(ac_grid, textvariable=self.ac_url_var)
        self.ac_url_entry.grid(row=0, column=1, sticky="ew", padx=(0, 10), pady=2)

        self.ac_test_btn = make_secondary_button(
            ac_grid, text="Test Connection", command=self._test_ankiconnect, padx=12, pady=4
        )
        self.ac_test_btn.grid(row=0, column=2, sticky="e", pady=2)

        self.ac_status_lbl = ttk.Label(self.ac_frame, text="", style="SurfaceMuted.TLabel")
        self.ac_status_lbl.pack(anchor="w", pady=(4, 0))

        # AnkiWeb Frame
        self.web_card, self.web_frame = make_card(main_frame, title="AnkiWeb Cloud Credentials", padding=(14, 10))
        self.web_card.pack(fill="x", pady=(0, 10))

        web_grid = ttk.Frame(self.web_frame, style="Surface.TFrame")
        web_grid.pack(fill="x")
        web_grid.columnconfigure(0, minsize=132)
        web_grid.columnconfigure(1, weight=1)

        ttk.Label(web_grid, text="Email / Username:", style="SurfaceBold.TLabel").grid(
            row=0, column=0, sticky="w", padx=(0, 10), pady=3
        )
        self.web_user_var = tk.StringVar(value=self.orig_username)
        self.web_user_entry = ttk.Entry(web_grid, textvariable=self.web_user_var)
        self.web_user_entry.grid(row=0, column=1, columnspan=2, sticky="ew", pady=3)

        ttk.Label(web_grid, text="Password:", style="SurfaceBold.TLabel").grid(
            row=1, column=0, sticky="w", padx=(0, 10), pady=3
        )
        self.web_pass_var = tk.StringVar(value=self.orig_password)
        self.web_pass_entry = ttk.Entry(web_grid, textvariable=self.web_pass_var, show="•")
        self.web_pass_entry.grid(row=1, column=1, sticky="ew", padx=(0, 10), pady=3)

        self.web_test_btn = make_secondary_button(
            web_grid, text="Validate Login", command=self._test_ankiweb, padx=12, pady=4
        )
        self.web_test_btn.grid(row=1, column=2, sticky="e", pady=3)

        self.web_status_lbl = ttk.Label(self.web_frame, text="", style="SurfaceMuted.TLabel")
        self.web_status_lbl.pack(anchor="w", pady=(4, 0))

        # Deck & Note Preferences Frame
        self.pref_card, pref_frame = make_card(main_frame, title="Deck & Card Preferences", padding=(14, 10))
        self.pref_card.pack(fill="x", pady=(0, 10))

        pref_grid = ttk.Frame(pref_frame, style="Surface.TFrame")
        pref_grid.pack(fill="x")
        pref_grid.columnconfigure(0, minsize=132)
        pref_grid.columnconfigure(1, weight=1)

        ttk.Label(pref_grid, text="Deck Prefix:", style="SurfaceBold.TLabel").grid(
            row=0, column=0, sticky="w", padx=(0, 10), pady=3
        )
        self.deck_prefix_var = tk.StringVar(value=self.cfg.get("deck_prefix", "3DS::"))
        ttk.Entry(pref_grid, textvariable=self.deck_prefix_var).grid(
            row=0, column=1, sticky="ew", pady=3
        )

        ttk.Label(pref_grid, text="Screenshot Mode:", style="SurfaceBold.TLabel").grid(
            row=1, column=0, sticky="w", padx=(0, 10), pady=3
        )
        self.ss_mode_var = tk.StringVar(value=self.cfg.get("screenshot_mode", "clicked"))
        ttk.Combobox(
            pref_grid,
            textvariable=self.ss_mode_var,
            values=["clicked", "both", "top", "bottom", "none"],
            state="readonly",
        ).grid(row=1, column=1, sticky="ew", pady=3)

        ttk.Label(pref_grid, text="Player Name (JP):", style="SurfaceBold.TLabel").grid(
            row=2, column=0, sticky="w", padx=(0, 10), pady=3
        )
        name_pair = ttk.Frame(pref_grid, style="Surface.TFrame")
        name_pair.grid(row=2, column=1, sticky="ew", pady=3)
        name_pair.columnconfigure(0, weight=1)
        name_pair.columnconfigure(2, weight=1)

        self.player_jp_var = tk.StringVar(value=self.cfg.get("player_name", ""))
        ttk.Entry(name_pair, textvariable=self.player_jp_var).grid(row=0, column=0, sticky="ew")
        ttk.Label(name_pair, text="English:", style="SurfaceBold.TLabel").grid(
            row=0, column=1, padx=(10, 6)
        )
        self.player_en_var = tk.StringVar(value=self.cfg.get("player_english_name", ""))
        ttk.Entry(name_pair, textvariable=self.player_en_var).grid(row=0, column=2, sticky="ew")

        self.sub_hira_var = tk.BooleanVar(value=bool(self.cfg.get("substitute_hiragana_terms", True)))
        ttk.Checkbutton(
            pref_frame,
            text="Substitute custom game vocabulary & Pokémon names into sentences",
            variable=self.sub_hira_var,
        ).pack(anchor="w", pady=(8, 0))

        # Bottom Buttons
        btn_box = ttk.Frame(main_frame)
        btn_box.pack(fill="x", pady=(10, 0))

        cancel_btn = make_secondary_button(btn_box, text="Cancel", command=self.destroy, padx=16, pady=6)
        cancel_btn.pack(side="right", padx=(8, 0))

        save_btn = make_action_button(
            btn_box,
            text="Save Settings",
            command=self._save_and_close,
            variant="success",
            padx=18,
            pady=6,
        )
        save_btn.pack(side="right")

        self._on_mode_changed()

    def _on_mode_changed(self) -> None:
        mode = self.mode_var.get()
        self.ac_card.pack_forget()
        self.web_card.pack_forget()

        if mode == "ankiconnect":
            self.ac_card.pack(fill="x", pady=(0, 10), before=self.pref_card)
        elif mode == "ankiweb":
            self.web_card.pack(fill="x", pady=(0, 10), before=self.pref_card)

        self.update_idletasks()
        req_h = self.winfo_reqheight()
        if req_h > 100:
            self.geometry(f"580x{req_h}")

    def _test_ankiconnect(self) -> None:
        url = self.ac_url_var.get().strip() or "http://127.0.0.1:8765"
        self.ac_status_lbl.config(text="Testing AnkiConnect...", foreground=CYAN)

        def worker():
            ok, msg = test_ankiconnect_connection(url)

            def update():
                if ok:
                    self.ac_status_lbl.config(text=f"✔ {msg}", foreground=SUCCESS)
                else:
                    self.ac_status_lbl.config(text=f"✖ {msg}", foreground=DANGER)

            self.after(0, update)

        threading.Thread(target=worker, daemon=True).start()

    def _test_ankiweb(self) -> None:
        u = self.web_user_var.get().strip()
        p = self.web_pass_var.get().strip()
        self.web_status_lbl.config(text="Validating credentials...", foreground=CYAN)

        def worker():
            ok, msg = test_ankiweb_login(u, p)

            def update():
                if ok:
                    self.web_status_lbl.config(text=f"✔ {msg}", foreground=SUCCESS)
                else:
                    self.web_status_lbl.config(text=f"✖ {msg}", foreground=DANGER)

            self.after(0, update)

        threading.Thread(target=worker, daemon=True).start()

    def _save_and_close(self) -> None:
        mode = self.mode_var.get()
        cfg_updates = {
            "anki_mode": mode,
            "use_ankiconnect": (mode == "ankiconnect"),
            "ankiconnect_url": self.ac_url_var.get().strip() or "http://127.0.0.1:8765",
            "deck_prefix": self.deck_prefix_var.get().strip(),
            "screenshot_mode": self.ss_mode_var.get(),
            "player_name": self.player_jp_var.get().strip(),
            "player_english_name": self.player_en_var.get().strip(),
            "substitute_hiragana_terms": self.sub_hira_var.get(),
        }
        save_anki_config(cfg_updates)

        if mode == "ankiweb":
            u = self.web_user_var.get().strip()
            p = self.web_pass_var.get().strip()
            save_anki_credentials(u, p)

        if self.on_save_callback:
            self.on_save_callback()

        messagebox.showinfo("Saved", "Anki settings updated successfully!")
        self.destroy()

    def destroy(self) -> None:
        try:
            self.web_pass_var.set("")
            self.orig_password = ""
        except Exception:
            pass
        super().destroy()
