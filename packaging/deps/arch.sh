#!/usr/bin/env bash
# Install Arch build dependencies.
set -euo pipefail
sudo pacman -S --needed base-devel pacman-contrib rsync \
    cmake pkgconf git \
    libnotify sdl2 libepoxy mesa curl sqlite
