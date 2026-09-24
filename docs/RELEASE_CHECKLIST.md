# Release checklist: 0.2.0-rc1 draft

This checklist is for the current Linux release candidate. A checked item must reflect a completed verification run; package configuration alone is not release validation.

## Scope

- [ ] Confirm the release version in `.release.toml`, CMake, and the PKGBUILD agrees.
- [ ] Confirm the README and release notes describe only shipped Linux features. Windows and macOS are not yet supported.
- [ ] Confirm browser integration is described as manual unpacked/temporary extension installation with per-user native-host registration.

## Build and runtime checks

- [ ] Build with `cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release` and `cmake --build build`.
- [ ] Run `ctest --test-dir build --output-on-failure`.
- [ ] Run `cdm daemon status`, `disable`, and `enable` in an isolated user environment; verify the output and exit codes in [daemon startup](daemon-autostart.md).
- [ ] Start `cdm daemon` twice and verify one serving PID; restart after `SIGKILL` to check stale socket recovery.
- [ ] Verify the installed GUI, `cdm cli` commands, and native browser host from a clean prefix.
- [ ] Confirm `/etc/xdg/autostart/cdm-daemon.desktop`, the desktop launcher, browser extension files, and native-host binary are installed.

## Package checks

- [ ] Build a DEB with `cpack --config build/CPackConfig.cmake -G DEB` and inspect/install it on Debian or Ubuntu.
- [ ] Build an RPM with `cpack --config build/CPackConfig.cmake -G RPM` and inspect/install it on Fedora.
- [ ] Build the Arch package with `./scripts/cdm-release arch` on Arch and inspect/install it with `pacman -U`.
- [ ] Verify package names and versions against `.release.toml`. Local CPack output uses `cdm-0.2.0-rc1-Linux.deb` or `.rpm`; release workflows copy them to `cdm-0.2.0-rc1-linux-ARCH` names.
- [ ] Verify the Arch PKGBUILD source checksum before publication; it currently uses `sha256sums=('SKIP')`.
- [ ] Check each package's dependencies, file list, autostart entry, and remove/upgrade behavior.

## Publish only after acceptance

- [ ] Create the matching `v0.2.0-rc1` tag from the intended commit.
- [ ] Generate `SHA256SUMS.txt` from the exact files to upload, then run `sha256sum -c SHA256SUMS.txt` in that directory.
- [ ] Upload artifacts, checksums, and release notes to the matching release.
- [ ] Check each public download URL and installation command on a clean target.

See [browser integration](browser-integration.md) and [daemon startup](daemon-autostart.md) for setup and behavior. Do not use `cdm --help` or `cdm cli --help` as a release check: those are not supported help modes in the current CLI.
