"""
git_updater.py - Git update checker, classifier, and fast-forward applier.

Checks the local git repository against its remote tracking branch, classifies
changed files into:
  - "reboot_gui": GUI/launcher files changed -> stop server, pull, reboot GUI
  - "restart_server": server/dictionary/OCR model files changed -> stop server, pull, restart server
  - "none": docs, 3DS .3gx plugins, or build scripts only -> pull without restart
"""

from __future__ import annotations

from dataclasses import dataclass, field
import os
import shutil
import subprocess
import sys
from typing import List, Optional, Tuple

_GUI_FILES = frozenset(
    {
        "ocr_launcher.py",
        "ui_theme.py",
        "anki_dialog.py",
        "dict_updater.py",
        "dict_updater_dialog.py",
        "psk_store.py",
        "git_updater.py",
        "requirements.txt",
        "OCR_Server.spec",
    }
)

_SERVER_FILES = frozenset(
    {
        "ocr_server.py",
        "japanese_word_meta.py",
    }
)

_NON_RUNTIME_PREFIXES = (
    "3gx_plugin/",
    "3gx_plugin_hb/",
    "3gx_plugin_slim/",
    "CTRPluginFramework",
    "full/",
    "homebrew/",
    "slim/",
    "tests/",
)

_NON_RUNTIME_FILES = frozenset(
    {
        ".gitignore",
        "README.md",
        "THIRD_PARTY_LICENSES.md",
        "build_hb_plugin.ps1",
        "build_plugin.bat",
        "build_plugin.ps1",
        "build_slim_plugin.ps1",
        "anki_config.example.json",
        "anki_credentials.example.json",
        "gemini_credentials.example.json",
    }
)


@dataclass
class GitUpdateStatus:
    ok: bool
    error: str = ""
    branch: str = ""
    upstream: str = ""
    local_commit: str = ""
    remote_commit: str = ""
    behind_count: int = 0
    dirty_tracked: bool = False
    commits: List[str] = field(default_factory=list)
    changed_files: List[str] = field(default_factory=list)
    action: str = "none"  # "none" | "restart_server" | "reboot_gui"
    action_reason: str = ""
    has_plugin_updates: bool = False


@dataclass
class GitApplyResult:
    ok: bool
    error: str = ""
    updated: bool = False
    action: str = "none"
    action_reason: str = ""
    changed_files: List[str] = field(default_factory=list)
    new_commit: str = ""
    has_plugin_updates: bool = False


def _run_git(
    repo_dir: str, args: List[str], timeout: int = 15
) -> Tuple[int, str, str]:
    git_bin = shutil.which("git")
    if not git_bin:
        return 127, "", "git executable not found on PATH."
    creation_flags = 0x08000000 if sys.platform == "win32" else 0
    try:
        proc = subprocess.run(
            [git_bin] + args,
            cwd=repo_dir,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            creationflags=creation_flags,
        )
        return proc.returncode, proc.stdout.strip(), proc.stderr.strip()
    except subprocess.TimeoutExpired:
        return 124, "", f"git {' '.join(args)} timed out after {timeout}s."
    except Exception as exc:
        return 1, "", str(exc)


def classify_update_action(changed_files: List[str]) -> Tuple[str, str, bool]:
    """
    Classify changed files into (action, action_reason, has_plugin_updates).
    Precedence: reboot_gui > restart_server > none.
    """
    if not changed_files:
        return "none", "No files changed", False

    norm_files = [f.replace("\\", "/").strip() for f in changed_files if f and f.strip()]
    has_plugin_updates = any(
        p.endswith(".3gx") or p.startswith(("full/", "homebrew/", "slim/"))
        for p in norm_files
    )

    gui_changed = False
    server_changed = False
    dict_changed = False

    for p in norm_files:
        if p in _GUI_FILES:
            gui_changed = True
            continue
        if p in _SERVER_FILES or p.startswith("ocr_models/"):
            server_changed = True
            continue
        if p.startswith("dictionaries/"):
            dict_changed = True
            continue
        if p in _NON_RUNTIME_FILES or p.startswith(_NON_RUNTIME_PREFIXES):
            continue
        if p.endswith(".py") and "/" not in p:
            server_changed = True

    if gui_changed:
        return "reboot_gui", "GUI/launcher files changed", has_plugin_updates
    if server_changed and dict_changed:
        return (
            "restart_server",
            "Server & dictionary files changed",
            has_plugin_updates,
        )
    if dict_changed:
        return (
            "restart_server",
            "Dictionary files changed (reload & restart server)",
            has_plugin_updates,
        )
    if server_changed:
        return "restart_server", "Server/model files changed", has_plugin_updates
    if has_plugin_updates:
        return (
            "none",
            "3DS plugin/docs files changed (no server restart needed)",
            True,
        )
    return "none", "Docs/non-runtime files changed", False


def get_local_git_info(repo_dir: str) -> Tuple[bool, str, str]:
    """Return (is_git_repo, branch, short_commit) without network I/O."""
    if not repo_dir or not os.path.isdir(repo_dir):
        return False, "", ""
    rc, inside, _ = _run_git(repo_dir, ["rev-parse", "--is-inside-work-tree"], timeout=5)
    if rc != 0 or inside.lower() != "true":
        return False, "", ""
    rc_b, branch, _ = _run_git(repo_dir, ["rev-parse", "--abbrev-ref", "HEAD"], timeout=5)
    rc_c, commit, _ = _run_git(repo_dir, ["rev-parse", "--short", "HEAD"], timeout=5)
    return True, (branch if rc_b == 0 else ""), (commit if rc_c == 0 else "")


def check_for_updates(
    repo_dir: str, fetch: bool = True, timeout: int = 15
) -> GitUpdateStatus:
    """Fetch remote (if requested) and inspect incoming commits and changed files."""
    if not repo_dir or not os.path.isdir(repo_dir):
        return GitUpdateStatus(ok=False, error=f"Directory not found: {repo_dir}")

    rc, inside, err = _run_git(repo_dir, ["rev-parse", "--is-inside-work-tree"], timeout=5)
    if rc != 0 or inside.lower() != "true":
        return GitUpdateStatus(ok=False, error=err or "Not a git repository.")

    rc_b, branch, err_b = _run_git(repo_dir, ["rev-parse", "--abbrev-ref", "HEAD"], timeout=5)
    if rc_b != 0 or not branch:
        return GitUpdateStatus(ok=False, error=err_b or "Could not determine current branch.")

    rc_u, upstream, _ = _run_git(
        repo_dir, ["rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"], timeout=5
    )
    if rc_u != 0 or not upstream:
        upstream = f"origin/{branch}"

    if fetch:
        rc_f, _, err_f = _run_git(repo_dir, ["fetch", "--quiet"], timeout=timeout)
        if rc_f != 0:
            return GitUpdateStatus(
                ok=False,
                error=err_f or "git fetch failed.",
                branch=branch,
                upstream=upstream,
            )

    rc_l, local_sha, _ = _run_git(repo_dir, ["rev-parse", "--short", "HEAD"], timeout=5)
    rc_r, remote_sha, err_r = _run_git(
        repo_dir, ["rev-parse", "--short", upstream], timeout=5
    )
    if rc_r != 0:
        return GitUpdateStatus(
            ok=False,
            error=err_r or f"Remote tracking branch '{upstream}' not found.",
            branch=branch,
            upstream=upstream,
            local_commit=local_sha if rc_l == 0 else "",
        )

    rc_cnt, cnt_str, err_cnt = _run_git(
        repo_dir, ["rev-list", "--count", f"HEAD..{upstream}"], timeout=5
    )
    if rc_cnt != 0:
        return GitUpdateStatus(
            ok=False,
            error=err_cnt or "Could not count incoming commits.",
            branch=branch,
            upstream=upstream,
            local_commit=local_sha,
            remote_commit=remote_sha,
        )
    try:
        behind_count = int(cnt_str or "0")
    except ValueError:
        behind_count = 0

    rc_st, st_out, _ = _run_git(
        repo_dir, ["status", "--porcelain", "--untracked-files=no"], timeout=5
    )
    dirty_tracked = bool(rc_st == 0 and st_out.strip())

    commits: List[str] = []
    changed_files: List[str] = []
    action = "none"
    action_reason = "Up to date"
    has_plugin_updates = False

    if behind_count > 0:
        rc_log, log_out, _ = _run_git(
            repo_dir, ["log", "--oneline", f"HEAD..{upstream}"], timeout=10
        )
        if rc_log == 0 and log_out:
            commits = [ln.strip() for ln in log_out.splitlines() if ln.strip()]

        rc_diff, diff_out, _ = _run_git(
            repo_dir, ["diff", "--name-only", f"HEAD..{upstream}"], timeout=10
        )
        if rc_diff == 0 and diff_out:
            changed_files = [ln.strip() for ln in diff_out.splitlines() if ln.strip()]

        action, action_reason, has_plugin_updates = classify_update_action(changed_files)

    return GitUpdateStatus(
        ok=True,
        branch=branch,
        upstream=upstream,
        local_commit=local_sha,
        remote_commit=remote_sha,
        behind_count=behind_count,
        dirty_tracked=dirty_tracked,
        commits=commits,
        changed_files=changed_files,
        action=action,
        action_reason=action_reason,
        has_plugin_updates=has_plugin_updates,
    )


def apply_updates(
    repo_dir: str,
    status: Optional[GitUpdateStatus] = None,
    timeout: int = 30,
) -> GitApplyResult:
    """Fast-forward pull updates from the remote tracking branch."""
    if status is None:
        status = check_for_updates(repo_dir, fetch=True, timeout=timeout)

    if not status.ok:
        return GitApplyResult(ok=False, error=status.error)

    if status.behind_count <= 0:
        return GitApplyResult(
            ok=True,
            updated=False,
            action="none",
            action_reason="Already up to date",
            new_commit=status.local_commit,
        )

    if status.dirty_tracked:
        return GitApplyResult(
            ok=False,
            error=(
                "Local tracked files have uncommitted changes. "
                "Commit or stash them before pulling updates."
            ),
        )

    rc_p, _, err_p = _run_git(repo_dir, ["pull", "--ff-only"], timeout=timeout)
    if rc_p != 0:
        return GitApplyResult(
            ok=False,
            error=err_p or "git pull --ff-only failed.",
        )

    rc_c, new_sha, _ = _run_git(repo_dir, ["rev-parse", "--short", "HEAD"], timeout=5)
    return GitApplyResult(
        ok=True,
        updated=True,
        action=status.action,
        action_reason=status.action_reason,
        changed_files=list(status.changed_files),
        new_commit=new_sha if rc_c == 0 else status.remote_commit,
        has_plugin_updates=status.has_plugin_updates,
    )
