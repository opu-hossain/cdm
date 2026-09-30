# CDM release notes (draft)

These notes describe the current source and packaging configuration. This draft does not state that a tag or downloadable package has been published.

## Unreleased — phase 5 polish (implementation complete; release gates open)

- Added versioned JSON export/import for settings and optional download history. Secrets are excluded from ordinary export; `--include-secrets` is explicit. Import validates records, backs up SQLite before replace, refuses replace while a download is active, and restores nonterminal imported jobs PAUSED. CLI and GUI expose the backup flow.
- Added System/Light/Dark GUI palettes and a settings selector. On Linux, System reads the XDG Settings portal; unavailable preference falls back to light. Added stable `tr()` keys with built-in English and an installed Spanish catalog. `[ui] locale` takes effect after restart.
- Added disabled-by-default `[security] scanner_command` and quoted `scanner_args`. Every published HTTP, HLS and DASH output is scanned before scheduler completion, including DASH companion audio. A nonzero exit, launch failure or 120-second timeout attempts a private `.quarantine` move and records nonretryable `Blocked by scanner`; a failed move can leave the original path under ERROR. Scanner command/args are included in JSON only with `--include-secrets`.
- Added a [Windows/macOS port plan](platform-port.md) covering local IPC, daemon/process/file primitives, desktop integration, browser registration and packaging. Neither platform is supported yet.
- On 2026-09-28, full Debug, Release and ASan builds each passed 41/41 CTest targets. The TSan build succeeded, but 24 Criterion targets aborted before assertions; a standalone one-assertion reproduction has the same runner failure. The user accepted this tooling exception for wrap-up, without a claim of race-free Criterion coverage. Staged daemon-status and native-host protocol smoke passed; the GUI started with SDL's offscreen driver. Loopback HLS/DASH downloads produced playable video and audio. Release-desktop visual checks, installed Chromium/Firefox offers, and target-distro package installation remain on the [release checklist](RELEASE_CHECKLIST.md).

## Unreleased — phase 4 media (implementation complete; release gates open)

- Browser extensions collect media candidates and offer them only after user selection. The popup identifies HLS, DASH and video offers; detection alone does not open a popup.
- Added finite HLS VOD parsing/downloading with master selection, AES-128 identity keys, initialization maps, validator-aware private resume state, and optional fixed-argument ffmpeg remux. Live, DRM, byte ranges, discontinuities and separate HLS audio renditions remain unsupported.
- Added static single-period DASH parsing/downloading with highest-bandwidth audio/video selection, inherited BaseURL/SegmentTemplate/SegmentList, finite timelines and optional ffmpeg merge. Missing/failed ffmpeg retains both tracks and records the companion audio path. Dynamic/live, DRM, SegmentBase, byte ranges and multi-period MPDs remain unsupported.
- Removed the external site downloader. Direct video, HLS, and DASH remain supported. Existing site-tool database records fail clearly when resumed; historical IPC numbers and columns remain reserved for upgrade compatibility.
- SQLite schema retains legacy `site_grab` columns for upgrades; HELLO reports IPC version 15, and legacy site messages are disabled. OpenSSL Crypto and libxml2 are required build dependencies. See [media downloads](media.md).
- The current Debug, Release and ASan builds each pass full CTest 41/41. A separate ffmpeg-generated loopback smoke produced playable HLS and DASH video+audio outputs; it found and fixed rejection of valid `PT0.0S` zero Period starts. An ASan scheduler gate also found and fixed terminal-state removal before worker join. Criterion/TSan is a documented tooling exception. Installed-browser media offers remain on the release checklist.

## Unreleased — phase 3 browser integration

- Added per-origin, optional browser-session consent and bounded Cookie/User-Agent/Referer capture. Captured context stays in memory; SQLite schema v10 stores only `requires_browser_context`. Restarted contextual downloads require a fresh confirmed browser offer. Contextual redirects are refused; Authorization, POST and Blob/data handoff remain unsupported.
- Added HTTP(S) link/page context menus and sync-backed options for byte minimum, extension/MIME lists, and hostname exclusions. Deny wins; either populated allowlist may match. Unknown sizes bypass positive minimums. Exclusions affect automatic interception only; wildcard patterns include the base host. Explicit menu offers bypass automatic policy.
- IPC v9 adds type 40 for ID-only URL refresh, type 56 for JSON browser context and type 57 for presence bits. Existing raw-wire layouts remain unchanged. Refresh preserves the record on failure and refuses active/lost-context downloads or changed partial content.
- Added stable Linux installer flags for Edge, Brave, Opera, and Vivaldi, requiring existing profile roots and the actual extension ID. Opera shares Chrome's native-host registration and reports the replacement. Custom/sandboxed profile layouts remain unsupported.
- On 2026-09-28 the current Debug and Release suites pass 41/41, including JS/options, native host, installer, HTTP and IPC fixtures; staged native-host protocol smoke passes. Chrome/Chromium and Firefox are the release browser targets. Installed Chromium/Firefox handshake/visual smoke remains unverified and is in the release checklist. Edge, Brave, Opera and Vivaldi extension smoke is deferred; their registration flags remain available. Criterion/TSan is a documented tooling exception.

## Unreleased — phase 2 queues and automation

- Named queues persist priority (0–1000), concurrency caps, local-time schedules, and post-actions. Deleting a queue moves its downloads to Default. Schedule pauses are tracked separately from user pauses. Post-actions fire only after every download in a nonempty queue is DONE for five seconds; shutdown, sleep, and command execution require explicit config opt-in.
- An optional Linux StatusNotifier tray exposes Pause all, Resume all, Quit, and aggregate progress. GUI clipboard monitoring is opt-in and requires review before adding a detected URL.
- `cdm cli add --file PATH` and a GUI multiline dialog support batch entry. CLI results are reported per line with 0/1/2 exit status for success, partial failure, or input failure.
- Categories persist names, extension lists, and default folders. Automatic destinations route by extension; explicit folders bypass routing. The GUI sidebar lists and edits stored categories. Downloads retain a category ID, and deleting a category moves its downloads to Default.
- IPC HELLO now reports version 7. Queue commands use types 45–49; automatic-directory add uses type 50; category CRUD and assigned-category history use types 51–55. Previous message layouts remain unchanged. SQLite schema is version 9.
- Local phase-2 Debug and ASan builds each passed 33/33 CTest targets on 2026-09-25; the current tree passes 41/41 in Debug and ASan. Criterion test processes abort before assertions under TSan even in a one-assertion standalone reproduction; non-Criterion TSan integration targets pass. The user accepted this as a tooling exception for wrap-up, without claiming race-free unit coverage. Graphical category editing remains a manual release acceptance check.

## Unreleased — phase 1 transfer parity

- Added HTTP and SOCKS5 proxy settings (URL and optional credentials), applied to metadata probes and download workers. Proxy configuration is editable in the GUI.
- Added per-download HTTP Basic username/password persistence and curl use for probes and workers. The details IPC reports the username and whether a password exists without returning the password. CLI/GUI credential entry is still absent.
- Added configurable connections per download (1–16, default 8), User-Agent (default `cdm/0.1`), connect timeout (default 10 seconds) and transfer timeout (default 30 seconds), with GUI controls.
- Added normalized active-URL duplicate detection. CLI add prints the existing ID and exits successfully; GUI highlights that row and shows a toast; the browser popup shows an existing-download notice.
- Added GUI `Remove from list` and confirmed `Delete file` actions. The latter deletes the database row and chunks in a transaction, then unlinks the file. ACTIVE downloads are refused. Remove IPC uses type 44 because type 34 was already assigned to v2 subscription; protocol HELLO now reports version 4.
- Local debug build and full CTest passed 31/31 on 2026-09-25. HTTP/proxy/auth and CLI duplicate integration tests use 127.0.0.1. Offscreen GUI startup passed; menu clicks were not automated.
- Cookies, HTTP Basic credentials and proxy password remain in plaintext local persistence. See `docs/open-questions.md` for credential entry and storage policy.

## Unreleased — phase 0 foundations

- Metadata probing falls back from a failed HEAD to a bounded GET `Range: bytes=0-0` request. Automatic filenames now use a safe `Content-Disposition` name when available and decode URL percent escapes; explicitly chosen filenames stay unchanged.
- Resume state stores ETag and Last-Modified. A changed validator restarts the download from byte zero; resumed ranges use `If-Range` when a suitable validator exists.
- Fixed unknown-size whole-file transfers, stale chunk state after parallel fallback, and the rebalance callback race. The Windows mutex-destroy declaration now matches its implementation.
- CLI add now fails when the daemon rejects it, and repeated `--header` values remain valid through IPC serialization.
- Daemon progress events now include received bytes, total bytes, sampled speed, ETA and an error string. The GUI and browser popup display speed and ETA while retaining legacy daemon fallback.
- History can be fetched in pages beyond the legacy 200-row list limit. The GUI loads a bounded window as the user scrolls; `cdm cli list` supports offset, limit and status filtering, with a daemon-recorded byte-size column.
- Local debug build and full CTest passed 28/28 on 2026-09-25. Offscreen GUI startup with 600 completed history rows and popup progress-stream smoke checks passed; visual scrolling was not automated.

## Linux scope

cdm provides a command-line interface, a background daemon, and an SDL2/Nuklear GUI. It downloads HTTP(S) URLs with segmented transfer and resume, stores its queue in SQLite, and supports per-user XDG login autostart. The CLI offers `add`, `pause`, `resume`, `cancel`, and paginated `list` commands.

Chrome, Chromium, and Firefox extensions can hand supported URL-only GET downloads to cdm through a native messaging host. The extensions require manual installation and per-user host registration. Browser cancellation is best effort; authenticated, POST, Blob, and data URL downloads are outside this integration's current scope. See [browser integration](browser-integration.md).

Windows and macOS are not yet supported.

## Packaging

CMake can build DEB and RPM packages. The Arch PKGBUILD and local release helper can build an Arch package. All package targets install `cdm`, `cdm_native_host`, the desktop launcher, browser extension files, and an XDG autostart entry. The autostart entry starts the daemon at the next graphical login; it does not start it during package installation. See [daemon startup](daemon-autostart.md).

Local Arch-host CPack generation produced DEB and RPM files with the expected installed file list. These are smoke artifacts, not target-distro release validation: the Arch-host DEB lacks automatically discovered non-GUI runtime dependencies, and the RPM auto-requires this host's glibc/libxml versions. The release workflows build on Ubuntu 24.04 and Fedora 41 respectively; inspect and install those outputs on their target systems before publishing.

The configured version is `0.3.0-rc1` (CPack package version `0.3.0~rc1`). Local CPack artifacts use names such as `cdm-0.3.0-rc1-Linux.deb` and `cdm-0.3.0-rc1-Linux.rpm`. Release workflows copy them to architecture-specific names before upload. These names describe build output, not a published download URL.

## Verification before publication

Use the [release checklist](RELEASE_CHECKLIST.md) to record build, test, package, runtime, and install checks on Debian or Ubuntu, Fedora, and Arch. Create checksums from the exact artifacts being published. The PKGBUILD currently uses `sha256sums=('SKIP')` and needs a reviewed checksum before publication.

## Known limitations

- Browser extension setup is manual; Firefox temporary add-ons must be reloaded after restart unless signed and installed persistently.
- Browser download cancellation can leave a partial browser file.
- Graphical login autostart requires a session that implements XDG autostart. Manual and on-demand daemon startup remain available without it.
- Public release availability and target-distro acceptance are not established by these draft notes.
