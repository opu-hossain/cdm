#!/usr/bin/env python3
"""Batch add reads one local URL per line and reports each result."""

import http.server
import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import tempfile
import threading
import time


class Handler(http.server.BaseHTTPRequestHandler):
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
        time.sleep(3)

    def log_message(self, *_args):
        pass


def main(cdm):
    with tempfile.TemporaryDirectory(prefix="cdm-cli-batch-") as root:
        runtime = Path(root) / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, HOME=root, XDG_RUNTIME_DIR=str(runtime),
                   DOWNLOADMGR_ROOT=root)
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        master, slave = pty.openpty()
        daemon = subprocess.Popen([cdm, "daemon"], stdin=slave,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL, env=env)
        os.close(slave)
        try:
            deadline = time.monotonic() + 10
            while not (runtime / "cdm.sock").exists() and time.monotonic() < deadline:
                assert daemon.poll() is None
                time.sleep(0.05)
            assert (runtime / "cdm.sock").exists()
            fixture = Path(root) / "batch.txt"
            fixture.write_text(
                f"# comment\n\nhttp://127.0.0.1:{server.server_port}/one\n"
                f"http://127.0.0.1:{server.server_port}/two\n"
            )
            result = subprocess.run([cdm, "cli", "add", "--file",
                                     str(fixture), root], env=env,
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result
            assert result.stdout.count("Download added") == 2, result
            fixture.write_text("not-a-url\n")
            rejected = subprocess.run([cdm, "cli", "add", "--file",
                                       str(fixture), root], env=env,
                                      capture_output=True, text=True,
                                      timeout=10)
            assert rejected.returncode == 1, rejected
            assert "Line 1: failed" in rejected.stdout, rejected
            missing = subprocess.run([cdm, "cli", "add", "--file",
                                      str(Path(root) / "missing.txt")], env=env,
                                     capture_output=True, text=True, timeout=10)
            assert missing.returncode == 2, missing
        finally:
            daemon.send_signal(signal.SIGTERM)
            daemon.wait(timeout=5)
            os.close(master)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main(sys.argv[1])
