#!/usr/bin/env python3
"""The CLI reports an active duplicate with its existing download ID."""

import http.server
import os
from pathlib import Path
import pty
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time


class SlowHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Length", "100000000")
        self.send_header("Accept-Ranges", "bytes")
        self.end_headers()

    def do_GET(self):
        self.send_response(206)
        self.send_header("Content-Length", "100000000")
        self.send_header("Content-Range", "bytes 0-99999999/100000000")
        self.end_headers()
        time.sleep(5)

    def log_message(self, *_args):
        pass


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-duplicate-cli-") as root:
        runtime = Path(root) / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, HOME=root, XDG_RUNTIME_DIR=str(runtime),
                   DOWNLOADMGR_ROOT=root)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SlowHandler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        master, slave = pty.openpty()
        daemon = subprocess.Popen([cdm, "daemon"], stdin=slave,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, env=env)
        os.close(slave)
        try:
            deadline = time.monotonic() + 10
            while not (runtime / "cdm.sock").exists() and time.monotonic() < deadline:
                assert daemon.poll() is None, "daemon exited during startup"
                time.sleep(0.05)
            assert (runtime / "cdm.sock").exists()
            url = f"http://127.0.0.1:{server.server_port}/file.bin"
            first = subprocess.run([cdm, "cli", "add", url, root], env=env,
                                   capture_output=True, text=True, timeout=10)
            assert first.returncode == 0, first
            match = re.search(r"Download added \(ID: (\d+)", first.stdout)
            assert match, first
            second = subprocess.run([cdm, "cli", "add", url + "#again", root],
                                    env=env, capture_output=True, text=True,
                                    timeout=10)
            assert second.returncode == 0, second
            assert f"Already downloading (ID {match.group(1)})" in second.stdout, second
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
