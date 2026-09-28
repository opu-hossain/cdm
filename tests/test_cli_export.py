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
            'url = "http://127.0.0.1:9"\nusername = "synthetic-user"\n'
            'password = "synthetic-password"\n')
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
                      "QUEUED" if i == 500 else "DONE",
                      (2**63 - 1) if i == 501 else i * 10,
                      "synthetic-cookie", "synthetic-basic")
                     for i in range(1, 502)])
                db.execute("UPDATE downloads SET media_kind=1, requires_browser_context=1 WHERE id=500")
                db.execute("UPDATE downloads SET site_grab=1 WHERE id=499")
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
            assert exported["downloads"][1]["media_kind"] == 1, exported["downloads"][1]
            assert exported["downloads"][1]["requires_browser_context"] is True
            assert exported["downloads"][2]["site_grab"] is True
            assert "synthetic-cookie" not in safe.read_text()
            assert "synthetic-password" not in safe.read_text()
            assert "synthetic-basic" not in safe.read_text()
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
            assert private["settings"]["proxy"]["password"] == "synthetic-password"
            assert private["downloads"][0]["cookie"] == "synthetic-cookie"
            assert "synthetic-basic" not in secret.read_text()
            for name, changed in (
                ("future", {**exported, "version": 99}),
                ("unknown", {**exported, "surprise": 1}),
            ):
                source = root / f"{name}.json"
                source.write_text(json.dumps(changed))
                rejected = subprocess.run([cdm, "cli", "import", "--in", str(source),
                                           "--merge"], env=env, capture_output=True,
                                          text=True, timeout=15)
                assert rejected.returncode != 0, rejected
            invalid_settings = json.loads(safe.read_text())
            invalid_settings["settings"]["downloads"]["default_directory"] = "/outside"
            invalid_file = root / "invalid-settings.json"
            invalid_file.write_text(json.dumps(invalid_settings))
            rejected = subprocess.run([cdm, "cli", "import", "--in", str(invalid_file),
                                       "--replace", "--yes"], env=env,
                                      capture_output=True, text=True, timeout=15)
            assert rejected.returncode != 0, rejected
            assert not list(config_dir.glob("downloads.db.backup.*"))
            invalid_media = json.loads(safe.read_text())
            invalid_media["downloads"][0]["media_kind"] = 99
            invalid_file.write_text(json.dumps(invalid_media))
            rejected = subprocess.run([cdm, "cli", "import", "--in", str(invalid_file),
                                       "--replace", "--yes"], env=env,
                                      capture_output=True, text=True, timeout=15)
            assert rejected.returncode != 0, rejected
            assert not list(config_dir.glob("downloads.db.backup.*"))
            with sqlite3.connect(config_dir / "downloads.db") as db:
                assert db.execute("SELECT count(*) FROM downloads").fetchone()[0] == 1
            invalid_path = json.loads(safe.read_text())
            invalid_path["downloads"][0]["dest_path"] = "/outside/item"
            invalid_file.write_text(json.dumps(invalid_path))
            rejected = subprocess.run([cdm, "cli", "import", "--in", str(invalid_file),
                                       "--replace", "--yes"], env=env,
                                      capture_output=True, text=True, timeout=15)
            assert rejected.returncode != 0, rejected
            assert not list(config_dir.glob("downloads.db.backup.*"))
            invalid_status = json.loads(safe.read_text())
            invalid_status["downloads"][0]["status"] = "RUNNING"
            invalid_file.write_text(json.dumps(invalid_status))
            rejected = subprocess.run([cdm, "cli", "import", "--in", str(invalid_file),
                                       "--replace", "--yes"], env=env,
                                      capture_output=True, text=True, timeout=15)
            assert rejected.returncode != 0, rejected
            assert not list(config_dir.glob("downloads.db.backup.*"))
            confirmed = subprocess.run([cdm, "cli", "import", "--in", str(safe),
                                        "--replace"], env=env, capture_output=True,
                                       text=True, timeout=15)
            assert confirmed.returncode != 0
            merged = subprocess.run([cdm, "cli", "import", "--in", str(safe),
                                     "--merge"], env=env, capture_output=True,
                                    text=True, timeout=15)
            assert merged.returncode == 0, merged
            with sqlite3.connect(config_dir / "downloads.db") as db:
                assert db.execute("SELECT count(*) FROM downloads").fetchone()[0] == 501
                assert db.execute("SELECT status FROM downloads WHERE id=500").fetchone()[0] == "PAUSED"
                assert db.execute("SELECT media_kind, requires_browser_context FROM downloads WHERE id=500").fetchone() == (1, 1)
                assert db.execute("SELECT site_grab FROM downloads WHERE id=499").fetchone()[0] == 1
                assert db.execute("SELECT cookie FROM downloads WHERE id=1").fetchone()[0] == "synthetic-cookie"
                db.execute("INSERT INTO downloads(id,url,dest_path,status) VALUES(?,?,?,?)",
                           (999, "https://example.invalid/extra", str(root / "extra"), "DONE"))
            replacement = json.loads(safe.read_text())
            replacement["settings"]["downloads"]["max_concurrent"] = 5
            replace_file = root / "replace.json"
            replace_file.write_text(json.dumps(replacement))
            replaced = subprocess.run([cdm, "cli", "import", "--in", str(replace_file),
                                       "--replace", "--yes"], env=env,
                                      capture_output=True, text=True, timeout=15)
            assert replaced.returncode == 0, replaced
            backups = list(config_dir.glob("downloads.db.backup.*"))
            assert len(backups) == 1
            with sqlite3.connect(backups[0]) as db:
                assert db.execute("SELECT count(*) FROM downloads").fetchone()[0] == 502
            with sqlite3.connect(config_dir / "downloads.db") as db:
                assert db.execute("SELECT count(*) FROM downloads").fetchone()[0] == 501
            assert "max_concurrent = 5" in (config_dir / "config.toml").read_text()
        finally:
            daemon.send_signal(signal.SIGTERM)
            daemon.wait(timeout=5)
            os.close(master)


if __name__ == "__main__":
    main(sys.argv[1])
