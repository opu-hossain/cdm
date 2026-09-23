#!/usr/bin/env bash
# Install Debian and Ubuntu build dependencies.
set -euo pipefail
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config \
    libnotify-dev libsdl2-dev libepoxy-dev libgl1-mesa-dev \
    libcurl4-openssl-dev libsqlite3-dev \
    libcriterion-dev python3
