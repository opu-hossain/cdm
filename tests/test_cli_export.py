#!/usr/bin/env python3
"""Exercise versioned JSON export through local daemon IPC only."""
import json
import os
from pathlib import Path
import pty
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time


def main(cdm):
    with tempfile.TemporaryDirectory(prefix="cdm-export-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        config_dir = root / ".local/share/cdm"
        config_dir.mkdir(parents=True)
        (config_dir / "config.toml").write_text(
            '[downloads]\nmax_concurrent = 4\n[proxy]\nmode = 1\n'
            'url = "http://127.0.0.1:9"\nusername = "private-user"\n'
            'password = "private-password"\n')
        env = dict(os.environ, HOME=temporary, XDG_RUNTIME_DIR=str(runtime),
                   DOWNLOADMGR_ROOT=temporary)
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
            with sqlite3.connect(config_dir / "downloads.db") as db:
                db.executemany(
                    "INSERT INTO downloads(id,url,dest_path,status,total_size,cookie,auth_password) "
                    "VALUES(?,?,?,?,?,?,?)",
                    [(i, "https://example.invalid/item", str(root / f"item-{i}"),
                      "DONE", (2**63 - 1) if i == 501 else i * 10,
                      "private-cookie", "private-basic")
                     for i in range(1, 502)])
            safe = root / "safe.json"
            result = subprocess.run([cdm, "cli", "export", "--out", str(safe),
                                     "--include-history"], env=env,
                                    capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result
            exported = json.loads(safe.read_text())
            assert exported["version"] == 1
            assert exported["settings"]["downloads"]["max_concurrent"] == 4
            assert len(exported["downloads"]) == 501
            assert exported["downloads"][0]["id"] == 501
            assert exported["downloads"][-1]["id"] == 1
            assert exported["downloads"][0]["total_size_bytes"] == str(2**63 - 1)
            assert "private-cookie" not in safe.read_text()
            assert "private-password" not in safe.read_text()
            assert "private-basic" not in safe.read_text()
            assert safe.stat().st_mode & 0o777 == 0o600
            settings_only = root / "settings.json"
            result = subprocess.run([cdm, "cli", "export", "--out", str(settings_only)],
                                    env=env, capture_output=True, text=True, timeout=15)
            assert result.returncode == 0, result
            assert json.loads(settings_only.read_text())["downloads"] == []
            with sqlite3.connect(config_dir / "downloads.db") as db:
                db.execute("DELETE FROM downloads WHERE id != 1")
            secret = root / "secret.json"
            result = subprocess.run([cdm, "cli", "export", "--out", str(secret),
                                     "--include-history", "--include-secrets"],
                                    env=env, capture_output=True, text=True,
                                    timeout=15)
            assert result.returncode == 0, result
            assert "warning" in result.stderr.lower()
            private = json.loads(secret.read_text())
            assert private["settings"]["proxy"]["password"] == "private-password"
            assert private["downloads"][0]["cookie"] == "private-cookie"
            assert "private-basic" not in secret.read_text()
        finally:
            daemon.send_signal(signal.SIGTERM)
            daemon.wait(timeout=5)
            os.close(master)


if __name__ == "__main__":
    main(sys.argv[1])
