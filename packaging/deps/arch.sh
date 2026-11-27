#!/usr/bin/env bash
# Install Arch build dependencies.
set -euo pipefail

pacman_args=(--needed)
if [[ "${PACMAN_NOCONFIRM:-0}" == "1" ]]; then
    pacman_args+=(--noconfirm)
fi

sudo pacman -S "${pacman_args[@]}" base-devel pacman-contrib rsync \
    cmake pkgconf git python \
    libnotify sdl2 libepoxy mesa curl sqlite openssl libxml2 glib2
