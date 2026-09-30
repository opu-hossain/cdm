# Core Download Manager (cdm)

cdm is a Linux download manager with a CLI, a background daemon, an SDL2/Nuklear GUI, and optional browser integration.

## Status

The current source builds Linux DEB and RPM packages and includes an Arch PKGBUILD. The release metadata is `0.3.0-rc1`; package availability depends on a release being published. The browser extensions are installed manually as unpacked or temporary extensions.

## Features

- **Reliable downloads:** HTTP(S), segmented transfer, resume with ETag/Last-Modified checks, retries, SHA-256 verification, speed limits, and HTTP/SOCKS5 proxies.
- **Organization:** SQLite history, named queues with schedules and optional completion actions, and categories that route files into folders.
- **Desktop GUI:** Add single or multiple URLs, manage downloads and queues, see speed and ETA, search history, choose Light/Dark/System theme, and switch between English and Spanish.
- **CLI:** Add, pause, resume, cancel, list, and export/import settings and history as JSON.
- **Browser handoff:** Optional Chrome/Chromium and Firefox extensions offer supported downloads for confirmation; filters, context menus, and optional site-specific browser-session sharing are available.
- **Video:** Select direct video, HLS, DASH, or an available combined YouTube MP4 from the in-page control. See [video downloads](#video-downloads).
- **Desktop extras:** On-demand or login-started daemon, notifications, optional tray and clipboard review, and an optional external antivirus scanner.

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
cdm cli add https://example.invalid/file.zip ~/Downloads
cdm cli list --limit 20
cdm daemon status
```

Replace the `example.invalid` URL with a real file URL. The optional argument to `cdm cli add` is a **directory**. Run `cdm cli` without a subcommand to see all CLI commands and flags. Configuration and the download database live in `~/.local/share/cdm/`.

## Browser integration

The optional extensions hand supported GET downloads to cdm. Per-origin consent can include bounded Cookie, User-Agent and Referer context; Authorization headers, POST bodies, Blob/data URLs, and browser-internal URLs are unsupported. Browser cancellation is best effort and can leave a partial browser file. See [browser integration](docs/browser-integration.md) for setup and limits.

## Video downloads

The extension's **ON** badge enables browser-session sharing for one site; it does not start video detection. In extension **Options**, enable **Media detection**, save, and grant the requested permission. Play a video and use **Download with cdm** at the top right of its frame to choose a detected direct video, HLS, or DASH stream. On YouTube, opening the picker also checks for a combined MP4 that cdm can download with its existing HTTP engine. Options lists retained direct-media candidates. Detection alone does not start a download. See [media downloads](docs/media.md) for format limits.

Browser-captured session values stay in memory. Cookies, proxy credentials, and request options entered directly into cdm use plaintext local storage; ordinary JSON exports omit secrets.

## Daemon lifecycle

Graphical sessions with XDG autostart start the packaged daemon at login. `cdm daemon status`, `cdm daemon disable`, and `cdm daemon enable` control the current user's autostart setting. See [daemon startup at login](docs/daemon-autostart.md) for behavior and exit codes.

## Roadmap

Windows and macOS are not yet supported. No release date is set for either platform.

## License

cdm is licensed under the [MIT License](LICENSE).
