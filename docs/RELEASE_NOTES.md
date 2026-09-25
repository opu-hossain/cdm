# CDM release notes (draft)

These notes describe the current source and packaging configuration. This draft does not state that a tag or downloadable package has been published.

## Unreleased — phase 2 queues and automation

- Named queues persist priority (0–1000), concurrency caps, local-time schedules, and post-actions. Deleting a queue moves its downloads to Default. Schedule pauses are tracked separately from user pauses. Post-actions fire only after every download in a nonempty queue is DONE for five seconds; shutdown, sleep, and command execution require explicit config opt-in.
- An optional Linux StatusNotifier tray exposes Pause all, Resume all, Quit, and aggregate progress. GUI clipboard monitoring is opt-in and requires review before adding a detected URL.
- `cdm cli add --file PATH` and a GUI multiline dialog support batch entry. CLI results are reported per line with 0/1/2 exit status for success, partial failure, or input failure.
- Categories persist names, extension lists, and default folders. Automatic destinations route by extension; explicit folders bypass routing. The GUI sidebar lists and edits stored categories. Downloads retain a category ID, and deleting a category moves its downloads to Default.
- IPC HELLO now reports version 7. Queue commands use types 45–49; automatic-directory add uses type 50; category CRUD and assigned-category history use types 51–55. Previous message layouts remain unchanged. SQLite schema is version 9.
- Local debug build and full CTest passed 33/33 on 2026-09-25. A separate ASan build passed 33/33. The TSan build compiled, but its full suite passed 13/33: 20 Criterion targets failed before assertions with arena initialization errors or crashes inside `libcriterion.so.3`. This does not establish a cdm data race; a compatible Criterion/TSan test environment is needed to complete that gate. Graphical category editing was not visually exercised in this environment.

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

The configured version is `0.3.0-rc1` (CPack package version `0.3.0~rc1`). Local CPack artifacts use names such as `cdm-0.3.0-rc1-Linux.deb` and `cdm-0.3.0-rc1-Linux.rpm`. Release workflows copy them to architecture-specific names before upload. These names describe build output, not a published download URL.

## Verification before publication

Use the [release checklist](RELEASE_CHECKLIST.md) to record build, test, package, runtime, and install checks on Debian or Ubuntu, Fedora, and Arch. Create checksums from the exact artifacts being published. The PKGBUILD currently uses `sha256sums=('SKIP')` and needs a reviewed checksum before publication.

## Known limitations

- Browser extension setup is manual; Firefox temporary add-ons must be reloaded after restart unless signed and installed persistently.
- Browser download cancellation can leave a partial browser file.
- Graphical login autostart requires a session that implements XDG autostart. Manual and on-demand daemon startup remain available without it.
- Public release availability and target-distro acceptance are not established by these draft notes.
