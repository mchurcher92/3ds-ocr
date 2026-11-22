"""
dict_updater_dialog.py - First-run setup and Dictionary Updater dialog for the desktop launcher.
"""

from __future__ import annotations

import os
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Callable, Dict, List, Optional

import dict_updater
from ui_theme import (
    ACCENT,
    BG_ELEVATED,
    BG_ROOT,
    BG_SURFACE,
    BORDER,
    DANGER,
    FG_MUTED,
    FG_PRIMARY,
    FG_SECONDARY,
    MONO_FONT,
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
    """Modal dialog for first-run open-licensed dictionary setup and on-demand updates."""

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
        self.win.title("First-Run Dictionary Setup" if first_run else "Dictionary Updater")
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
        _center_dialog(self.win, 660, 540)

    def _build_ui(self) -> None:
        outer = tk.Frame(self.win, bg=BG_ROOT, padx=18, pady=16)
        outer.pack(fill="both", expand=True)

        heading = (
            "First-Run Dictionary Setup"
            if self.first_run
            else "Open-Licensed Dictionary Updater"
        )
        subtitle = (
            "Core Japanese dictionaries are not bundled in Git. Download the open-licensed "
            "dictionaries below to get started right away."
            if self.first_run
            else "Download or refresh openly licensed Japanese dictionaries and stroke-order fonts "
            "from their official upstream sources."
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
            wraplength=620,
            justify="left",
        ).pack(anchor="w", pady=(4, 12))

        # Asset cards list
        list_frame = tk.Frame(outer, bg=BORDER, bd=0)
        list_frame.pack(fill="x", pady=(0, 12))
        inner = tk.Frame(list_frame, bg=BG_SURFACE, padx=12, pady=10)
        inner.pack(fill="both", expand=True, padx=1, pady=1)

        missing_first_run = set(dict_updater.get_missing_first_run_assets(self.app_dir))

        for idx, spec in enumerate(dict_updater.OPEN_DICT_ASSETS):
            if idx > 0:
                tk.Frame(inner, bg=BORDER, height=1).pack(fill="x", pady=8)

            row = tk.Frame(inner, bg=BG_SURFACE)
            row.pack(fill="x")

            default_checked = (
                (spec.asset_id in missing_first_run)
                if self.first_run
                else (not dict_updater.get_asset_status(self.app_dir, spec.asset_id)["installed"] or spec.asset_id == "jamdict")
            )
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
                text=f"{spec.description}",
                bg=BG_SURFACE,
                fg=FG_SECONDARY,
                font=(UI_FONT, 8),
                wraplength=580,
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

        # Bring-your-own Yomitan dictionaries info box
        byo_border = tk.Frame(outer, bg=BORDER)
        byo_border.pack(fill="x", pady=(0, 12))
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
                "automatic downloads for licensing reasons. To use any Yomitan-format .zip dictionary "
                "you already own, drop it into the dictionaries/ folder and click Reload Dictionaries."
            ),
            bg=BG_ELEVATED,
            fg=FG_SECONDARY,
            font=(UI_FONT, 8),
            wraplength=595,
            justify="left",
        ).pack(anchor="w", pady=(2, 0))

        # Progress bar & status
        self.progress_var = tk.DoubleVar(value=0.0)
        self.progress_bar = ttk.Progressbar(
            outer, variable=self.progress_var, maximum=100.0, mode="determinate"
        )
        self.progress_bar.pack(fill="x", pady=(0, 4))

        self.progress_text_var = tk.StringVar(
            value="Select the dictionaries to download or update."
        )
        tk.Label(
            outer,
            textvariable=self.progress_text_var,
            bg=BG_ROOT,
            fg=FG_SECONDARY,
            font=(UI_FONT, 9),
        ).pack(anchor="w", pady=(0, 12))

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
            text="Open dictionaries/ Folder",
            command=self._open_dict_folder,
            padx=12,
            pady=6,
        )
        self.open_folder_btn.pack(side="left", padx=(0, 8))

        self.cancel_btn = make_action_button(
            btns,
            text="Cancel",
            command=self._cancel_download,
            variant="danger",
            padx=12,
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
        for spec in dict_updater.OPEN_DICT_ASSETS:
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
                    ver_text = f"{ver_text} • {st['updated_at'][:10]}" if ver_text else st["updated_at"][:10]
                ver_lbl.configure(text=ver_text)
            else:
                lbl.configure(
                    text="Missing (Recommended)" if spec.required_for_first_run else "Not Installed",
                    fg=WARNING if spec.required_for_first_run else FG_MUTED,
                )
                ver_lbl.configure(text="")

    def _open_dict_folder(self) -> None:
        d = dict_updater.get_dictionaries_dir(self.app_dir)
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
            for spec in dict_updater.OPEN_DICT_ASSETS
            if self._check_vars[spec.asset_id].get()
        ]
        if not selected:
            messagebox.showinfo(
                "No Selection",
                "Check at least one dictionary to download or update.",
                parent=self.win,
            )
            return

        # Stop running server before writing so jamdict.db isn't locked on Windows
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
            name="DictUpdaterWorker",
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
                "Dictionary Update Errors",
                "Some dictionaries failed to update:\n\n" + "\n".join(errors),
                parent=self.win,
            )
            return

        self.progress_var.set(100.0)
        self.progress_text_var.set(
            f"Successfully updated {completed_count} open-licensed dictionary asset(s)!"
        )
        self.close_btn.configure(text="Done")
        if self.first_run:
            self.win.after(600, self._on_close)

    def _on_close(self) -> None:
        if self._working:
            if not messagebox.askyesno(
                "Cancel Download?",
                "A dictionary download is in progress. Cancel it and close?",
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
