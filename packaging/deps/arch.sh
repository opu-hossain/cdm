#!/usr/bin/env bash
# Build dependencies for Arch. Safe to run repeatedly.
set -euo pipefail
sudo pacman -S --needed base-devel pacman-contrib rsync \
    cmake pkgconf git \
    libnotify gtk3 webkit2gtk-4.1 curl sqlite
