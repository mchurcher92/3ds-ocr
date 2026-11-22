import os
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import unittest

from anki.collection import Collection
from anki.sync import SyncAuth, SyncOutput

from ocr_server import _upload_only_media_proxy


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class TestAnkiUploadOnlyMediaSync(unittest.TestCase):
    def test_upload_only_media_proxy_against_syncserver(self) -> None:
        port = _free_port()
        with tempfile.TemporaryDirectory() as td:
            srv_dir = os.path.join(td, "srv")
            os.makedirs(srv_dir)
            log_path = os.path.join(td, "srv.log")
            env = os.environ.copy()
            env["SYNC_USER1"] = "testuser:testpass"
            env["SYNC_BASE"] = srv_dir
            env["SYNC_HOST"] = "127.0.0.1"
            env["SYNC_PORT"] = str(port)

            with open(log_path, "wb") as log_file:
                proc = subprocess.Popen(
                    [sys.executable, "-m", "anki.syncserver"],
                    env=env,
                    stdout=log_file,
                    stderr=subprocess.STDOUT,
                )
                c1 = None
                c2 = None
                try:
                    for _ in range(50):
                        try:
                            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                                break
                        except OSError:
                            time.sleep(0.1)

                    endpoint = f"http://127.0.0.1:{port}/"

                    # 1. Client 1 uploads two existing media files normally
                    c1_dir = os.path.join(td, "c1")
                    os.makedirs(c1_dir)
                    c1 = Collection(os.path.join(c1_dir, "collection.anki2"))
                    auth1 = c1.sync_login("testuser", "testpass", endpoint)
                    out1 = c1.sync_collection(auth1, sync_media=False)
                    if out1.required in (SyncOutput.FULL_UPLOAD, SyncOutput.FULL_SYNC):
                        c1.full_upload_or_download(
                            auth=auth1, server_usn=out1.server_media_usn, upload=True
                        )
                    c1.media.write_data("existing1.jpg", b"existing_data_1")
                    c1.media.write_data("existing2.jpg", b"existing_data_2")
                    c1.sync_media(auth1)
                    while c1.media_sync_status().active:
                        time.sleep(0.05)
                    c1.close()
                    c1 = None

                    # 2. Client 2 syncs collection only, adds 3ds_new1.jpg, and syncs via proxy
                    c2_dir = os.path.join(td, "c2")
                    os.makedirs(c2_dir)
                    c2 = Collection(os.path.join(c2_dir, "collection.anki2"))
                    auth2 = c2.sync_login("testuser", "testpass", endpoint)
                    out2 = c2.sync_collection(auth2, sync_media=False)
                    if out2.required in (SyncOutput.FULL_DOWNLOAD, SyncOutput.FULL_SYNC):
                        c2.full_upload_or_download(
                            auth=auth2, server_usn=out2.server_media_usn, upload=False
                        )
                        c2.close()
                        c2 = Collection(os.path.join(c2_dir, "collection.anki2"))

                    c2.media.write_data("3ds_new1.jpg", b"screenshot_1_bytes")
                    c2.sync_collection(auth2, sync_media=False)
                    with _upload_only_media_proxy(endpoint) as proxy_url:
                        c2.sync_media(SyncAuth(hkey=auth2.hkey, endpoint=proxy_url))
                        while c2.media_sync_status().active:
                            time.sleep(0.05)

                    c2_files_1 = sorted(os.listdir(os.path.join(c2_dir, "collection.media")))
                    self.assertEqual(c2_files_1, ["3ds_new1.jpg"])

                    mconn = sqlite3.connect(os.path.join(c2_dir, "collection.media.db2"))
                    try:
                        rows = mconn.execute("SELECT fname, dirty FROM media").fetchall()
                    finally:
                        mconn.close()
                    self.assertEqual(rows, [("3ds_new1.jpg", 0)])

                    # 3. Client 2 adds 3ds_new2.jpg and syncs again via proxy
                    c2.media.write_data("3ds_new2.jpg", b"screenshot_2_bytes")
                    with _upload_only_media_proxy(endpoint) as proxy_url:
                        c2.sync_media(SyncAuth(hkey=auth2.hkey, endpoint=proxy_url))
                        while c2.media_sync_status().active:
                            time.sleep(0.05)

                    c2_files_2 = sorted(os.listdir(os.path.join(c2_dir, "collection.media")))
                    self.assertEqual(c2_files_2, ["3ds_new1.jpg", "3ds_new2.jpg"])

                    mconn = sqlite3.connect(os.path.join(c2_dir, "collection.media.db2"))
                    try:
                        rows = sorted(mconn.execute("SELECT fname, dirty FROM media").fetchall())
                    finally:
                        mconn.close()
                    self.assertEqual(rows, [("3ds_new1.jpg", 0), ("3ds_new2.jpg", 0)])

                    c2.close()
                    c2 = None

                    # 4. Client 1 syncs normally and receives both screenshots
                    c1 = Collection(os.path.join(c1_dir, "collection.anki2"))
                    c1.sync_collection(auth1, sync_media=False)
                    c1.sync_media(auth1)
                    while c1.media_sync_status().active:
                        time.sleep(0.05)

                    c1_files = sorted(os.listdir(os.path.join(c1_dir, "collection.media")))
                    self.assertEqual(
                        c1_files,
                        ["3ds_new1.jpg", "3ds_new2.jpg", "existing1.jpg", "existing2.jpg"],
                    )
                    with open(os.path.join(c1_dir, "collection.media", "3ds_new1.jpg"), "rb") as f:
                        self.assertEqual(f.read(), b"screenshot_1_bytes")
                    with open(os.path.join(c1_dir, "collection.media", "3ds_new2.jpg"), "rb") as f:
                        self.assertEqual(f.read(), b"screenshot_2_bytes")
                    c1.close()
                    c1 = None
                finally:
                    if c1 is not None:
                        c1.close()
                    if c2 is not None:
                        c2.close()
                    proc.terminate()
                    proc.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
