"""
psk_store.py - Thread-safe per-3DS PSK store and pairing registry (`ocr_psks.json`).

Manages:
  - `pending_psk`: the 256-bit key currently displayed in the pairing QR code.
  - `devices`: list of paired 3DS console records with their individual PSKs,
    hardware IDs, system nicknames, models, and timestamps.
"""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import subprocess
import threading
import time
from typing import Any, Dict, List, Optional, Tuple

_STORE_LOCK = threading.Lock()
_CACHE_LOCK = threading.Lock()
_VALID_PSK_CACHE: Dict[str, Tuple[float, set]] = {}  # store_path -> (mtime_ns, set_of_psks)

STORE_FILENAME = "ocr_psks.json"
LEGACY_PSK_FILENAME = "ocr_psk.txt"


def get_store_path(app_dir: str) -> str:
    return os.path.join(app_dir, STORE_FILENAME)


def _now_str() -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S")


def _apply_restricted_permissions(path: str) -> None:
    try:
        if os.name == "nt":
            user = os.environ.get("USERNAME", "")
            if user:
                subprocess.run(
                    ["icacls", path, "/inheritance:r", "/grant:r", f"{user}:(R,W)"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                )
        else:
            os.chmod(path, 0o600)
    except Exception:
        pass


def _atomic_write_json(path: str, data: Dict[str, Any]) -> None:
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    tmp_path = f"{path}.{os.getpid()}.{threading.get_ident()}.tmp"
    with open(tmp_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    _apply_restricted_permissions(tmp_path)

    for attempt in range(5):
        try:
            os.replace(tmp_path, path)
            break
        except PermissionError:
            if attempt == 4:
                try:
                    os.remove(tmp_path)
                except OSError:
                    pass
                raise
            time.sleep(0.03 * (attempt + 1))

    _apply_restricted_permissions(path)
    with _CACHE_LOCK:
        _VALID_PSK_CACHE.pop(path, None)


def _normalize_store(raw: Any) -> Tuple[Dict[str, Any], bool]:
    changed = False
    if not isinstance(raw, dict):
        raw = {}
        changed = True

    pending = str(raw.get("pending_psk", "") or "").strip()
    if not pending:
        pending = secrets.token_urlsafe(32)
        changed = True

    devices_in = raw.get("devices", [])
    if not isinstance(devices_in, list):
        devices_in = []
        changed = True

    devices: List[Dict[str, Any]] = []
    seen_psks = set()
    for d in devices_in:
        if not isinstance(d, dict):
            changed = True
            continue
        psk = str(d.get("psk", "") or "").strip()
        if not psk or psk in seen_psks:
            changed = True
            continue
        seen_psks.add(psk)
        dev_id = str(d.get("device_id", "") or "").strip() or (
            "dev_" + hashlib.sha256(psk.encode("utf-8")).hexdigest()[:8]
        )
        name = str(d.get("name", "") or "").strip() or "3DS"
        model = str(d.get("model", "") or "").strip() or "3DS"
        devices.append(
            {
                "device_id": dev_id,
                "name": name,
                "model": model,
                "psk": psk,
                "paired_at": str(d.get("paired_at", "") or ""),
                "last_seen": str(d.get("last_seen", "") or ""),
                "custom_name": bool(d.get("custom_name", False)),
            }
        )

    return {"pending_psk": pending, "devices": devices}, changed


def _load_or_init_locked(app_dir: str) -> Dict[str, Any]:
    path = get_store_path(app_dir)
    if os.path.isfile(path):
        try:
            with open(path, "r", encoding="utf-8") as f:
                raw = json.load(f)
            store, changed = _normalize_store(raw)
            if changed:
                _atomic_write_json(path, store)
            return store
        except Exception:
            pass

    devices: List[Dict[str, Any]] = []
    legacy_path = os.path.join(app_dir, LEGACY_PSK_FILENAME)
    if os.path.isfile(legacy_path):
        try:
            with open(legacy_path, "r", encoding="utf-8") as f:
                old_psk = f.read().strip()
            if old_psk:
                devices.append(
                    {
                        "device_id": "legacy",
                        "name": "Existing 3DS (Legacy)",
                        "model": "3DS",
                        "psk": old_psk,
                        "paired_at": _now_str(),
                        "last_seen": "",
                        "custom_name": False,
                    }
                )
        except Exception:
            pass

    store = {
        "pending_psk": secrets.token_urlsafe(32),
        "devices": devices,
    }
    try:
        _atomic_write_json(path, store)
    except Exception:
        pass
    return store


def load_or_init_store(app_dir: str) -> Dict[str, Any]:
    with _STORE_LOCK:
        return _load_or_init_locked(app_dir)


def get_all_valid_psks(app_dir: str) -> List[Tuple[bytes, str, Optional[str]]]:
    """Return list of `(psk_bytes, role, device_id)` for all active PSKs."""
    result: List[Tuple[bytes, str, Optional[str]]] = []
    seen: set = set()

    env_psk = os.environ.get("OCR_PSK", "").strip()
    if env_psk:
        b = env_psk.encode("utf-8")
        result.append((b, "env", None))
        seen.add(env_psk)

    store = load_or_init_store(app_dir)
    for dev in store.get("devices", []):
        p = str(dev.get("psk", "") or "").strip()
        if p and p not in seen:
            seen.add(p)
            result.append((p.encode("utf-8"), "device", str(dev.get("device_id", ""))))

    pending = str(store.get("pending_psk", "") or "").strip()
    if pending and pending not in seen:
        seen.add(pending)
        result.append((pending.encode("utf-8"), "pending", None))

    return result


def is_psk_valid(app_dir: str, psk_str: str) -> bool:
    """Fast mtime-cached check for whether `psk_str` is currently authorized."""
    if not psk_str:
        return False
    env_psk = os.environ.get("OCR_PSK", "").strip()
    if env_psk and psk_str == env_psk:
        return True

    path = get_store_path(app_dir)
    try:
        mtime_ns = os.stat(path).st_mtime_ns
    except OSError:
        mtime_ns = -1

    with _CACHE_LOCK:
        cached = _VALID_PSK_CACHE.get(path)
        if cached is not None and cached[0] == mtime_ns:
            return psk_str in cached[1]

    store = load_or_init_store(app_dir)
    valid_set = set()
    pending = str(store.get("pending_psk", "") or "").strip()
    if pending:
        valid_set.add(pending)
    for dev in store.get("devices", []):
        p = str(dev.get("psk", "") or "").strip()
        if p:
            valid_set.add(p)

    try:
        new_mtime_ns = os.stat(path).st_mtime_ns
    except OSError:
        new_mtime_ns = mtime_ns
    with _CACHE_LOCK:
        _VALID_PSK_CACHE[path] = (new_mtime_ns, valid_set)

    return psk_str in valid_set


def claim_pairing_psk(
    app_dir: str,
    matched_psk: str,
    device_id: str,
    name: str,
    model: str,
) -> Tuple[bool, str, Dict[str, Any]]:
    """
    Claim `matched_psk` for a 3DS console.
    If `matched_psk` is `pending_psk`, registers/updates the device and rotates `pending_psk`.
    Returns `(rotated_pending, current_pending_psk, device_record)`.
    """
    clean_id = "".join(ch for ch in (device_id or "").strip() if ch.isprintable())[:64]
    clean_name = "".join(ch for ch in (name or "").strip() if ch.isprintable())[:64] or "3DS"
    clean_model = "".join(ch for ch in (model or "").strip() if ch.isprintable())[:32] or "3DS"
    if not clean_id:
        clean_id = "dev_" + hashlib.sha256(matched_psk.encode("utf-8")).hexdigest()[:12]

    now = _now_str()
    with _STORE_LOCK:
        store = _load_or_init_locked(app_dir)
        path = get_store_path(app_dir)

        if matched_psk == store.get("pending_psk"):
            target: Optional[Dict[str, Any]] = None
            for dev in store["devices"]:
                if dev.get("device_id") == clean_id:
                    target = dev
                    break

            if target is not None:
                target["psk"] = matched_psk
                target["model"] = clean_model
                target["paired_at"] = now
                target["last_seen"] = now
                if not target.get("custom_name"):
                    target["name"] = clean_name
            else:
                target = {
                    "device_id": clean_id,
                    "name": clean_name,
                    "model": clean_model,
                    "psk": matched_psk,
                    "paired_at": now,
                    "last_seen": now,
                    "custom_name": False,
                }
                store["devices"].append(target)

            new_pending = secrets.token_urlsafe(32)
            store["pending_psk"] = new_pending
            _atomic_write_json(path, store)
            return True, new_pending, dict(target)

        for dev in store["devices"]:
            if dev.get("psk") == matched_psk:
                old_id = str(dev.get("device_id", "") or "")
                if old_id == "legacy" or old_id.startswith("unregistered_"):
                    dev["device_id"] = clean_id
                dev["model"] = clean_model
                dev["last_seen"] = now
                if not dev.get("custom_name"):
                    dev["name"] = clean_name
                _atomic_write_json(path, store)
                return False, store["pending_psk"], dict(dev)

        return False, store["pending_psk"], {}


def touch_device_last_seen(
    app_dir: str, matched_psk: str, fallback_ip: str = ""
) -> Optional[Dict[str, Any]]:
    """
    Update `last_seen` for `matched_psk`. If `matched_psk` was still `pending_psk`
    (e.g. paired while server was offline), auto-claims it with a placeholder record
    and rotates `pending_psk`.
    """
    now = _now_str()
    with _STORE_LOCK:
        store = _load_or_init_locked(app_dir)
        path = get_store_path(app_dir)

        for dev in store["devices"]:
            if dev.get("psk") == matched_psk:
                dev["last_seen"] = now
                _atomic_write_json(path, store)
                return dict(dev)

        if matched_psk == store.get("pending_psk"):
            placeholder_id = "unregistered_" + hashlib.sha256(matched_psk.encode("utf-8")).hexdigest()[:8]
            label = f"3DS ({fallback_ip})" if fallback_ip else "3DS"
            rec = {
                "device_id": placeholder_id,
                "name": label,
                "model": "3DS",
                "psk": matched_psk,
                "paired_at": now,
                "last_seen": now,
                "custom_name": False,
            }
            store["devices"].append(rec)
            store["pending_psk"] = secrets.token_urlsafe(32)
            _atomic_write_json(path, store)
            return dict(rec)

        return None


def rotate_pending_psk(app_dir: str) -> str:
    """Generate a new pending pairing PSK without invalidating any already-paired 3DSes."""
    with _STORE_LOCK:
        store = _load_or_init_locked(app_dir)
        new_pending = secrets.token_urlsafe(32)
        store["pending_psk"] = new_pending
        _atomic_write_json(get_store_path(app_dir), store)
        return new_pending


def invalidate_device_psk(app_dir: str, psk_or_device_id: str) -> Optional[Dict[str, Any]]:
    """Remove a paired 3DS by its PSK or `device_id`, immediately revoking its access."""
    key = (psk_or_device_id or "").strip()
    if not key:
        return None
    with _STORE_LOCK:
        store = _load_or_init_locked(app_dir)
        removed: Optional[Dict[str, Any]] = None
        kept: List[Dict[str, Any]] = []
        for dev in store["devices"]:
            if removed is None and (dev.get("psk") == key or dev.get("device_id") == key):
                removed = dict(dev)
            else:
                kept.append(dev)
        if removed is not None:
            store["devices"] = kept
            _atomic_write_json(get_store_path(app_dir), store)
        return removed


def rename_device(app_dir: str, device_id: str, new_name: str) -> bool:
    """Assign a custom display name to a paired 3DS."""
    clean_name = "".join(ch for ch in (new_name or "").strip() if ch.isprintable())[:64]
    if not clean_name:
        return False
    with _STORE_LOCK:
        store = _load_or_init_locked(app_dir)
        for dev in store["devices"]:
            if dev.get("device_id") == device_id:
                dev["name"] = clean_name
                dev["custom_name"] = True
                _atomic_write_json(get_store_path(app_dir), store)
                return True
        return False
