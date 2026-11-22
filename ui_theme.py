#!/usr/bin/env python3
"""
ui_theme.py - Cohesive Dark Theme Palette, Custom Controls & Styling
"""

import sys
import tkinter as tk
from tkinter import ttk
from typing import Callable, Dict, Optional, Tuple

from PIL import Image, ImageDraw, ImageTk

UI_FONT = "Segoe UI" if sys.platform == "win32" else "Helvetica"
MONO_FONT = "Consolas" if sys.platform == "win32" else "Courier"

# Surface & Text Palette
BG_ROOT = "#0D0F14"        # App window & header background
BG_SURFACE = "#161A23"     # Cards & dialog panels
BG_ELEVATED = "#262E42"    # Secondary buttons & active tab surface
BG_ELEVATED_HOVER = "#313B54"
BG_INPUT = "#0F1219"       # Recessed entry & combobox fields
BG_CANVAS = "#090B10"      # Terminal log & screenshot canvas

BORDER = "#2B3347"         # Card 1px border
BORDER_INPUT = "#3B4763"   # Input field border
BORDER_BTN = "#4A5878"     # Clear button border for contrast
BORDER_BTN_HOVER = "#6B7FA8"
BORDER_FOCUS = "#3B82F6"   # Focus highlight

FG_PRIMARY = "#F1F5F9"     # Crisp primary text
FG_SECONDARY = "#94A3B8"   # Subtitles, helper text, metadata
FG_MUTED = "#64748B"       # Disabled text, timestamps

# Semantic & Accent Colors
ACCENT = "#2563EB"
ACCENT_HOVER = "#1D4ED8"
ACCENT_BORDER = "#60A5FA"

CYAN = "#38BDF8"           # OCR headword & section titles
SUCCESS = "#22C55E"        # Running indicator
SUCCESS_BTN = "#16A34A"
SUCCESS_HOVER = "#15803D"
SUCCESS_BORDER = "#4ADE80"
SUCCESS_BG = "#11281B"

WARNING = "#F59E0B"
WARNING_BG = "#2B1F0D"

DANGER = "#EF4444"
DANGER_BTN = "#DC2626"
DANGER_HOVER = "#B91C1C"
DANGER_BORDER = "#F87171"
DANGER_BG = "#2C1216"

BUTTON_VARIANTS: Dict[str, Dict[str, str]] = {
    "secondary": {
        "bg": BG_ELEVATED,
        "hover_bg": BG_ELEVATED_HOVER,
        "border": BORDER_BTN,
        "hover_border": BORDER_BTN_HOVER,
        "fg": FG_PRIMARY,
        "disabled_bg": "#181C26",
        "disabled_border": "#262D3E",
        "disabled_fg": "#546078",
    },
    "primary": {
        "bg": ACCENT,
        "hover_bg": ACCENT_HOVER,
        "border": ACCENT_BORDER,
        "hover_border": "#93C5FD",
        "fg": "#FFFFFF",
        "disabled_bg": "#1B263B",
        "disabled_border": "#2B3A55",
        "disabled_fg": "#64748B",
    },
    "success": {
        "bg": SUCCESS_BTN,
        "hover_bg": SUCCESS_HOVER,
        "border": SUCCESS_BORDER,
        "hover_border": "#86EFAC",
        "fg": "#FFFFFF",
        "disabled_bg": "#192920",
        "disabled_border": "#253D2F",
        "disabled_fg": "#64748B",
    },
    "danger": {
        "bg": DANGER_BTN,
        "hover_bg": DANGER_HOVER,
        "border": DANGER_BORDER,
        "hover_border": "#FCA5A5",
        "fg": "#FFFFFF",
        "disabled_bg": "#2B181B",
        "disabled_border": "#3F2327",
        "disabled_fg": "#64748B",
    },
}


def create_app_icon() -> ImageTk.PhotoImage:
    """Generate a crisp 32x32 dual-screen 3DS OCR icon in memory."""
    img = Image.new("RGBA", (32, 32), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    # Rounded dark-blue badge background
    draw.rounded_rectangle([1, 1, 30, 30], radius=7, fill=(22, 26, 35, 255), outline=(59, 130, 246, 255), width=2)
    # Top 3DS screen (cyan-blue)
    draw.rounded_rectangle([6, 6, 25, 15], radius=2, fill=(37, 99, 235, 255), outline=(56, 189, 248, 255), width=1)
    # Bottom 3DS screen (smaller centered)
    draw.rounded_rectangle([9, 18, 22, 25], radius=2, fill=(30, 41, 59, 255), outline=(148, 163, 184, 255), width=1)
    return ImageTk.PhotoImage(img)


def apply_dark_titlebar(window: tk.Misc) -> None:
    """Enable Windows 10/11 immersive dark mode title bar if supported."""
    if sys.platform != "win32":
        return
    try:
        import ctypes

        window.update_idletasks()
        hwnd = ctypes.windll.user32.GetParent(window.winfo_id())
        if not hwnd:
            hwnd = window.winfo_id()
        value = ctypes.c_int(1)
        res = ctypes.windll.dwmapi.DwmSetWindowAttribute(
            hwnd, 20, ctypes.byref(value), ctypes.sizeof(value)
        )
        if res != 0:
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                hwnd, 19, ctypes.byref(value), ctypes.sizeof(value)
            )
    except Exception:
        pass


def apply_dark_theme(root: tk.Misc) -> ttk.Style:
    """Configure ttk 'clam' theme and Tk option database for full dark mode."""
    style = ttk.Style(root)
    try:
        style.theme_use("clam")
    except Exception:
        pass

    root.configure(bg=BG_ROOT)

    # Style combobox dropdown listboxes globally
    root.option_add("*TCombobox*Listbox.background", BG_INPUT)
    root.option_add("*TCombobox*Listbox.foreground", FG_PRIMARY)
    root.option_add("*TCombobox*Listbox.selectBackground", ACCENT)
    root.option_add("*TCombobox*Listbox.selectForeground", "#FFFFFF")
    root.option_add("*TCombobox*Listbox.font", (UI_FONT, 9))

    style.configure(
        ".",
        background=BG_ROOT,
        foreground=FG_PRIMARY,
        bordercolor=BORDER,
        darkcolor=BG_SURFACE,
        lightcolor=BG_SURFACE,
        troughcolor=BG_CANVAS,
        selectbackground=ACCENT,
        selectforeground="#FFFFFF",
        font=(UI_FONT, 9),
    )

    # Frames
    style.configure("TFrame", background=BG_ROOT)
    style.configure("Surface.TFrame", background=BG_SURFACE)
    style.configure("Elevated.TFrame", background=BG_ELEVATED)

    # Labels
    style.configure("TLabel", background=BG_ROOT, foreground=FG_PRIMARY, font=(UI_FONT, 9))
    style.configure("Muted.TLabel", background=BG_ROOT, foreground=FG_SECONDARY, font=(UI_FONT, 9))
    style.configure("Title.TLabel", background=BG_ROOT, foreground=FG_PRIMARY, font=(UI_FONT, 15, "bold"))

    style.configure("Surface.TLabel", background=BG_SURFACE, foreground=FG_PRIMARY, font=(UI_FONT, 9))
    style.configure("SurfaceBold.TLabel", background=BG_SURFACE, foreground=FG_PRIMARY, font=(UI_FONT, 9, "bold"))
    style.configure("SurfaceMuted.TLabel", background=BG_SURFACE, foreground=FG_SECONDARY, font=(UI_FONT, 8))
    style.configure("CardHeader.TLabel", background=BG_SURFACE, foreground=CYAN, font=(UI_FONT, 10, "bold"))
    style.configure("DialogTitle.TLabel", background=BG_ROOT, foreground=FG_PRIMARY, font=(UI_FONT, 13, "bold"))
    style.configure("Accent.TLabel", background=BG_SURFACE, foreground=CYAN, font=(UI_FONT, 10, "bold"))

    # Fallback ttk.Button styling (for any standard ttk buttons)
    style.configure(
        "TButton",
        background=BG_ELEVATED,
        foreground=FG_PRIMARY,
        bordercolor=BORDER_BTN,
        lightcolor=BORDER_BTN,
        darkcolor=BORDER_BTN,
        focuscolor=BORDER_FOCUS,
        padding=[12, 6],
        borderwidth=1,
        relief="solid",
        font=(UI_FONT, 9, "bold"),
    )
    style.map(
        "TButton",
        background=[("disabled", "#181C26"), ("pressed", BG_ELEVATED_HOVER), ("active", BG_ELEVATED_HOVER)],
        foreground=[("disabled", "#546078")],
        bordercolor=[("disabled", "#262D3E"), ("focus", BORDER_FOCUS), ("active", BORDER_BTN_HOVER)],
    )

    # Recessed Entries
    style.configure(
        "TEntry",
        fieldbackground=BG_INPUT,
        foreground=FG_PRIMARY,
        insertcolor=FG_PRIMARY,
        bordercolor=BORDER_INPUT,
        lightcolor=BG_INPUT,
        darkcolor=BG_INPUT,
        padding=[8, 5],
        borderwidth=1,
    )
    style.map(
        "TEntry",
        fieldbackground=[("readonly", BG_CANVAS), ("disabled", "#12151D")],
        foreground=[("disabled", FG_MUTED), ("readonly", FG_SECONDARY)],
        bordercolor=[("disabled", BORDER), ("focus", BORDER_FOCUS)],
    )

    # Recessed Combobox
    style.configure(
        "TCombobox",
        fieldbackground=BG_INPUT,
        background=BG_ELEVATED,
        foreground=FG_PRIMARY,
        arrowcolor=FG_PRIMARY,
        bordercolor=BORDER_INPUT,
        lightcolor=BG_INPUT,
        darkcolor=BG_INPUT,
        padding=[8, 5],
        borderwidth=1,
    )
    style.map(
        "TCombobox",
        fieldbackground=[("readonly", BG_INPUT), ("disabled", "#12151D")],
        foreground=[("readonly", FG_PRIMARY), ("disabled", FG_MUTED)],
        background=[("active", BG_ELEVATED_HOVER), ("disabled", "#181C26")],
        arrowcolor=[("disabled", FG_MUTED), ("active", "#FFFFFF")],
        bordercolor=[("focus", BORDER_FOCUS), ("active", BORDER_BTN_HOVER)],
    )

    # Checkbuttons & Radiobuttons (inside Surface cards)
    style.configure(
        "TCheckbutton",
        background=BG_SURFACE,
        foreground=FG_PRIMARY,
        indicatorbackground=BG_INPUT,
        indicatorcolor=BG_INPUT,
        bordercolor="#5B6B8F",
        indicatormargin=[0, 0, 8, 0],
        focuscolor=BG_SURFACE,
        font=(UI_FONT, 9),
    )
    style.map(
        "TCheckbutton",
        background=[("active", BG_SURFACE)],
        indicatorbackground=[("selected", ACCENT), ("active", BG_ELEVATED)],
        indicatorcolor=[("selected", ACCENT)],
        foreground=[("disabled", FG_MUTED)],
    )

    style.configure(
        "TRadiobutton",
        background=BG_SURFACE,
        foreground=FG_PRIMARY,
        indicatorbackground=BG_INPUT,
        indicatorcolor=BG_INPUT,
        bordercolor="#5B6B8F",
        indicatormargin=[0, 0, 8, 0],
        focuscolor=BG_SURFACE,
        font=(UI_FONT, 9, "bold"),
    )
    style.map(
        "TRadiobutton",
        background=[("active", BG_SURFACE)],
        indicatorbackground=[("selected", ACCENT), ("active", BG_ELEVATED)],
        indicatorcolor=[("selected", ACCENT)],
        foreground=[("disabled", FG_MUTED)],
    )

    # Scrollbars
    style.configure(
        "Vertical.TScrollbar",
        background=BG_ELEVATED,
        troughcolor=BG_CANVAS,
        bordercolor=BG_CANVAS,
        arrowcolor=FG_SECONDARY,
        lightcolor=BG_ELEVATED,
        darkcolor=BG_ELEVATED,
        gripcount=0,
        borderwidth=0,
    )
    style.map(
        "Vertical.TScrollbar",
        background=[("active", BORDER_BTN), ("pressed", BORDER_BTN_HOVER)],
    )

    style.configure(
        "Horizontal.TScrollbar",
        background=BG_ELEVATED,
        troughcolor=BG_CANVAS,
        bordercolor=BG_CANVAS,
        arrowcolor=FG_SECONDARY,
        lightcolor=BG_ELEVATED,
        darkcolor=BG_ELEVATED,
        gripcount=0,
        borderwidth=0,
    )
    style.map(
        "Horizontal.TScrollbar",
        background=[("active", BORDER_BTN), ("pressed", BORDER_BTN_HOVER)],
    )

    # Dark Treeview (Paired 3DSes list, etc.)
    style.configure(
        "Treeview",
        background=BG_CANVAS,
        fieldbackground=BG_CANVAS,
        foreground=FG_PRIMARY,
        bordercolor=BORDER,
        borderwidth=0,
        rowheight=26,
        font=(UI_FONT, 9),
    )
    style.map(
        "Treeview",
        background=[("selected", ACCENT)],
        foreground=[("selected", "#FFFFFF")],
    )
    style.configure(
        "Treeview.Heading",
        background=BG_ELEVATED,
        foreground=FG_PRIMARY,
        bordercolor=BORDER,
        relief="flat",
        padding=[8, 6],
        font=(UI_FONT, 9, "bold"),
    )
    style.map(
        "Treeview.Heading",
        background=[("active", BG_ELEVATED_HOVER)],
        foreground=[("active", "#FFFFFF")],
    )

    return style


def make_card(parent: tk.Widget, title: str = "", padding: Tuple[int, int] = (14, 12)) -> Tuple[tk.Frame, ttk.Frame]:
    """
    Create a sleek 1px-bordered dark card container with an optional header label.
    Returns (outer_border_frame, inner_content_frame).
    """
    outer = tk.Frame(parent, bg=BORDER, bd=0, highlightthickness=0)
    inner = ttk.Frame(outer, style="Surface.TFrame", padding=padding)
    inner.pack(fill="both", expand=True, padx=1, pady=1)
    if title:
        hdr = ttk.Label(inner, text=title, style="CardHeader.TLabel")
        hdr.pack(anchor="w", pady=(0, 6))
    content = ttk.Frame(inner, style="Surface.TFrame")
    content.pack(fill="both", expand=True)
    return outer, content


class ThemedButton(tk.Frame):
    """High-contrast flat button with crisp 1px border, hover state, and clean disabled look."""

    def __init__(
        self,
        parent: tk.Widget,
        text: str,
        command: Optional[Callable[[], None]] = None,
        variant: str = "secondary",
        width: Optional[int] = None,
        padx: int = 12,
        pady: int = 5,
        font_size: int = 9,
    ) -> None:
        self._variant = variant
        pal = BUTTON_VARIANTS.get(variant, BUTTON_VARIANTS["secondary"])
        super().__init__(parent, bg=pal["border"], bd=0, highlightthickness=0)

        btn_kwargs: Dict[str, object] = {
            "text": text,
            "command": command,
            "font": (UI_FONT, font_size, "bold"),
            "bg": pal["bg"],
            "fg": pal["fg"],
            "activebackground": pal["hover_bg"],
            "activeforeground": pal["fg"],
            "disabledforeground": pal["disabled_fg"],
            "relief": "flat",
            "bd": 0,
            "highlightthickness": 0,
            "cursor": "hand2",
            "padx": padx,
            "pady": pady,
        }
        if width is not None:
            btn_kwargs["width"] = width

        self._btn = tk.Button(self, **btn_kwargs)
        self._btn.pack(fill="both", expand=True, padx=1, pady=1)

        self._btn.bind("<Enter>", self._on_enter)
        self._btn.bind("<Leave>", self._on_leave)

    def _apply_palette(self, hovering: bool = False) -> None:
        pal = BUTTON_VARIANTS.get(self._variant, BUTTON_VARIANTS["secondary"])
        if str(self._btn["state"]) == "disabled":
            super().configure(bg=pal["disabled_border"])
            self._btn.configure(bg=pal["disabled_bg"], cursor="arrow")
        elif hovering:
            super().configure(bg=pal["hover_border"])
            self._btn.configure(bg=pal["hover_bg"], cursor="hand2")
        else:
            super().configure(bg=pal["border"])
            self._btn.configure(bg=pal["bg"], fg=pal["fg"], activebackground=pal["hover_bg"], cursor="hand2")

    def _on_enter(self, _e: tk.Event) -> None:
        self._apply_palette(hovering=True)

    def _on_leave(self, _e: tk.Event) -> None:
        self._apply_palette(hovering=False)

    def set_variant(self, variant: str) -> None:
        self._variant = variant
        self._apply_palette(hovering=False)

    def configure(self, cnf=None, **kwargs):
        if cnf:
            kwargs.update(cnf)
        variant = kwargs.pop("variant", None)
        if variant is not None:
            self._variant = variant
        btn_keys = ("text", "command", "state", "width", "padx", "pady")
        btn_updates = {k: kwargs.pop(k) for k in list(kwargs.keys()) if k in btn_keys}
        if btn_updates:
            self._btn.configure(**btn_updates)
        if kwargs:
            super().configure(**kwargs)
        self._apply_palette(hovering=False)

    config = configure

    def cget(self, key: str):
        if key in ("text", "command", "state", "width", "padx", "pady"):
            return self._btn.cget(key)
        return super().cget(key)

    __getitem__ = cget


def make_secondary_button(
    parent: tk.Widget,
    text: str,
    command: Optional[Callable[[], None]] = None,
    width: Optional[int] = None,
    padx: int = 12,
    pady: int = 5,
    font_size: int = 9,
) -> ThemedButton:
    """Create a high-contrast elevated secondary button with crisp 1px border."""
    return ThemedButton(
        parent,
        text=text,
        command=command,
        variant="secondary",
        width=width,
        padx=padx,
        pady=pady,
        font_size=font_size,
    )


def make_action_button(
    parent: tk.Widget,
    text: str,
    command: Optional[Callable[[], None]] = None,
    variant: str = "primary",
    width: Optional[int] = None,
    padx: int = 14,
    pady: int = 6,
    font_size: int = 9,
) -> ThemedButton:
    """Create a high-contrast semantic action button ('primary', 'success', 'danger')."""
    return ThemedButton(
        parent,
        text=text,
        command=command,
        variant=variant,
        width=width,
        padx=padx,
        pady=pady,
        font_size=font_size,
    )


class ModernTabBar(tk.Frame):
    """
    Sleek dark-mode underline tab switcher replacing classic Windows 98 ttk.Notebook tabs.
    """

    def __init__(self, parent: tk.Widget, content_parent: tk.Widget) -> None:
        super().__init__(parent, bg=BG_ROOT, bd=0, highlightthickness=0)
        self.content_parent = content_parent
        self._tabs: Dict[str, Dict[str, object]] = {}
        self._order: list[str] = []
        self._active_key: Optional[str] = None

        self._row = tk.Frame(self, bg=BG_ROOT, bd=0, highlightthickness=0)
        self._row.pack(fill="x")
        tk.Frame(self, bg=BORDER, height=1).pack(fill="x")

    def add_tab(self, key: str, title: str, frame: tk.Widget) -> None:
        holder = tk.Frame(self._row, bg=BG_ROOT, bd=0, highlightthickness=0)
        holder.pack(side="left", padx=(0, 6))

        lbl = tk.Label(
            holder,
            text=title,
            font=(UI_FONT, 10, "bold"),
            bg=BG_ROOT,
            fg=FG_SECONDARY,
            padx=18,
            pady=9,
            cursor="hand2",
        )
        lbl.pack(fill="x")

        indicator = tk.Frame(holder, bg=BG_ROOT, height=2, bd=0, highlightthickness=0)
        indicator.pack(fill="x")

        lbl.bind("<Button-1>", lambda _e, k=key: self.select(k))
        lbl.bind("<Enter>", lambda _e, k=key: self._on_hover(k, True))
        lbl.bind("<Leave>", lambda _e, k=key: self._on_hover(k, False))

        self._tabs[key] = {
            "title": title,
            "frame": frame,
            "holder": holder,
            "label": lbl,
            "indicator": indicator,
        }
        self._order.append(key)

        if self._active_key is None:
            self.select(key)

    def _on_hover(self, key: str, entering: bool) -> None:
        if key == self._active_key:
            return
        tab = self._tabs[key]
        lbl: tk.Label = tab["label"]  # type: ignore[assignment]
        holder: tk.Frame = tab["holder"]  # type: ignore[assignment]
        if entering:
            lbl.configure(bg="#141821", fg=FG_PRIMARY)
            holder.configure(bg="#141821")
        else:
            lbl.configure(bg=BG_ROOT, fg=FG_SECONDARY)
            holder.configure(bg=BG_ROOT)

    def select(self, key: str) -> None:
        if key not in self._tabs:
            return
        if self._active_key and self._active_key in self._tabs:
            prev = self._tabs[self._active_key]
            prev["frame"].pack_forget()  # type: ignore[attr-defined]
            prev["label"].configure(bg=BG_ROOT, fg=FG_SECONDARY)  # type: ignore[attr-defined]
            prev["holder"].configure(bg=BG_ROOT)  # type: ignore[attr-defined]
            prev["indicator"].configure(bg=BG_ROOT)  # type: ignore[attr-defined]

        cur = self._tabs[key]
        cur["label"].configure(bg=BG_SURFACE, fg=FG_PRIMARY)  # type: ignore[attr-defined]
        cur["holder"].configure(bg=BG_SURFACE)  # type: ignore[attr-defined]
        cur["indicator"].configure(bg=BORDER_FOCUS)  # type: ignore[attr-defined]
        cur["frame"].pack(in_=self.content_parent, fill="both", expand=True)  # type: ignore[attr-defined]
        self._active_key = key

    def tab_keys(self) -> list[str]:
        return list(self._order)

    def active_key(self) -> Optional[str]:
        return self._active_key
