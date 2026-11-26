#!/usr/bin/env python3
"""Manual display smoke test for the standalone SDL/Nuklear popup."""

import json
import ctypes
import http.server
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


class BrowserProgress(ctypes.Structure):
    _fields_ = [
        ("download_id", ctypes.c_uint32),
        ("bytes_received", ctypes.c_uint64),
        ("total_bytes", ctypes.c_uint64),
        ("progress", ctypes.c_float),
        ("status", ctypes.c_char * 24),
        ("error", ctypes.c_char * 256),
        ("dest_path", ctypes.c_char * 1024),
    ]


class ProgressV2(ctypes.Structure):
    _fields_ = [
        ("download_id", ctypes.c_uint32),
        ("bytes_received", ctypes.c_uint64),
        ("total_bytes", ctypes.c_uint64),
        ("speed_bps", ctypes.c_uint64),
        ("eta_seconds", ctypes.c_uint64),
        ("progress", ctypes.c_float),
        ("status", ctypes.c_char * 24),
        ("error", ctypes.c_char * 256),
    ]


def read_exact(connection: socket.socket, length: int) -> bytes:
    data = bytearray()
    while len(data) < length:
        part = connection.recv(length - len(data))
        assert part, "daemon closed the progress stream"
        data.extend(part)
    return bytes(data)


def offer_download(socket_path: Path, request_id: str, url: str) -> int:
    offer = json.dumps({
        "request_id": request_id, "url": url,
        "filename": "file.bin", "total_bytes": 1024,
    }).encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3)
        client.connect(str(socket_path))
        client.sendall(struct.pack("=II", len(offer), 12) + offer)
        return struct.unpack("=I", read_exact(client, 4))[0]


def check_popup_progress_stream(socket_path: Path, root: str, url: str) -> None:
    offer_id = offer_download(socket_path, "popup-progress-smoke", url)
    assert offer_id > 0
    destination = str(Path(root) / "file.bin").encode()
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(5)
        client.connect(str(socket_path))
        payload = struct.pack("=II", offer_id, len(destination)) + destination
        client.sendall(struct.pack("=II", len(payload), 14) + payload)
        download_id = struct.unpack("=I", read_exact(client, 4))[0]
        assert download_id > 0
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as subscriber:
        subscriber.settimeout(10)
        subscriber.connect(str(socket_path))
        subscriber.sendall(struct.pack("=III", 4, 16, download_id))
        initial = BrowserProgress.from_buffer_copy(
            read_exact(subscriber, ctypes.sizeof(BrowserProgress)))
        assert initial.download_id == download_id
        subscriber.sendall(struct.pack("=II", 0, 34))  # MSG_SUBSCRIBE_V2
        for _ in range(20):
            length, kind = struct.unpack("=II", read_exact(subscriber, 8))
            payload = read_exact(subscriber, length)
            if kind == 33:  # MSG_STATUS_EVENT_V2
                assert length == ctypes.sizeof(ProgressV2)
                update = ProgressV2.from_buffer_copy(payload)
                assert update.download_id == download_id
                assert update.eta_seconds == (2**64 - 1)
                assert update.speed_bps == 0
                assert update.status
                return
        raise AssertionError("popup subscription received no v2 progress")


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-popup-smoke-") as root:
        runtime = Path(root) / "runtime"
        runtime.mkdir(mode=0o700)
        environment = os.environ.copy()
        environment.update({"HOME": root, "XDG_RUNTIME_DIR": str(runtime),
                            "DOWNLOADMGR_ROOT": root})
        if not environment.get("DISPLAY") and not environment.get("WAYLAND_DISPLAY"):
            environment.setdefault("SDL_VIDEODRIVER", "offscreen")
            environment.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")
        class SlowFailure(http.server.BaseHTTPRequestHandler):
            def log_message(self, *_args):
                pass

            def do_HEAD(self):
                time.sleep(0.5)
                self.send_error(404)

            do_GET = do_HEAD

        origin = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SlowFailure)
        threading.Thread(target=origin.serve_forever, daemon=True).start()
        master, slave = pty.openpty()
        daemon = subprocess.Popen([cdm, "daemon"], stdin=slave,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL,
                                  env=environment)
        os.close(slave)
        popup = None
        try:
            socket_path = runtime / "cdm.sock"
            deadline = time.monotonic() + 10
            while not socket_path.exists() and time.monotonic() < deadline:
                assert daemon.poll() is None, "daemon exited"
                time.sleep(0.05)
            assert socket_path.exists(), "daemon did not start"
            url = f"http://127.0.0.1:{origin.server_address[1]}/file.bin"
            offer_id = offer_download(socket_path, "popup-smoke", url)
            assert offer_id > 0
            ready_read, ready_write = os.pipe()
            environment["CDM_BROWSER_READY_FD"] = str(ready_write)
            popup = subprocess.Popen([cdm, "browser-popup", "--offer", str(offer_id)],
                                     stdin=subprocess.DEVNULL,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE,
                                     env=environment, pass_fds=(ready_write,))
            os.close(ready_write)
            ready, _, _ = select.select([ready_read], [], [], 5)
            assert ready and os.read(ready_read, 1) == b"R", "popup did not signal readiness"
            os.close(ready_read)
            time.sleep(2)
            assert popup.poll() is None, (
                f"popup exited early: {popup.returncode}; "
                f"stderr={popup.stderr.read().decode(errors='replace')}"
            )
            check_popup_progress_stream(socket_path, root, url)
        finally:
            if popup and popup.poll() is None:
                popup.terminate()
                popup.wait(timeout=3)
            daemon.send_signal(signal.SIGTERM)
            daemon.wait(timeout=3)
            os.close(master)
            origin.shutdown()
            origin.server_close()


if __name__ == "__main__":
    main(sys.argv[1])
