#!/usr/bin/env python3
"""Exercise daemon lifecycle output, singleton startup, and stale socket recovery."""

import os
import pty
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main() -> None:
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="cdm-daemon-lifecycle-") as temporary:
        root = Path(temporary)
        home = root / "home"
        runtime = root / "runtime"
        home.mkdir()
        runtime.mkdir()
        env = dict(os.environ, HOME=str(home), XDG_RUNTIME_DIR=str(runtime))
        for _key in (
            "XDG_CONFIG_HOME",
            "XDG_DATA_HOME",
            "XDG_STATE_HOME",
            "XDG_CACHE_HOME",
            "XDG_CONFIG_DIRS",
        ):
            env.pop(_key, None)
        config_dir = root / "etc" / "xdg" / "autostart"
        config_dir.mkdir(parents=True)
        (config_dir / "cdm-daemon.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=CDM\n"
            f"Exec={binary} daemon\nTryExec={binary}\n"
        )
        env["XDG_CONFIG_DIRS"] = str(config_dir.parent)
        override = home / ".config" / "autostart" / "cdm-daemon.desktop"

        def command(*actions: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                [binary, "daemon", *actions],
                env=env,
                text=True,
                capture_output=True,
            )

        initial = command("status")
        assert initial.returncode == 0 and not initial.stderr, initial
        assert initial.stdout.endswith("Daemon: not running\n"), initial
        assert "Autostart: enabled\n" in initial.stdout
        print(
            f"status stopped: exit={initial.returncode}, stdout={initial.stdout.rstrip()!r}, stderr={initial.stderr!r}"
        )
        disabled = command("disable")
        assert (disabled.returncode, disabled.stdout, disabled.stderr) == (
            0,
            f"Autostart: disabled (wrote {override})\n",
            "",
        ), disabled
        print(
            f"disable: exit={disabled.returncode}, stdout={disabled.stdout.rstrip()!r}, stderr={disabled.stderr!r}"
        )
        disabled = command("disable")
        assert (disabled.returncode, disabled.stdout, disabled.stderr) == (
            0,
            f"Autostart: disabled (updated {override})\n",
            "",
        ), disabled
        assert "Autostart: disabled" in command("status").stdout
        assert "Hidden=true" in override.read_text()
        enabled = command("enable")
        assert (enabled.returncode, enabled.stdout, enabled.stderr) == (
            0,
            f"Autostart: enabled (removed {override})\n",
            "",
        ), enabled
        print(
            f"enable: exit={enabled.returncode}, stdout={enabled.stdout.rstrip()!r}, stderr={enabled.stderr!r}"
        )
        enabled = command("enable")
        assert (enabled.returncode, enabled.stdout, enabled.stderr) == (
            0,
            f"Autostart: enabled (using {config_dir / 'cdm-daemon.desktop'}; no file changed)\n",
            "",
        ), enabled
        assert "Autostart: enabled" in command("status").stdout
        assert not override.exists()
        override.write_text(
            "[Desktop Entry]\nType=Application\nName=Custom\n"
            f"Exec={binary} daemon\nHidden=false\nX-Custom=keep\n"
        )
        assert f"updated {override}" in command("disable").stdout
        assert f"updated {override}" in command("enable").stdout
        assert "X-Custom=keep" in override.read_text()
        assert "Hidden=false" in override.read_text()
        override.write_text("[Desktop Entry]\nHidden=true\nX-Custom=keep\n")
        assert f"updated {override}" in command("enable").stdout
        assert "Autostart: enabled" in command("status").stdout
        assert "X-Custom=keep" in override.read_text()
        assert "Exec=" in override.read_text()
        override.unlink()
        (config_dir / "cdm-daemon.desktop").write_text(
            "[Desktop Entry]\nType=Application\nName=CDM\n"
            f"Exec={binary} daemon\nHidden=true\n"
        )
        assert f"wrote {override}" in command("enable").stdout
        assert "Autostart: enabled" in command("status").stdout
        assert override.exists()
        assert not (home / ".local" / "share" / "cdm").exists()
        invalid = command("start")
        assert (invalid.returncode, invalid.stdout, invalid.stderr) == (
            2,
            "",
            "Usage: cdm daemon [enable|disable|status]\n",
        ), invalid
        print(
            f"unknown start: exit={invalid.returncode}, stdout={invalid.stdout!r}, stderr={invalid.stderr.rstrip()!r}"
        )
        master, slave = pty.openpty()
        processes = []
        try:
            for _ in range(8):
                processes.append(
                    subprocess.Popen(
                        [binary, "daemon"],
                        env=env,
                        stdin=slave,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                    )
                )
            socket_path = runtime / "cdm.sock"
            deadline = time.monotonic() + 10
            while not socket_path.exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            assert socket_path.exists(), "daemon did not create IPC socket"
            time.sleep(0.5)
            active = [process for process in processes if process.poll() is None]
            assert len(active) == 1, f"expected one daemon, got {len(active)}"
            serving_pid = active[0].pid
            running = command("status")
            assert running.returncode == 0 and not running.stderr, running
            assert running.stdout.endswith(
                f"Daemon: running (pid {serving_pid})\n"
            ), running
            print(
                f"foreground daemon: pid={serving_pid}, alive={active[0].poll() is None}"
            )
            print(
                f"status running: exit={running.returncode}, stdout={running.stdout.rstrip()!r}, stderr={running.stderr!r}"
            )
            duplicate = command()
            assert (duplicate.returncode, duplicate.stdout, duplicate.stderr) == (
                1,
                "",
                f"cdm daemon: already running (pid {serving_pid})\n",
            ), duplicate
            print(
                f"duplicate daemon: exit={duplicate.returncode}, stdout={duplicate.stdout!r}, stderr={duplicate.stderr.rstrip()!r}"
            )
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(str(socket_path))
            assert (runtime / "cdm.lock").stat().st_mode & 0o777 == 0o600
            lock_inode = (runtime / "cdm.lock").stat().st_ino
            active[0].kill()
            assert active[0].wait(timeout=5) == -signal.SIGKILL
            assert socket_path.exists(), "stale socket pathname disappeared"
            assert (runtime / "cdm.lock").exists(), "lock file disappeared"
            replacement = subprocess.Popen(
                [binary, "daemon"],
                env=env,
                stdin=slave,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            processes.append(replacement)
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                status = command("status")
                if status.stdout.endswith(f"Daemon: running (pid {replacement.pid})\n"):
                    break
                time.sleep(0.05)
            assert replacement.poll() is None, "replacement daemon exited"
            assert status.returncode == 0 and not status.stderr, status
            assert status.stdout.endswith(
                f"Daemon: running (pid {replacement.pid})\n"
            ), status
            print(
                f"after SIGKILL: stale socket and lock retained; replacement pid={replacement.pid}, alive={replacement.poll() is None}"
            )
            assert (runtime / "cdm.lock").stat().st_ino == lock_inode
            with socket.socket(socket.AF_UNIX) as client:
                client.connect(str(socket_path))
            print("singleton PID reported; duplicate rejected; stale socket recovered")
        finally:
            for process in processes:
                if process.poll() is None:
                    process.terminate()
            for process in processes:
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
            os.close(master)
            os.close(slave)
        stopped = command("status")
        assert stopped.returncode == 0 and not stopped.stderr, stopped
        assert stopped.stdout.endswith("Daemon: not running\n"), stopped
        print(
            f"status after stop: exit={stopped.returncode}, stdout={stopped.stdout.rstrip()!r}, stderr={stopped.stderr!r}"
        )

    with tempfile.TemporaryDirectory(prefix="cdm-daemon-migration-") as temporary:
        home = Path(temporary) / "home"
        old = home / ".local" / "share" / "downloadmgr"
        old.mkdir(parents=True)
        (old / "migration-marker").write_text("preserve")
        env = dict(os.environ, HOME=str(home))
        for _key in (
            "XDG_RUNTIME_DIR",
            "XDG_CONFIG_HOME",
            "XDG_DATA_HOME",
            "XDG_STATE_HOME",
            "XDG_CACHE_HOME",
            "XDG_CONFIG_DIRS",
        ):
            env.pop(_key, None)
        master, slave = pty.openpty()
        process = subprocess.Popen(
            [binary, "daemon"],
            env=env,
            stdin=slave,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            new = home / ".local" / "share" / "cdm"
            deadline = time.monotonic() + 10
            while not (new / "ipc.sock").exists() and time.monotonic() < deadline:
                time.sleep(0.05)
            assert (new / "migration-marker").read_text() == "preserve"
            assert not old.exists()
            assert (home / ".local" / "share" / "cdm.lock").exists()
            assert "Migrated data directory" in (new / "daemon.log").read_text()
            print("legacy data migrated before fallback IPC startup")
        finally:
            process.terminate()
            process.wait(timeout=5)
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    main()
