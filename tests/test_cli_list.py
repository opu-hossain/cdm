#!/usr/bin/env python3
"""Exercise paged CLI history and recorded total size against a local daemon."""

import os
from pathlib import Path
import pty
import signal
import sqlite3
import subprocess
import sys
import tempfile
import time


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-cli-list-") as temporary:
        root = Path(temporary)
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        env = dict(os.environ, HOME=temporary, XDG_RUNTIME_DIR=str(runtime),
                   DOWNLOADMGR_ROOT=temporary)
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

            database = root / ".local/share/cdm/downloads.db"
            with sqlite3.connect(database) as db:
                db.executemany(
                    "INSERT INTO downloads(id,url,dest_path,status,total_size) "
                    "VALUES(?,?,?,?,?)",
                    [(i, "http://127.0.0.1/item", str(root / f"item-{i}"),
                      "DONE" if i % 2 == 0 else "ERROR", i * 10)
                     for i in range(1, 601)],
                )

            listed = subprocess.run(
                [cdm, "cli", "list", "--offset", "500", "--limit", "100"],
                env=env, capture_output=True, text=True, timeout=15,
            )
            assert listed.returncode == 0, listed
            rows = [line.split("\t") for line in listed.stdout.splitlines()]
            assert rows[0] == ["id", "status", "percent", "size", "filename"]
            assert len(rows) == 101
            for index, row in enumerate(rows[1:]):
                ident = 100 - index
                assert row == [str(ident), "DONE" if ident % 2 == 0 else "ERROR",
                               "100%" if ident % 2 == 0 else "0%",
                               str(ident * 10), f"item-{ident}"], row

            filtered = subprocess.run(
                [cdm, "cli", "list", "--status", "DONE", "--offset", "250",
                 "--limit", "50"],
                env=env, capture_output=True, text=True, timeout=15,
            )
            assert filtered.returncode == 0, filtered
            rows = [line.split("\t") for line in filtered.stdout.splitlines()]
            assert len(rows) == 51
            assert [int(row[0]) for row in rows[1:]] == list(range(100, 0, -2))
            assert all(row[1] == "DONE" for row in rows[1:])
        finally:
            daemon.send_signal(signal.SIGTERM)
            daemon.wait(timeout=5)
            os.close(master)


if __name__ == "__main__":
    main(sys.argv[1])
