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


def starting_version():
    """Read the base version so this test survives future bumps."""
    text = (SOURCE / ".release.toml").read_text(encoding="utf-8")
    match = re.search(r'^version\s*=\s*["\']?([0-9]+\.[0-9]+\.[0-9]+)',
                      text, re.MULTILINE)
    assert match, ".release.toml: missing base version"
    return match.group(1)


def next_version(base, kind):
    major, minor, patch = map(int, base.split("."))
    if kind == "major":
        return f"{major + 1}.0.0"
    if kind == "minor":
        return f"{major}.{minor + 1}.0"
    if kind == "patch":
        return f"{major}.{minor}.{patch + 1}"
    raise ValueError(kind)


def main():
    base = starting_version()
    patch_base = next_version(base, "patch")
    minor_base = next_version(base, "minor")
    major_base = next_version(base, "major")
    with tempfile.TemporaryDirectory(prefix="cdm-release-bump-") as temp:
        root = pathlib.Path(temp)
        fixture(root)
        bump(root, "rc", "2")
        check_versions(root, f"{base}-rc2", f"{base}~rc2", base)
        assert "- [x]" not in (root / "docs/RELEASE_CHECKLIST.md").read_text()
        bump(root, "rc")
        check_versions(root, f"{base}-rc3", f"{base}~rc3", base)
        bump(root, "stable")
        check_versions(root, base, base, base)
        bump(root, "rcreset")
        check_versions(root, f"{base}-rc1", f"{base}~rc1", base)
        bump(root, "stable")
        bump(root, "patch")
        check_versions(root, f"{patch_base}-rc1", f"{patch_base}~rc1", patch_base)
        bump(root, "minor")
        check_versions(root, f"{minor_base}-rc1", f"{minor_base}~rc1", minor_base)
        bump(root, "major")
        check_versions(root, f"{major_base}-rc1", f"{major_base}~rc1", major_base)
        bump(root, "rc", "3")
        bump(root, "rcreset")
        check_versions(root, f"{major_base}-rc1", f"{major_base}~rc1", major_base)
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
