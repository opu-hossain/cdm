# Core Download Manager (cdm)

cdm is a Linux download manager with a CLI, a background daemon, an SDL2/Nuklear GUI, and optional browser integration.

## Status

The current source builds Linux DEB and RPM packages and includes an Arch PKGBUILD. The release metadata is `0.3.0-rc1`; package availability depends on a release being published. The browser extensions are installed manually as unpacked or temporary extensions.

## Features

### Downloads and recovery

- Download HTTP(S) files with multiple connections per download, segmented transfer, resume, and work rebalancing. Configure up to 16 connections per download, concurrent download count, connection and transfer timeouts, and the User-Agent.
- Recover from transient failures with configurable retry attempts and exponential backoff. SQLite retains downloads and chunk state across daemon restarts.
- Probe metadata with HEAD and a bounded GET fallback. Derive safe, unique filenames from `Content-Disposition` or the URL, including percent-decoded names.
- Validate resumed content with stored ETag and Last-Modified values and `If-Range`; restart from byte zero when the source changes. Check the final size and an optional expected SHA-256 digest.
- Set a global bandwidth limit or a per-download byte-per-second limit. Use HTTP or SOCKS5 proxies with optional credentials; individual downloads can supply cookies, Referer, and repeated custom headers.
- View received bytes, total size when known, sampled speed, ETA, progress, and failure details. Active-URL duplicate detection points to the existing download instead of queuing a second copy.

### Queues, categories, and automation

- Create and reorder named queues with priority, per-queue concurrency caps, and daily local-time schedules. Scheduled pauses are separate from manual pauses.
- Enable a queue post-action after every download in a nonempty queue completes: run a command, shut down, or suspend. Each action requires an explicit configuration opt-in.
- Create categories with extension rules and destination folders. Automatically chosen destinations follow category rules; a folder chosen explicitly takes precedence. Deleting a queue or category moves its downloads to Default.
- Keep completed and stopped downloads in paginated history. Remove a record without deleting its file, or confirm deletion of both record and file. Refresh the stored URL and validators of a stopped download when its source redirects.

### Desktop and command line

- Use the SDL2/Nuklear GUI to add one URL or paste a batch, choose a queue and destination, search and filter history, inspect details, manage queues and categories, and export or import backups. Row actions include pause, resume, cancel, open file/folder, copy URL, re-download, refresh URL, and removal.
- Use System, Light, or Dark appearance and English or Spanish interface text. System appearance follows the Linux desktop preference when available; a locale change takes effect after restart.
- Opt into clipboard URL monitoring; a copied link opens a review prompt before adding. Desktop notifications report completion and failure. An optional Linux tray shows aggregate progress and offers Pause all, Resume all, and Quit.
- Use the CLI for single and batch additions, pause/resume/cancel, paginated and status-filtered history, and JSON export/import. Ordinary exports exclude secrets; `--include-secrets` is explicit. Imports can merge or replace after validation; replace backs up SQLite and is refused while a download is active.
- Run a per-user daemon on demand or through XDG graphical-login autostart. `cdm daemon enable|disable|status` manages autostart for the current user.

### Browser and media integration

- Load the optional Chrome/Chromium or Firefox extension and register the per-user native messaging host. Supported HTTP(S) GET downloads open a cdm confirmation popup; link and page context menus can also offer a URL explicitly.
- Configure automatic interception by minimum byte size, extension and MIME allow/deny lists, and excluded hostnames. Explicit context-menu offers bypass automatic filters. An extension badge reports skipped downloads and host errors.
- Opt into site-specific browser-session sharing for bounded Cookie, User-Agent, and Referer capture. Captured values stay in memory and require a fresh offer after daemon restart; the confirmation popup shows their presence without displaying them.
- Select detected video, HLS, or DASH candidates in the extension before offering them to cdm. Finite HLS VOD (including supported AES-128 streams) and static single-period DASH downloads support private resume state; optional `ffmpeg` remuxes or merges outputs. See [media downloads](docs/media.md) for supported stream layouts.
- Explicitly opt into the optional `yt-dlp` site extractor for allowlisted HTTPS sites. It is disabled by default and requires a local `yt-dlp` installation.
- Optionally configure an external antivirus scanner. Published HTTP and media outputs are scanned before completion; scanner failure or timeout blocks completion and attempts to quarantine the file.

Browser extensions require manual installation and host registration. Edge, Brave, Opera, and Vivaldi have native-host registration commands but are not release-verified browser targets. See [browser integration](docs/browser-integration.md) for setup, permissions, and download types that cannot be handed off.

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

`cdm cli add` takes an optional destination **directory** and derives a unique filename from the URL or response headers. Other commands are:

| Command | Purpose |
|---|---|
| `cdm cli add --file list.txt [dest_dir]` | Add one HTTP(S) URL per line. |
| `cdm cli add URL [dest_dir] --cookie VALUE --referrer URL --header "Name: value" --sha256 HEX --limit BYTES_PER_SEC` | Supply optional request headers, integrity check, and a per-download speed limit; repeat `--header` as needed. |
| `cdm cli pause ID`, `resume ID`, `cancel ID` | Control a download. |
| `cdm cli list [--offset N] [--limit N] [--status STATUS]` | Page through history; show ID, status, progress, total size in bytes, and filename. |
| `cdm cli export --out FILE [--include-history] [--include-secrets]` | Save settings and optional history as JSON. |
| `cdm cli import --in FILE --merge` or `--replace` | Restore a JSON backup; replace requires confirmation or `--yes`. |

Replace the `example.invalid` URL with a real file URL. Configuration is read from `~/.local/share/cdm/config.toml` on Linux; absent values use built-in defaults. The daemon stores downloads in `~/.local/share/cdm/downloads.db` and logs to `~/.local/share/cdm/daemon.log`. See [CONTRIBUTING.md](CONTRIBUTING.md) for build and test commands.

## Browser integration

The optional extensions hand supported GET downloads to cdm. Per-origin consent can include bounded Cookie, User-Agent and Referer context; Authorization headers, POST bodies, Blob/data URLs, and browser-internal URLs are unsupported. Browser cancellation is best effort and can leave a partial browser file. See [browser integration](docs/browser-integration.md) for setup and limits.

## Privacy and current limits

Browser-captured session values are kept in memory, but cookies and other request options entered directly in the CLI or GUI, HTTP Basic credentials stored through the engine, and proxy credentials use plaintext local persistence. Protect your user account and backups; ordinary JSON exports omit secrets. The CLI and GUI do not currently provide an entry field for HTTP Basic credentials.

HLS and DASH support the finite layouts described in [media downloads](docs/media.md); live streams, DRM, and several advanced manifest layouts are unsupported. The site extractor and antivirus scanner require external programs and are disabled by default. Browser extension installation and native-host registration are manual.

## Daemon lifecycle

Graphical sessions with XDG autostart start the packaged daemon at login. `cdm daemon status`, `cdm daemon disable`, and `cdm daemon enable` control the current user's autostart setting. See [daemon startup at login](docs/daemon-autostart.md) for behavior and exit codes.

## Roadmap

Windows and macOS are not yet supported. No release date is set for either platform.

## License

cdm is licensed under the [MIT License](LICENSE).
