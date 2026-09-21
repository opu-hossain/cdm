#!/usr/bin/env bash
# Build dependencies for Fedora / RHEL. Safe to run repeatedly.
#
# Note: this script does not call sudo. Inside the Fedora container the user is already root, 
# and if you run it on a real Fedora host you would prefix with sudo yourself.
#
set -euo pipefail
dnf install -y \
    cmake gcc gcc-c++ make git pkgconf-pkg-config \
    libnotify-devel gtk3-devel webkit2gtk4.1-devel \
    libcurl-devel sqlite-devel \
    rpm-build python3 which sed findutils
