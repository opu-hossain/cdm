#!/usr/bin/env python3
"""Apply one release bump to the metadata and its checked-in references."""

import os
from pathlib import Path
import re
import sys
import tempfile


ROOT = Path(__file__).resolve().parent.parent
DOCS = ("README.md", "CONTRIBUTING.md", "docs/RELEASE_NOTES.md",
        "docs/RELEASE_CHECKLIST.md")


def one(text, pattern, replacement, name):
    changed, count = re.subn(pattern, replacement, text, count=1, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"{name}: expected one matching version field")
    return changed


def field(text, key, pattern):
    match = re.search(rf"^{key}\s*=\s*{pattern}\s*$", text, re.MULTILINE)
    if not match:
        raise ValueError(f".release.toml: missing or invalid {key}")
    return match.group(1)


def tag(channel, version, rc):
    return f"{version}-rc{rc}" if channel == "rc" else version


def replace_tag(text, old, new, name):
    if old != new:
        if old not in text:
            raise ValueError(f"{name}: current version {old} is missing")
        return text.replace(old, new)
    return text


def bump(kind, requested=None):
    paths = (".release.toml", "packaging/arch/PKGBUILD",
             "browser/chromium/manifest.json", "browser/firefox/manifest.json",
             *DOCS)
    original = {name: (ROOT / name).read_text(encoding="utf-8") for name in paths}
    current = original[".release.toml"]
    channel = field(current, "channel", r'"(rc|stable)"')
    version = field(current, "version", r'"([0-9]+\.[0-9]+\.[0-9]+)"')
    rc = int(field(current, "rc", r"([0-9]+)"))
    old_tag = tag(channel, version, rc)
    old_package = old_tag.replace("-rc", "~rc")

    if kind == "rc":
        if requested is not None and (not requested.isascii() or
                                      not requested.isdecimal() or int(requested) < 1):
            raise ValueError("RC number must be a positive decimal integer")
        rc = int(requested) if requested is not None else rc + 1
        channel = "rc"
    elif kind == "rcreset":
        rc, channel = 1, "rc"
    elif kind == "stable":
        channel = "stable"
    elif kind in ("major", "minor", "patch"):
        major, minor, patch = map(int, version.split("."))
        if kind == "major":
            major, minor, patch = major + 1, 0, 0
        elif kind == "minor":
            minor, patch = minor + 1, 0
        else:
            patch += 1
        version, rc, channel = f"{major}.{minor}.{patch}", 1, "rc"
    else:
        raise ValueError("usage: bump rc [N] | rcreset | stable | patch | minor | major")
    if kind != "rc" and requested is not None:
        raise ValueError(f"{kind} does not accept an RC number")

    new_tag = tag(channel, version, rc)
    new_package = new_tag.replace("-rc", "~rc")
    updated = dict(original)
    updated[".release.toml"] = one(current, r"^channel\s*=.*$",
                                   f'channel = "{channel}"', ".release.toml")
    updated[".release.toml"] = one(updated[".release.toml"], r"^version\s*=.*$",
                                   f'version = "{version}"', ".release.toml")
    updated[".release.toml"] = one(updated[".release.toml"], r"^rc\s*=.*$",
                                   f"rc = {rc}", ".release.toml")

    pkg = original["packaging/arch/PKGBUILD"]
    updated["packaging/arch/PKGBUILD"] = one(
        pkg, r"^pkgver=.*$", f'pkgver={new_tag.replace("-rc", "rc")}', "PKGBUILD")
    updated["packaging/arch/PKGBUILD"] = one(
        updated["packaging/arch/PKGBUILD"], r"^pkgrel=.*$", "pkgrel=1", "PKGBUILD")
    for name in ("browser/chromium/manifest.json", "browser/firefox/manifest.json"):
        updated[name] = one(original[name], r'^(\s*"version": )"[^"]+"(,\s*)$',
                            rf'\g<1>"{version}"\g<2>', name)

    for name in DOCS:
        text = original[name]
        if (name in ("docs/RELEASE_NOTES.md", "docs/RELEASE_CHECKLIST.md")
                and old_package == old_tag and old_package != new_package):
            marker = ("CPack package version" if name.endswith("RELEASE_NOTES.md")
                      else "internal version")
            text = text.replace(f"{marker} `{old_package}`",
                                f"{marker} `__CDM_PACKAGE_VERSION__`")
        text = replace_tag(text, old_tag, new_tag, name)
        if name in ("docs/RELEASE_NOTES.md", "docs/RELEASE_CHECKLIST.md"):
            if old_package != old_tag and old_package != new_package:
                text = replace_tag(text, old_package, new_package, name)
            text = text.replace("__CDM_PACKAGE_VERSION__", new_package)
        if name == "docs/RELEASE_CHECKLIST.md" and old_tag != new_tag:
            text = text.replace("- [x] ", "- [ ] ")
            text = re.sub(r"(^- \[ \] Verify local CPack package names[^\n]*?) \(2026-\d\d-\d\d\)",
                          r"\1", text, flags=re.MULTILINE)
        updated[name] = text

    # Validate all expected files and patterns before writing any of them.
    pending = []
    try:
        for name, contents in updated.items():
            if contents == original[name]:
                continue
            target = ROOT / name
            with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8",
                                             dir=target.parent, delete=False) as output:
                output.write(contents)
                pending.append((Path(output.name), target))
            os.chmod(pending[-1][0], target.stat().st_mode)
        for temporary, target in pending:
            os.replace(temporary, target)
    finally:
        for temporary, _ in pending:
            temporary.unlink(missing_ok=True)
    print(f"release version: {new_tag} (package {new_package})")


if __name__ == "__main__":
    try:
        if len(sys.argv) not in (2, 3):
            raise ValueError("usage: sync-release-version.py KIND [RC_NUMBER]")
        bump(sys.argv[1], sys.argv[2] if len(sys.argv) == 3 else None)
    except (OSError, ValueError) as error:
        print(f"release bump: {error}", file=sys.stderr)
        sys.exit(1)
