# Core Download Manager (cdm)

cdm is a Linux download manager with a CLI, a background daemon, an SDL2/Nuklear GUI, and optional browser integration.

## Status

The current source builds Linux DEB and RPM packages and includes an Arch PKGBUILD. The release metadata is `0.2.0-rc1`; package availability depends on a release being published. The browser extensions are installed manually as unpacked or temporary extensions.

## Features

- HTTP and HTTPS downloads with segmented transfer and resume.
- A SQLite queue that survives daemon restarts.
- CLI commands to add, pause, resume, and cancel downloads.
- An SDL2/Nuklear GUI for downloads and settings.
- A per-user daemon with XDG login autostart controls.
- Optional Chrome, Chromium, and Firefox URL handoff through a native messaging host.

## Install a locally built package

Run these commands from the repository root after installing the build dependencies in [CONTRIBUTING.md](CONTRIBUTING.md). The commands build from this checkout; they do not depend on a published release asset.

### Debian or Ubuntu

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build && cpack --config build/CPackConfig.cmake -G DEB
sudo apt install ./cdm-0.2.0-rc1-Linux.deb
```

### Fedora or RHEL

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build && cpack --config build/CPackConfig.cmake -G RPM
sudo dnf install ./cdm-0.2.0-rc1-Linux.rpm
```

### Arch Linux

The release helper builds an Arch package from this checkout and leaves it in `packaging/arch/`.

```sh
./scripts/cdm-release arch
sudo pacman -U "$(find packaging/arch -maxdepth 1 -name 'cdm-*.pkg.tar.zst' ! -name '*-debug-*' -print -quit)"
```

## Quick start

```sh
cdm gui
cdm cli add https://example.org/file.zip ~/Downloads
cdm daemon status
```

`cdm cli add` accepts a URL and an optional destination **directory**. It derives a unique filename from the URL. `cdm cli pause ID`, `cdm cli resume ID`, and `cdm cli cancel ID` control queued downloads. Configuration is read from `~/.local/share/cdm/config.toml` on Linux; absent values use built-in defaults.

## Browser integration

The optional extensions hand supported URL-only GET downloads to cdm. Browser cancellation is best effort, and per-user native-host registration is required. See [browser integration](docs/browser-integration.md) for limitations and setup.

## Daemon lifecycle

Graphical sessions with XDG autostart start the packaged daemon at login. `cdm daemon status`, `cdm daemon disable`, and `cdm daemon enable` control the current user's autostart setting. See [daemon startup at login](docs/daemon-autostart.md) for behavior and exit codes.

## Build from source

See [CONTRIBUTING.md](CONTRIBUTING.md) for dependencies, build commands, and tests.

## Roadmap

Windows and macOS are not yet supported. No release date is set for either platform.

## License

cdm is licensed under the [MIT License](LICENSE).
