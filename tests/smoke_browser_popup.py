#!/usr/bin/env python3
"""Manual display smoke test for the standalone SDL/Nuklear popup."""

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
import time


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-popup-smoke-") as root:
        runtime = Path(root) / "runtime"
        runtime.mkdir(mode=0o700)
        environment = os.environ.copy()
        environment.update({"HOME": root, "XDG_RUNTIME_DIR": str(runtime)})
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
            offer = json.dumps({
                "request_id": "popup-smoke", "url": "https://example.org/file.bin",
                "filename": "file.bin", "total_bytes": 1024,
            }).encode()
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
                client.settimeout(3)
                client.connect(str(socket_path))
                client.sendall(struct.pack("=II", len(offer), 12) + offer)
                offer_id = struct.unpack("=I", client.recv(4))[0]
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
        finally:
            if popup and popup.poll() is None:
                popup.terminate()
                popup.wait(timeout=3)
            daemon.send_signal(signal.SIGTERM)
            daemon.wait(timeout=3)
            os.close(master)


if __name__ == "__main__":
    main(sys.argv[1])
