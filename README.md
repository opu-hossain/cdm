# Core Download Manager (cdm)

cdm is a Linux download manager with a CLI, a background daemon, an SDL2/Nuklear GUI, and optional browser integration.

## Status

The current source builds Linux DEB and RPM packages and includes an Arch PKGBUILD. The release metadata is `0.3.0-rc1`; package availability depends on a release being published. The browser extensions are installed manually as unpacked or temporary extensions.

## Features

- HTTP and HTTPS downloads with segmented transfer, resume validators, speed limits, SHA-256 verification, and proxy support.
- SQLite-backed downloads, named queues, schedules, categories, and gated post-actions that survive daemon restarts.
- CLI commands to add, pause, resume, cancel, list, export, and import downloads and settings.
- An SDL2/Nuklear GUI with queue/category controls, batch add, history, System/Light/Dark themes, and English/Spanish text.
- A per-user daemon with XDG login autostart controls.
- Optional Chrome, Chromium, and Firefox handoff through a native messaging host, with filters, context menus, and per-origin consent for browser context.
- Finite HLS and DASH VOD downloads, optional ffmpeg output processing, and an explicit opt-in yt-dlp site tool.
- Optional Linux tray, clipboard review, desktop notifications, and a disabled-by-default antivirus scanner.

## Install a locally built package

Run these commands from the repository root after installing the build dependencies in [CONTRIBUTING.md](CONTRIBUTING.md). The commands build from this checkout; they do not depend on a published release asset.

### Debian or Ubuntu

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build && cpack --config build/CPackConfig.cmake -G DEB
sudo apt install ./cdm-0.3.0-rc1-Linux.deb
```

### Fedora or RHEL

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build && cpack --config build/CPackConfig.cmake -G RPM
sudo dnf install ./cdm-0.3.0-rc1-Linux.rpm
```

### Arch Linux

The release helper builds an Arch package from this checkout and leaves it in `packaging/arch/`.

```sh
./scripts/cdm-release arch
sudo pacman -U "$(find packaging/arch -maxdepth 1 -name 'cdm-*.pkg.tar.zst' ! -name '*-debug-*' -print -quit)"
```

## Install from source

Install the build dependencies for your Linux distribution first. The package
lists and helper scripts are in [CONTRIBUTING.md](CONTRIBUTING.md#development-dependencies).
These commands install cdm system-wide under `/usr/local`; the install also
writes `/etc/xdg/autostart/cdm-daemon.desktop`, so it needs administrator access.

```sh
git clone https://github.com/opu-hossain/cdm.git
cd cdm
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local -DBUILD_TESTING=OFF
cmake --build build -j
sudo cmake --install build
/usr/local/bin/cdm daemon status
/usr/local/bin/cdm gui
```

Run the daemon and GUI as your normal user, without `sudo`. A graphical session
with XDG autostart starts the daemon at the next login; the CLI and GUI can also
start it on demand. Browser integration needs a separate extension install and
per-user native-host registration; see [browser integration](docs/browser-integration.md).
To build and run the test suite before installing, follow
[CONTRIBUTING.md](CONTRIBUTING.md#clone-build-and-test).

## Quick start

```sh
cdm gui
cdm cli add https://example.org/file.zip ~/Downloads
cdm daemon status
```

`cdm cli add` accepts a URL and an optional destination **directory**. It derives a unique filename from the URL or response headers. `cdm cli pause ID`, `cdm cli resume ID`, and `cdm cli cancel ID` control queued downloads; `cdm cli list` pages through history. `cdm cli export` and `cdm cli import` manage JSON backups. Configuration is read from `~/.local/share/cdm/config.toml` on Linux; absent values use built-in defaults.

## Browser integration

The optional extensions hand supported GET downloads to cdm. Per-origin consent can include bounded Cookie, User-Agent and Referer context; Authorization headers, POST bodies and Blob/data URLs are unsupported. Browser cancellation is best effort, and per-user native-host registration is required. See [browser integration](docs/browser-integration.md) for limitations and setup.

## Daemon lifecycle

Graphical sessions with XDG autostart start the packaged daemon at login. `cdm daemon status`, `cdm daemon disable`, and `cdm daemon enable` control the current user's autostart setting. See [daemon startup at login](docs/daemon-autostart.md) for behavior and exit codes.

## Roadmap

Windows and macOS are not yet supported. No release date is set for either platform.

## License

cdm is licensed under the [MIT License](LICENSE).
