#!/usr/bin/env bash
# Install Fedora and RHEL build dependencies. The caller must provide root access.
set -euo pipefail
dnf install -y \
    cmake gcc gcc-c++ make git pkgconf-pkg-config \
    libnotify-devel gtk3-devel webkit2gtk4.1-devel \
    libcurl-devel sqlite-devel \
    rpm-build python3 which sed findutils
