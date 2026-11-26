#!/usr/bin/env python3
"""Verify repeated CLI headers survive the add request and database storage."""

import http.server
import os
from pathlib import Path
import pty
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "1")
        self.send_header("Content-Disposition", "attachment; filename=server-name.bin")
        self.end_headers()

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", "1")
        self.end_headers()
        self.wfile.write(b"x")


def read_exact(client: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = client.recv(size - len(data))
        assert chunk, "daemon closed IPC response"
        data.extend(chunk)
    return bytes(data)


def read_string(client: socket.socket) -> str:
    length = struct.unpack("=I", read_exact(client, 4))[0]
    return read_exact(client, length).decode()


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-cli-headers-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, HOME=temporary, XDG_RUNTIME_DIR=str(runtime),
                   DOWNLOADMGR_ROOT=temporary)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        master, slave = pty.openpty()
        daemon = subprocess.Popen([cdm, "daemon"], stdin=slave,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, env=env)
        os.close(slave)
        try:
            socket_path = runtime / "cdm.sock"
            deadline = time.monotonic() + 10
            while not socket_path.exists() and time.monotonic() < deadline:
                assert daemon.poll() is None, "daemon exited during startup"
                time.sleep(0.05)
            assert socket_path.exists(), "daemon did not create IPC socket"
            url = f"http://127.0.0.1:{server.server_port}/sample.bin"
            added = subprocess.run(
                [cdm, "cli", "add", url, temporary,
                 "--header", "X-Test: 1", "--header", "X-Test: 1"],
                env=env, capture_output=True, text=True, timeout=10,
            )
            assert added.returncode == 0, added
            match = re.search(r"Download added \(ID: ([1-9][0-9]*)", added.stdout)
            assert match, added
            download_id = int(match.group(1))
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(5)
                client.connect(str(socket_path))
                client.sendall(struct.pack("=III", 4, 10, download_id))
                assert read_exact(client, 1) == b"\x01"
                cookie = read_string(client)
                referrer = read_string(client)
                headers = read_string(client)
                sha256 = read_string(client)
                speed = struct.unpack("=Q", read_exact(client, 8))[0]
            assert cookie == referrer == sha256 == ""
            assert speed == 0
            assert headers == "X-Test: 1\nX-Test: 1", repr(headers)
            final_path = root / "server-name.bin"
            deadline = time.monotonic() + 10
            while not final_path.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            assert final_path.read_bytes() == b"x"
            assert not (root / "sample.bin").exists()
        finally:
            daemon.send_signal(signal.SIGTERM)
            try:
                daemon.wait(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                daemon.wait(timeout=5)
            os.close(master)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main(sys.argv[1])
