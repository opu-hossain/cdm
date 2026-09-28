#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${1:-$("$ROOT/scripts/cdm-release" version | awk '/^tag version:/ {print $3}')}"
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
  --exclude='PLAN.md' \
  --exclude='PROJECT_CONTEXT.md' \
  --exclude='docs/agent-log.md' \
  --exclude='docs/agent-task-brief-template.md' \
  --exclude='docs/agent-task-queue.md' \
  --exclude='docs/open-questions.md' \
  "$ROOT/" "$STAGE/cdm-${VERSION}/"

tar -czf "$OUT" -C "$STAGE" "cdm-${VERSION}"
echo "wrote $OUT"
