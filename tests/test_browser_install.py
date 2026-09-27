#!/usr/bin/env python3
"""Check per-user native messaging registration without touching a real profile."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


CHROMIUM_ID = "abcdefghijklmnopabcdefghijklmnop"


def run(cdm: str, environment: dict[str, str], *args: str) -> subprocess.CompletedProcess:
    return subprocess.run([cdm, "browser", *args], env=environment,
                          capture_output=True, text=True, timeout=5)


def main(cdm: str) -> None:
    with tempfile.TemporaryDirectory(prefix="cdm-browser-install-") as root:
        environment = os.environ.copy()
        environment["HOME"] = root
        cases = [
            ("--chrome", Path(root) / ".config/google-chrome/NativeMessagingHosts/org.cdm.browser.json"),
            ("--chromium", Path(root) / ".config/chromium/NativeMessagingHosts/org.cdm.browser.json"),
            ("--firefox", Path(root) / ".mozilla/native-messaging-hosts/org.cdm.browser.json"),
        ]
        additional = [
            ("--edge", "microsoft-edge", "microsoft-edge"),
            ("--brave", "BraveSoftware/Brave-Browser", "BraveSoftware/Brave-Browser"),
            ("--opera", "opera", "google-chrome"),
            ("--vivaldi", "vivaldi", "vivaldi"),
        ]
        for browser, profile, registry in additional:
            profile_path = Path(root) / ".config" / profile
            result = run(cdm, environment, "install", browser, "--id", CHROMIUM_ID)
            assert result.returncode != 0, result
            assert "config directory" in result.stderr, result.stderr
            assert not profile_path.exists(), "installer created a missing browser profile"
            profile_path.parent.mkdir(parents=True, exist_ok=True)
            profile_path.write_text("not a directory")
            result = run(cdm, environment, "install", browser, "--id", CHROMIUM_ID)
            assert result.returncode != 0
            assert "config directory" in result.stderr
            profile_path.unlink()
            profile_path.mkdir()
            result = run(cdm, environment, "install", browser, "--id", "invalid")
            assert result.returncode != 0
            assert "32 letters a-p" in result.stderr
            cases.append((browser, Path(root) / ".config" / registry /
                          "NativeMessagingHosts/org.cdm.browser.json"))
        for browser, manifest_path in cases:
            extension_id = "browser@cdm.local" if browser == "--firefox" else CHROMIUM_ID
            result = run(cdm, environment, "install", browser, "--id", extension_id)
            assert result.returncode == 0, result.stderr
            if browser == "--opera":
                assert "shares Chrome" in result.stderr
                assert not (Path(root) / ".config/opera/NativeMessagingHosts").exists()
            manifest = json.loads(manifest_path.read_text())
            assert manifest["name"] == "org.cdm.browser"
            assert manifest["type"] == "stdio"
            assert Path(manifest["path"]).name == "cdm_native_host"
            if browser == "--firefox":
                assert manifest["allowed_extensions"] == [extension_id]
            else:
                assert manifest["allowed_origins"] == [f"chrome-extension://{extension_id}/"]
            result = run(cdm, environment, "uninstall", browser)
            assert result.returncode == 0, result.stderr
            assert not manifest_path.exists()
            # Uninstall may only remove a manifest owned by cdm.
            manifest_path.write_text(json.dumps({"name": "org.other.host",
                                                 "path": "/tmp/other-host"}))
            result = run(cdm, environment, "uninstall", browser)
            assert result.returncode != 0
            assert manifest_path.exists()
            manifest_path.unlink()
            result = run(cdm, environment, "uninstall", browser)
            assert result.returncode == 0
        result = run(cdm, environment, "install", "--chrome", "--id", "invalid")
        assert result.returncode != 0


if __name__ == "__main__":
    main(sys.argv[1])
