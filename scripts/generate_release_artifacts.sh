#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
RELEASE_FILE="${RELEASE_FILE:-$ROOT_DIR/.release.toml}"

if [[ ! -f "$RELEASE_FILE" ]]; then
  echo "Release metadata file not found: $RELEASE_FILE" >&2
  exit 1
fi

python3 - "$RELEASE_FILE" <<'PY'
import re, sys
from pathlib import Path

release_path = Path(sys.argv[1])
text = release_path.read_text(encoding='utf-8')
channel = re.search(r'^channel\s*=\s*"?([A-Za-z0-9_-]+)"?', text, re.M)
version = re.search(r'^version\s*=\s*"?([0-9]+\.[0-9]+\.[0-9]+)"?', text, re.M)
rc = re.search(r'^rc\s*=\s*([0-9]+)', text, re.M)
if not channel or not version:
    raise SystemExit('Release file must include channel and version.')

channel_name = channel.group(1).strip()
base_version = version.group(1).strip()
rc_num = int(rc.group(1)) if rc else 0
if channel_name == 'rc':
    final_version = f"{base_version}-rc{rc_num}"
elif channel_name == 'stable':
    final_version = base_version
else:
    raise SystemExit(f"Unsupported release channel: {channel_name}")

print(f"CHANNEL={channel_name}")
print(f"VERSION={final_version}")
PY

export CHANNEL
export VERSION

CHANNEL="$(python3 - "$RELEASE_FILE" <<'PY'
import re, sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding='utf-8')
channel = re.search(r'^channel\s*=\s*"?([A-Za-z0-9_-]+)"?', text, re.M)
print(channel.group(1).strip() if channel else 'stable')
PY
)"
VERSION="$(python3 - "$RELEASE_FILE" <<'PY'
import re, sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding='utf-8')
channel = re.search(r'^channel\s*=\s*"?([A-Za-z0-9_-]+)"?', text, re.M)
version = re.search(r'^version\s*=\s*"?([0-9]+\.[0-9]+\.[0-9]+)"?', text, re.M)
rc = re.search(r'^rc\s*=\s*([0-9]+)', text, re.M)
if not version:
    raise SystemExit('Missing version')
base = version.group(1)
if channel and channel.group(1).strip() == 'rc':
    rc_num = int(rc.group(1)) if rc else 1
    print(f"{base}-rc{rc_num}")
else:
    print(base)
PY
)"

ARCH="$(dpkg --print-architecture)"
PACKAGE_NAME="cdm-${VERSION}-linux-${ARCH}.deb"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"${JOBS:-$(nproc)}"
ctest --test-dir "$BUILD_DIR" --output-on-failure
cpack --config "$BUILD_DIR/CPackConfig.cmake" -G DEB

PACKAGE_PATH="$(find "$ROOT_DIR" -maxdepth 1 -name '*.deb' -printf '%T@ %p\n' | sort -nr | head -n 1 | cut -d' ' -f2-)"
if [[ -z "$PACKAGE_PATH" ]]; then
  echo "No .deb package was generated." >&2
  exit 1
fi

RENAMED_PATH="$ROOT_DIR/$PACKAGE_NAME"
if [[ "$PACKAGE_PATH" != "$RENAMED_PATH" ]]; then
  mv "$PACKAGE_PATH" "$RENAMED_PATH"
fi
PACKAGE_PATH="$RENAMED_PATH"

( cd "$ROOT_DIR" && sha256sum "$PACKAGE_NAME" ) > "$ROOT_DIR/SHA256SUMS.txt"
ls -l "$PACKAGE_PATH" "$ROOT_DIR/SHA256SUMS.txt"

echo "Release channel: $CHANNEL"
echo "Version: $VERSION"
echo "Artifact created: $PACKAGE_PATH"
echo "Checksum file created: $ROOT_DIR/SHA256SUMS.txt"
