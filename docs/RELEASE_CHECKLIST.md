# Release checklist: 0.3.0-rc1 draft

This checklist is for the current Linux release candidate. A checked item must reflect a completed verification run; package configuration alone is not release validation.

## Scope

- [x] Confirm the release version in `.release.toml`, CMake, and the PKGBUILD agrees.
- [x] Confirm the README and release notes describe shipped Linux features and mark Windows/macOS as unsupported (2026-09-28 source review).
- [x] Confirm browser integration is described as manual unpacked/temporary extension installation with per-user native-host registration (2026-09-28 source review).

## Build and runtime checks

- [x] Configure and build the Release tree with `BUILD_TESTING=ON` (2026-09-28, `/tmp/cdm-release-audit`; local Arch host).
- [x] Run full Debug, Release, and ASan CTest suites (41/41 each, 2026-09-28). Criterion/TSan remains a user-accepted tooling exception: 24 Criterion targets abort before assertions in this environment, also reproduced outside cdm.
- [ ] Run `cdm daemon status`, `disable`, and `enable` in an isolated user environment; verify the output and exit codes in [daemon startup](daemon-autostart.md).
- [ ] Start `cdm daemon` twice and verify one serving PID; restart after `SIGKILL` to check stale socket recovery.
- [ ] Verify the installed GUI, `cdm cli` commands, and native browser host from a clean prefix. Staged CLI daemon-status and native-host protocol passed; GUI started with `SDL_VIDEODRIVER=offscreen`, which does not validate visual interaction.
- [ ] On a release desktop, inspect queue schedules/actions, category editing, tray controls, clipboard review, and both GUI themes. Confirm dialogs before destructive actions.
- [ ] In installed Chromium and Firefox, load each extension and register its native host. Verify confirmation/progress, context consent, filters/exclusions, context menus, and URL refresh against a local HTTP server.
- [x] Download ffmpeg-generated HLS and DASH VOD over `127.0.0.1` and verify both published outputs contain playable video and audio with ffprobe (2026-09-28).
- [ ] Confirm HLS/DASH offers in an installed browser and an explicit yt-dlp selection on a consented external test site; verify failure behavior when ffmpeg or yt-dlp is absent.
- [x] Confirm staged `/etc/xdg/autostart/cdm-daemon.desktop`, desktop launcher, browser extension files, Spanish catalog, and native-host binary are present (`DESTDIR` install and DEB/RPM file-list inspection, 2026-09-28).

## Package checks

- [ ] Build a DEB and inspect/install it on Debian or Ubuntu. Arch-host CPack generation and file-list inspection passed, but its metadata omits curl/SQLite/OpenSSL/XML dependencies because `dpkg-shlibdeps` cannot resolve Arch libraries. Use the Ubuntu 24.04 release workflow or equivalent target-distro build before publication.
- [ ] Build an RPM and inspect/install it on Fedora. Arch-host CPack generation and file-list inspection passed, but its auto-requires reference Arch libraries, including glibc 2.43. Use the Fedora container in the release workflow or equivalent target-distro build before publication.
- [ ] Build the Arch package with `./scripts/cdm-release arch` on Arch and inspect/install it with `pacman -U`.
- [x] Verify local CPack package names and versions against `.release.toml`: `cdm-0.3.0-rc1-Linux.deb` / `.rpm`, internal version `0.3.0~rc1` (2026-09-28). Release workflows copy them to architecture-specific names; those published artifacts remain unchecked.
- [ ] Verify the Arch PKGBUILD source checksum before publication; it currently uses `sha256sums=('SKIP')`.
- [ ] Check each package's dependencies, file list, autostart entry, and remove/upgrade behavior.

## Publish only after acceptance

- [ ] Create the matching `v0.3.0-rc1` tag from the intended commit.
- [ ] Generate `SHA256SUMS.txt` from the exact files to upload, then run `sha256sum -c SHA256SUMS.txt` in that directory.
- [ ] Upload artifacts, checksums, and release notes to the matching release.
- [ ] Check each public download URL and installation command on a clean target.

See [browser integration](browser-integration.md) and [daemon startup](daemon-autostart.md) for setup and behavior. Do not use `cdm --help` or `cdm cli --help` as a release check: those are not supported help modes in the current CLI.
