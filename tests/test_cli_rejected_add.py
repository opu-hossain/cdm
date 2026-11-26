#!/usr/bin/env python3
"""A daemon-rejected add must fail at the CLI boundary."""

import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import tempfile
import time


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-cli-root-") as allowed_root:
        with tempfile.TemporaryDirectory(prefix="cdm-cli-outside-") as outside:
            runtime = Path(allowed_root) / "runtime"
            runtime.mkdir(mode=0o700)
            env = dict(os.environ, HOME=allowed_root,
                       XDG_RUNTIME_DIR=str(runtime),
                       DOWNLOADMGR_ROOT=allowed_root)
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

                result = subprocess.run(
                    [cdm, "cli", "add",
                     "http://127.0.0.1:1/rejected.bin", outside],
                    env=env, capture_output=True, text=True, timeout=10,
                )
                assert result.returncode != 0, result
                assert "Download added" not in result.stdout, result
                assert result.stderr, result
            finally:
                daemon.send_signal(signal.SIGTERM)
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait(timeout=5)
                os.close(master)


if __name__ == "__main__":
    main(sys.argv[1])
