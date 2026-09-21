#!/usr/bin/env bash
set -euo pipefail

VERSION="${1:-0.1.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STAGE="$(mktemp -d)"
OUT="/tmp/cdm-${VERSION}.tar.gz"

cleanup() { rm -rf "$STAGE"; }
trap cleanup EXIT

mkdir -p "$STAGE/cdm-${VERSION}"

rsync -a \
  --exclude='.git' \
  --exclude='build*' \
  --exclude='_CPack_Packages' \
  --exclude='*.deb' \
  --exclude='*.rpm' \
  --exclude='*.pkg.tar.zst' \
  --exclude='packaging/arch/pkg' \
  --exclude='packaging/arch/src' \
  --exclude='packaging/arch/cdm-*.tar.gz' \
  --exclude='.cache' \
  "$ROOT/" "$STAGE/cdm-${VERSION}/"

tar -czf "$OUT" -C "$STAGE" "cdm-${VERSION}"
echo "wrote $OUT"
