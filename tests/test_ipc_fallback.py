#!/usr/bin/env python3
"""Check HOME socket fallback, including an unusably long runtime path."""

import os
from pathlib import Path
import pty
import signal
import subprocess
import sys
import tempfile
import time


def check_home_fallback(cdm: str, home: Path, runtime: Path | None) -> None:
    env = dict(os.environ, HOME=str(home))
    if runtime is None:
        env.pop("XDG_RUNTIME_DIR", None)
    else:
        env["XDG_RUNTIME_DIR"] = str(runtime)
    master, slave = pty.openpty()
    daemon = subprocess.Popen([cdm, "daemon"], stdin=slave,
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, env=env)
    os.close(slave)
    try:
        socket_path = home / ".local" / "share" / "cdm" / "ipc.sock"
        lock_path = home / ".local" / "share" / "cdm.lock"
        deadline = time.monotonic() + 5
        while not socket_path.exists() and time.monotonic() < deadline:
            assert daemon.poll() is None, "daemon exited before HOME fallback"
            time.sleep(0.05)
        assert socket_path.is_socket(), "HOME fallback socket missing"
        assert lock_path.is_file(), "HOME fallback lock missing"
    finally:
        if daemon.poll() is None:
            daemon.send_signal(signal.SIGTERM)
        try:
            daemon.wait(timeout=5)
        except subprocess.TimeoutExpired:
            daemon.kill()
            daemon.wait(timeout=5)
        os.close(master)


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-ipc-fallback-") as temporary:
        root = Path(temporary)
        home = root / "home"
        home.mkdir()
        check_home_fallback(cdm, home, None)

        long_runtime = root / ("r" * 90)
        long_runtime.mkdir()
        check_home_fallback(cdm, home, long_runtime)


if __name__ == "__main__":
    main(sys.argv[1])
