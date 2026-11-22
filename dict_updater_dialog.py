"""
dict_updater_dialog.py - First-run setup and Dictionaries & OCR Models Updater dialog.
"""

from __future__ import annotations

import os
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Callable, Dict, List, Optional, Sequence

import dict_updater
from ui_theme import (
    BG_ELEVATED,
    BG_ROOT,
    BG_SURFACE,
    BORDER,
    FG_MUTED,
    FG_PRIMARY,
    FG_SECONDARY,
    MONO_FONT,
    ModernTabBar,
    SUCCESS,
    UI_FONT,
    WARNING,
    apply_dark_titlebar,
    make_action_button,
    make_secondary_button,
)


def _center_dialog(win: tk.Toplevel, width: int, height: int) -> None:
    win.update_idletasks()
    sw = win.winfo_screenwidth()
    sh = win.winfo_screenheight()
    x = max(0, (sw - width) // 2)
    y = max(0, (sh - height) // 2)
    win.geometry(f"{width}x{height}+{x}+{y}")


def _fmt_size(num_bytes: int) -> str:
    if num_bytes <= 0:
        return "0 B"
    if num_bytes >= 1024 * 1024:
        return f"{num_bytes / (1024 * 1024):.1f} MB"
    if num_bytes >= 1024:
        return f"{num_bytes / 1024:.0f} KB"
    return f"{num_bytes} B"


class DictionaryUpdaterDialog:
    """Modal dialog for first-run setup and on-demand updates of open-licensed dictionaries & OCR models."""

    def __init__(
        self,
        parent: tk.Tk,
        app_dir: str,
        first_run: bool = False,
        on_before_write: Optional[Callable[[], None]] = None,
        on_complete: Optional[Callable[[bool], None]] = None,
    ) -> None:
        self.parent = parent
        self.app_dir = app_dir
        self.first_run = first_run
        self.on_before_write = on_before_write
        self.on_complete = on_complete

        self._cancel_event = threading.Event()
        self._working = False
        self._did_update = False
        self._closed = False

        self.win = tk.Toplevel(parent)
        self.win.title(
            "First-Run Setup: Dictionaries & OCR Models"
            if first_run
            else "Dictionaries & OCR Models Updater"
        )
        self.win.configure(bg=BG_ROOT)
        self.win.resizable(False, False)
        self.win.transient(parent)
        self.win.grab_set()
        apply_dark_titlebar(self.win)
        self.win.protocol("WM_DELETE_WINDOW", self._on_close)

        self._check_vars: Dict[str, tk.BooleanVar] = {}
        self._status_labels: Dict[str, tk.Label] = {}
        self._version_labels: Dict[str, tk.Label] = {}

        self._build_ui()
        self._refresh_statuses()
        _center_dialog(self.win, 680, 585)

    def _build_asset_list(
        self,
        parent: tk.Widget,
        assets: Sequence[dict_updater.DictAssetSpec],
        missing_first_run: set,
    ) -> None:
        list_frame = tk.Frame(parent, bg=BORDER, bd=0)
        list_frame.pack(fill="x", pady=(8, 10))
        inner = tk.Frame(list_frame, bg=BG_SURFACE, padx=12, pady=10)
        inner.pack(fill="both", expand=True, padx=1, pady=1)

        for idx, spec in enumerate(assets):
            if idx > 0:
                tk.Frame(inner, bg=BORDER, height=1).pack(fill="x", pady=8)

            row = tk.Frame(inner, bg=BG_SURFACE)
            row.pack(fill="x")

            installed = dict_updater.get_asset_status(self.app_dir, spec.asset_id)["installed"]
            if self.first_run:
                default_checked = spec.asset_id in missing_first_run
            else:
                default_checked = (not installed) and spec.required_for_first_run
                if not default_checked and spec.asset_id == "jamdict" and not any(
                    not dict_updater.get_asset_status(self.app_dir, a.asset_id)["installed"]
                    for a in dict_updater.ALL_ASSETS
                    if a.required_for_first_run
                ):
                    default_checked = True

            var = tk.BooleanVar(value=default_checked)
            self._check_vars[spec.asset_id] = var

            top_row = tk.Frame(row, bg=BG_SURFACE)
            top_row.pack(fill="x")

            chk = ttk.Checkbutton(top_row, text=spec.title, variable=var)
            chk.pack(side="left")

            status_lbl = tk.Label(
                top_row,
                text="",
                bg=BG_SURFACE,
                fg=FG_MUTED,
                font=(UI_FONT, 9, "bold"),
            )
            status_lbl.pack(side="right")
            self._status_labels[spec.asset_id] = status_lbl

            meta_row = tk.Frame(row, bg=BG_SURFACE)
            meta_row.pack(fill="x", padx=(22, 0), pady=(2, 0))

            tk.Label(
                meta_row,
                text=spec.description,
                bg=BG_SURFACE,
                fg=FG_SECONDARY,
                font=(UI_FONT, 8),
                wraplength=600,
                justify="left",
            ).pack(anchor="w")

            sub_row = tk.Frame(row, bg=BG_SURFACE)
            sub_row.pack(fill="x", padx=(22, 0), pady=(2, 0))

            tk.Label(
                sub_row,
                text=f"License: {spec.license_label}  •  Source: {spec.source_label}  •  {spec.approx_size}",
                bg=BG_SURFACE,
                fg=FG_MUTED,
                font=(UI_FONT, 8),
            ).pack(side="left")

            ver_lbl = tk.Label(
                sub_row,
                text="",
                bg=BG_SURFACE,
                fg=FG_MUTED,
                font=(MONO_FONT, 8),
            )
            ver_lbl.pack(side="right")
            self._version_labels[spec.asset_id] = ver_lbl

    def _build_ui(self) -> None:
        outer = tk.Frame(self.win, bg=BG_ROOT, padx=18, pady=14)
        outer.pack(fill="both", expand=True)

        heading = (
            "First-Run Setup: Dictionaries & OCR Models"
            if self.first_run
            else "Dictionaries & OCR Models Updater"
        )
        subtitle = (
            "Large open-licensed dictionaries and local OCR models are not bundled in Git. "
            "Download the recommended assets below (checked across both tabs) to get started."
            if self.first_run
            else "Download or refresh openly licensed dictionaries and pinned, SHA-256-verified "
            "local OCR models from their official upstream sources."
        )

        tk.Label(
            outer, text=heading, bg=BG_ROOT, fg=FG_PRIMARY, font=(UI_FONT, 13, "bold")
        ).pack(anchor="w")
        tk.Label(
            outer,
            text=subtitle,
            bg=BG_ROOT,
            fg=FG_SECONDARY,
            font=(UI_FONT, 9),
            wraplength=640,
            justify="left",
        ).pack(anchor="w", pady=(4, 10))

        # Modern tab bar: Dictionaries | OCR Models
        tab_bar_wrap = tk.Frame(outer, bg=BG_ROOT)
        tab_bar_wrap.pack(fill="x")
        tab_content = tk.Frame(outer, bg=BG_ROOT)
        tab_content.pack(fill="both", expand=True)

        self.tab_bar = ModernTabBar(tab_bar_wrap, tab_content)
        self.tab_bar.pack(fill="x")

        missing_first_run = set(dict_updater.get_missing_first_run_assets(self.app_dir))

        # Tab 1: Dictionaries
        tab_dicts = tk.Frame(tab_content, bg=BG_ROOT)
        self._build_asset_list(tab_dicts, dict_updater.OPEN_DICT_ASSETS, missing_first_run)

        byo_border = tk.Frame(tab_dicts, bg=BORDER)
        byo_border.pack(fill="x", pady=(0, 6))
        byo = tk.Frame(byo_border, bg=BG_ELEVATED, padx=12, pady=8)
        byo.pack(fill="x", padx=1, pady=1)
        tk.Label(
            byo,
            text="Optional: Bring Your Own Yomitan Dictionaries",
            bg=BG_ELEVATED,
            fg=FG_PRIMARY,
            font=(UI_FONT, 9, "bold"),
        ).pack(anchor="w")
        tk.Label(
            byo,
            text=(
                "Proprietary dictionaries (e.g. Babylon, Daijirin, Kenkyusha) are excluded from "
                "automatic downloads for licensing reasons. Drop any Yomitan .zip dictionary you "
                "own into dictionaries/ and click Reload Dictionaries."
            ),
            bg=BG_ELEVATED,
            fg=FG_SECONDARY,
            font=(UI_FONT, 8),
            wraplength=610,
            justify="left",
        ).pack(anchor="w", pady=(2, 0))

        # Tab 2: OCR Models
        tab_ocr = tk.Frame(tab_content, bg=BG_ROOT)
        self._build_asset_list(tab_ocr, dict_updater.OPEN_OCR_MODEL_ASSETS, missing_first_run)

        ocr_note_border = tk.Frame(tab_ocr, bg=BORDER)
        ocr_note_border.pack(fill="x", pady=(0, 6))
        ocr_note = tk.Frame(ocr_note_border, bg=BG_ELEVATED, padx=12, pady=8)
        ocr_note.pack(fill="x", padx=1, pady=1)
        tk.Label(
            ocr_note,
            text="Note on OCR Models & Licensing",
            bg=BG_ELEVATED,
            fg=FG_PRIMARY,
            font=(UI_FONT, 9, "bold"),
        ).pack(anchor="w")
        tk.Label(
            ocr_note,
            text=(
                "• RapidOCR & Manga-OCR (Apache 2.0) auto-download via Python packages on first use.\n"
                "• Local ONNX/Safetensors models above are pinned & SHA-256 verified to match ocr_server.py.\n"
                "• YomiToku PARSeq-Small is CC-BY-NC-SA 4.0 (personal/non-commercial use only; opt-in)."
            ),
            bg=BG_ELEVATED,
            fg=FG_SECONDARY,
            font=(UI_FONT, 8),
            wraplength=610,
            justify="left",
        ).pack(anchor="w", pady=(2, 0))

        self.tab_bar.add_tab("dictionaries", "Dictionaries", tab_dicts)
        self.tab_bar.add_tab("ocr_models", "OCR Models", tab_ocr)
        # If only OCR models are missing on first run, focus OCR Models tab
        if self.first_run and missing_first_run and not any(
            a.asset_id in missing_first_run for a in dict_updater.OPEN_DICT_ASSETS
        ):
            self.tab_bar.select("ocr_models")
        else:
            self.tab_bar.select("dictionaries")

        # Progress bar & status
        self.progress_var = tk.DoubleVar(value=0.0)
        self.progress_bar = ttk.Progressbar(
            outer, variable=self.progress_var, maximum=100.0, mode="determinate"
        )
        self.progress_bar.pack(fill="x", pady=(8, 4))

        self.progress_text_var = tk.StringVar(
            value="Select items across Dictionaries and OCR Models to download or update."
        )
        tk.Label(
            outer,
            textvariable=self.progress_text_var,
            bg=BG_ROOT,
            fg=FG_SECONDARY,
            font=(UI_FONT, 9),
        ).pack(anchor="w", pady=(0, 10))

        # Bottom action buttons
        btns = tk.Frame(outer, bg=BG_ROOT)
        btns.pack(fill="x")

        self.download_btn = make_action_button(
            btns,
            text="Download Selected" if self.first_run else "Download / Update Selected",
            command=self._start_download,
            variant="primary",
            padx=14,
            pady=6,
        )
        self.download_btn.pack(side="left", padx=(0, 8))

        self.open_folder_btn = make_secondary_button(
            btns,
            text="Open dictionaries/",
            command=lambda: self._open_subdir("dictionaries"),
            padx=10,
            pady=6,
        )
        self.open_folder_btn.pack(side="left", padx=(0, 6))

        self.open_models_btn = make_secondary_button(
            btns,
            text="Open ocr_models/",
            command=lambda: self._open_subdir("ocr_models"),
            padx=10,
            pady=6,
        )
        self.open_models_btn.pack(side="left", padx=(0, 8))

        self.cancel_btn = make_action_button(
            btns,
            text="Cancel",
            command=self._cancel_download,
            variant="danger",
            padx=10,
            pady=6,
        )
        self.cancel_btn.configure(state="disabled")
        self.cancel_btn.pack(side="left")

        close_label = "Skip for Now" if self.first_run else "Close"
        self.close_btn = make_secondary_button(
            btns,
            text=close_label,
            command=self._on_close,
            padx=14,
            pady=6,
        )
        self.close_btn.pack(side="right")

    def _refresh_statuses(self) -> None:
        for spec in dict_updater.ALL_ASSETS:
            st = dict_updater.get_asset_status(self.app_dir, spec.asset_id)
            lbl = self._status_labels[spec.asset_id]
            ver_lbl = self._version_labels[spec.asset_id]
            if st["installed"]:
                lbl.configure(
                    text=f"Installed ({_fmt_size(st['size_bytes'])})",
                    fg=SUCCESS,
                )
                ver_text = st["version"]
                if st["updated_at"]:
                    ver_text = (
                        f"{ver_text} • {st['updated_at'][:10]}"
                        if ver_text
                        else st["updated_at"][:10]
                    )
                ver_lbl.configure(text=ver_text)
            else:
                lbl.configure(
                    text="Missing (Recommended)" if spec.required_for_first_run else "Optional",
                    fg=WARNING if spec.required_for_first_run else FG_MUTED,
                )
                ver_lbl.configure(text="")

    def _open_subdir(self, subdir: str) -> None:
        d = os.path.join(self.app_dir, subdir)
        os.makedirs(d, exist_ok=True)
        try:
            if sys.platform == "win32":
                os.startfile(d)
            elif sys.platform == "darwin":
                subprocess.Popen(["open", d])
            else:
                subprocess.Popen(["xdg-open", d])
        except Exception as exc:
            messagebox.showerror("Error", f"Could not open folder:\n{exc}", parent=self.win)

    def _start_download(self) -> None:
        if self._working:
            return
        selected: List[str] = [
            spec.asset_id
            for spec in dict_updater.ALL_ASSETS
            if self._check_vars[spec.asset_id].get()
        ]
        if not selected:
            messagebox.showinfo(
                "No Selection",
                "Check at least one dictionary or OCR model to download or update.",
                parent=self.win,
            )
            return

        # Stop running server before writing so jamdict.db / model files aren't locked on Windows
        if self.on_before_write is not None:
            try:
                self.on_before_write()
            except Exception:
                pass

        self._working = True
        self._cancel_event.clear()
        self.download_btn.configure(state="disabled")
        self.close_btn.configure(state="disabled")
        self.cancel_btn.configure(state="normal")
        self.progress_var.set(0.0)
        self.progress_text_var.set("Starting download...")

        threading.Thread(
            target=self._download_worker,
            args=(selected,),
            daemon=True,
            name="AssetUpdaterWorker",
        ).start()

    def _cancel_download(self) -> None:
        if self._working:
            self._cancel_event.set()
            self.progress_text_var.set("Cancelling...")
            self.cancel_btn.configure(state="disabled")

    def _download_worker(self, selected: List[str]) -> None:
        errors: List[str] = []
        completed_count = 0
        total_items = len(selected)

        for idx, asset_id in enumerate(selected):
            if self._cancel_event.is_set():
                break
            spec = dict_updater.ASSETS_BY_ID[asset_id]

            def _prog(done: int, total: int, label: str, item_idx: int = idx) -> None:
                if total > 0:
                    frac = min(1.0, done / float(total))
                    pct = ((item_idx + frac) / total_items) * 100.0
                    msg = f"[{item_idx + 1}/{total_items}] {label} ({_fmt_size(done)} / {_fmt_size(total)})"
                else:
                    pct = (item_idx / total_items) * 100.0
                    msg = f"[{item_idx + 1}/{total_items}] {label}"
                self._schedule_ui(lambda p=pct, m=msg: self._update_progress(p, m))

            try:
                dict_updater.download_or_update_asset(
                    self.app_dir,
                    asset_id,
                    progress_cb=_prog,
                    cancel_event=self._cancel_event,
                )
                completed_count += 1
                self._did_update = True
                self._schedule_ui(self._refresh_statuses)
            except Exception as exc:
                if self._cancel_event.is_set():
                    break
                errors.append(f"{spec.title}: {exc}")

        cancelled = self._cancel_event.is_set()
        self._schedule_ui(lambda: self._on_download_finished(completed_count, errors, cancelled))

    def _schedule_ui(self, fn: Callable[[], None]) -> None:
        if self._closed:
            return
        try:
            self.win.after(0, fn)
        except tk.TclError:
            pass

    def _update_progress(self, pct: float, text: str) -> None:
        if self._closed:
            return
        self.progress_var.set(pct)
        self.progress_text_var.set(text)

    def _on_download_finished(self, completed_count: int, errors: List[str], cancelled: bool) -> None:
        if self._closed:
            return
        self._working = False
        self.download_btn.configure(state="normal")
        self.close_btn.configure(state="normal")
        self.cancel_btn.configure(state="disabled")
        self._refresh_statuses()

        if cancelled:
            self.progress_text_var.set("Download cancelled.")
            return

        if errors:
            self.progress_text_var.set(
                f"Completed {completed_count} item(s) with {len(errors)} error(s)."
            )
            messagebox.showerror(
                "Update Errors",
                "Some items failed to update:\n\n" + "\n".join(errors),
                parent=self.win,
            )
            return

        self.progress_var.set(100.0)
        self.progress_text_var.set(
            f"Successfully updated {completed_count} asset(s)!"
        )
        self.close_btn.configure(text="Done")
        if self.first_run:
            self.win.after(600, self._on_close)

    def _on_close(self) -> None:
        if self._working:
            if not messagebox.askyesno(
                "Cancel Download?",
                "A download is in progress. Cancel it and close?",
                parent=self.win,
            ):
                return
            self._cancel_event.set()
        self._closed = True
        try:
            self.win.grab_release()
        except Exception:
            pass
        try:
            self.win.destroy()
        except Exception:
            pass
        if self.on_complete is not None:
            try:
                self.on_complete(self._did_update)
            except Exception:
                pass
