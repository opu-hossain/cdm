#!/usr/bin/env python3
"""Exercise native host -> daemon offer -> confirm -> completed transfer."""

import http.server
import json
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


CONTENT = b"cdm-browser-integration\n" * 4096


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", str(len(CONTENT)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        start, end = 0, len(CONTENT) - 1
        requested = self.headers.get("Range")
        if requested and requested.startswith("bytes="):
            bounds = requested[6:].split("-", 1)
            start = int(bounds[0])
            if bounds[1]:
                end = min(int(bounds[1]), end)
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{len(CONTENT)}")
        else:
            self.send_response(200)
        payload = CONTENT[start : end + 1]
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()
        self.wfile.write(payload)


def read_exact(fd: int, length: int, deadline: float) -> bytes:
    result = b""
    while len(result) < length:
        remaining = deadline - time.monotonic()
        assert remaining > 0, "timed out reading native host"
        ready, _, _ = select.select([fd], [], [], remaining)
        assert ready, "timed out reading native host"
        chunk = os.read(fd, length - len(result))
        assert chunk, "native host closed unexpectedly"
        result += chunk
    return result


def native_reply(host: subprocess.Popen, deadline: float) -> dict:
    fd = host.stdout.fileno()
    length = struct.unpack("=I", read_exact(fd, 4, deadline))[0]
    assert 0 < length <= 1024 * 1024
    return json.loads(read_exact(fd, length, deadline))


def main(cdm: str, native_host: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-browser-flow-") as root:
        root_path = Path(root)
        runtime = root_path / "runtime"
        runtime.mkdir(mode=0o700)
        environment = os.environ.copy()
        environment.update({
            "HOME": root,
            "XDG_RUNTIME_DIR": str(runtime),
            "DOWNLOADMGR_ROOT": root,
            "SDL_VIDEODRIVER": "dummy",  # Popup exits without a display.
        })
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        server_thread = threading.Thread(target=server.serve_forever, daemon=True)
        server_thread.start()
        master, slave = pty.openpty()
        daemon = subprocess.Popen([cdm, "daemon"], stdin=slave,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL,
                                  env=environment)
        os.close(slave)
        host = None
        try:
            socket_path = runtime / "cdm.sock"
            deadline = time.monotonic() + 10
            while not socket_path.exists() and time.monotonic() < deadline:
                assert daemon.poll() is None, "daemon exited during startup"
                time.sleep(0.05)
            assert socket_path.exists(), "daemon did not create IPC socket"

            host = subprocess.Popen([native_host], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, env=environment)
            url = f"http://127.0.0.1:{server.server_port}/sample.bin"
            offer = json.dumps({
                "type": "download_offer", "request_id": "flow-test-1",
                "url": url, "filename": "sample.bin", "browser": "chromium",
                "total_bytes": len(CONTENT),
            }).encode()
            host.stdin.write(struct.pack("=I", len(offer)) + offer)
            host.stdin.flush()
            reply = native_reply(host, time.monotonic() + 10)
            assert reply["type"] == "offer_registered", reply
            offer_id = reply["offer_id"]

            destination = root_path / "sample.bin"
            raw_path = str(destination).encode()
            payload = struct.pack("=II", offer_id, len(raw_path)) + raw_path
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(5)
                client.connect(str(socket_path))
                client.sendall(struct.pack("=II", len(payload), 14) + payload)
                download_id = struct.unpack("=I", client.recv(4))[0]
                assert download_id > 0

            states = []
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                event = native_reply(host, deadline)
                if event["type"] == "error":
                    assert event["error"] == "could not launch cdm popup", event
                    continue  # The test intentionally has no graphical display.
                if event["type"] == "offer_state":
                    states.append(event["state"])
                    if event["state"] in ("complete", "error"):
                        break
            assert "complete" in states, states
            assert destination.read_bytes() == CONTENT
        finally:
            if host:
                host.stdin.close()
                try:
                    host.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    host.kill()
                    host.wait(timeout=3)
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=3)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=3)
            os.close(master)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
