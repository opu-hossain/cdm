#!/usr/bin/env bash
# Build dependencies for Debian / Ubuntu. Safe to run repeatedly.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libnotify-dev libgtk-3-dev libwebkit2gtk-4.1-dev \
    libcurl4-openssl-dev libsqlite3-dev \
    libcriterion-dev python3
