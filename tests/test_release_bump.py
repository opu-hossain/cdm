"""Release bumps must keep shipped version references together."""

import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile


SOURCE = pathlib.Path(__file__).resolve().parents[1]
FILES = (
    ".release.toml",
    "scripts/cdm-release",
    "scripts/sync-release-version.py",
    "packaging/arch/PKGBUILD",
    "browser/chromium/manifest.json",
    "browser/firefox/manifest.json",
    "README.md",
    "CONTRIBUTING.md",
    "docs/RELEASE_NOTES.md",
    "docs/RELEASE_CHECKLIST.md",
)


def fixture(root):
    for name in FILES:
        source = SOURCE / name
        if not source.exists():
            continue
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def bump(root, *args, success=True):
    result = subprocess.run(
        ["bash", "scripts/cdm-release", "bump", *args],
        cwd=root, text=True, capture_output=True, check=False,
    )
    assert (result.returncode == 0) == success, result.stdout + result.stderr


def check_versions(root, tag, package, base):
    read = lambda name: (root / name).read_text(encoding="utf-8")
    expected_pkgver = tag.replace("-rc", "rc")
    assert re.search(rf"^pkgver={re.escape(expected_pkgver)}$", read("packaging/arch/PKGBUILD"), re.M)
    assert "pkgrel=1" in read("packaging/arch/PKGBUILD")
    assert "_upstream_version=\"${pkgver/rc/-rc}\"" in read("packaging/arch/PKGBUILD")
    for browser in ("chromium", "firefox"):
        manifest = json.loads(read(f"browser/{browser}/manifest.json"))
        assert manifest["version"] == base
    for name in ("README.md", "CONTRIBUTING.md", "docs/RELEASE_NOTES.md",
                 "docs/RELEASE_CHECKLIST.md"):
        assert tag in read(name), name
    assert package in read("docs/RELEASE_NOTES.md")
    assert package in read("docs/RELEASE_CHECKLIST.md")
    assert f"v{tag}" in read("docs/RELEASE_CHECKLIST.md")


def main():
    with tempfile.TemporaryDirectory(prefix="cdm-release-bump-") as temp:
        root = pathlib.Path(temp)
        fixture(root)
        bump(root, "rc", "2")
        check_versions(root, "0.3.0-rc2", "0.3.0~rc2", "0.3.0")
        assert "- [x]" not in (root / "docs/RELEASE_CHECKLIST.md").read_text()
        bump(root, "rc")
        check_versions(root, "0.3.0-rc3", "0.3.0~rc3", "0.3.0")
        bump(root, "stable")
        check_versions(root, "0.3.0", "0.3.0", "0.3.0")
        bump(root, "rcreset")
        check_versions(root, "0.3.0-rc1", "0.3.0~rc1", "0.3.0")
        bump(root, "stable")
        bump(root, "patch")
        check_versions(root, "0.3.1-rc1", "0.3.1~rc1", "0.3.1")
        bump(root, "minor")
        check_versions(root, "0.4.0-rc1", "0.4.0~rc1", "0.4.0")
        bump(root, "major")
        check_versions(root, "1.0.0-rc1", "1.0.0~rc1", "1.0.0")
        bump(root, "rc", "3")
        bump(root, "rcreset")
        check_versions(root, "1.0.0-rc1", "1.0.0~rc1", "1.0.0")
        before = (root / ".release.toml").read_bytes()
        bump(root, "rc", "0", success=False)
        assert (root / ".release.toml").read_bytes() == before
        (root / "docs/RELEASE_NOTES.md").unlink()
        bump(root, "patch", success=False)
        assert (root / ".release.toml").read_bytes() == before


if __name__ == "__main__":
    try:
        main()
    except AssertionError as error:
        print(error, file=sys.stderr)
        raise
