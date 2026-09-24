#!/usr/bin/env python3
"""Verify a browser offer starts the daemon after a fresh login."""

import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys
import tempfile
import time

from test_browser_end_to_end import native_reply


def main(native_host: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-browser-autostart-") as root:
        runtime = Path(root) / "runtime"
        runtime.mkdir(mode=0o700)
        environment = os.environ.copy()
        environment.update({
            "HOME": root,
            "XDG_RUNTIME_DIR": str(runtime),
            "DOWNLOADMGR_ROOT": root,
            "SDL_VIDEODRIVER": "dummy",
        })
        host = subprocess.Popen([native_host], stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                env=environment)
        daemon_pid = None
        try:
            for request_id in ("autostart-test-1", "autostart-test-2"):
                payload = json.dumps({
                    "type": "download_offer", "request_id": request_id,
                    "url": "https://example.org/file.bin", "filename": "file.bin",
                }).encode()
                host.stdin.write(struct.pack("=I", len(payload)) + payload)
                host.stdin.flush()
                deadline = time.monotonic() + 10
                while True:
                    reply = native_reply(host, deadline)
                    if (reply["type"] == "error" and
                            reply.get("error") == "could not launch cdm popup"):
                        continue  # SDL's dummy driver has no popup window.
                    break
                assert reply["type"] == "offer_registered", reply
                assert reply["request_id"] == request_id, reply
            assert (runtime / "cdm.sock").exists()
            log = Path(root) / ".local/share/cdm/daemon.log"
            startup_lines = [line for line in log.read_text().splitlines()
                             if "Daemon started, listening" in line]
            assert len(startup_lines) == 1, startup_lines
            match = re.search(r"pid=(\d+)", startup_lines[0])
            if match:
                daemon_pid = int(match.group(1))
            assert daemon_pid, "could not locate isolated daemon PID"
            status = subprocess.run(
                [str(Path(native_host).with_name("cdm")), "daemon", "status"],
                env=environment, text=True, capture_output=True, check=True,
            )
            assert status.stdout.endswith(f"Daemon: running (pid {daemon_pid})\n")
            assert not status.stderr, status.stderr
            print(f"two browser offers: one daemon startup, pid={daemon_pid}; status agrees")
        finally:
            host.stdin.close()
            try:
                host.wait(timeout=3)
            except subprocess.TimeoutExpired:
                host.kill()
                host.wait(timeout=3)
            if daemon_pid:
                os.kill(daemon_pid, signal.SIGTERM)
                deadline = time.monotonic() + 5
                while (runtime / "cdm.sock").exists() and time.monotonic() < deadline:
                    time.sleep(0.05)


if __name__ == "__main__":
    main(sys.argv[1])
