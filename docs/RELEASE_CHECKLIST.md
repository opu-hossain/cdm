# Release checklist

This checklist captures the acceptance steps for the first Linux-first release of CDM.

## Release scope
- Linux-first release only for the initial public package.
- Supported public interfaces: CLI, daemon, and GUI on Linux.
- Windows/macOS support is not part of this release and should not be claimed.

## Pre-release validation
- [ ] Build succeeds in Release mode.
- [ ] Full test suite passes: `ctest --test-dir build --output-on-failure`.
- [ ] Installed package can be generated: `cpack --config build/CPackConfig.cmake -G DEB`.
- [ ] Installed binary launches from a clean prefix without depending on the source tree.
- [ ] GUI assets resolve under the installed runtime layout.
- [ ] Daemon socket path resolves under `XDG_RUNTIME_DIR` or the user-local runtime location.
- [ ] CLI help output works from the installed binary.

## Package acceptance
- [ ] Debian package builds cleanly with `cpack`.
- [ ] Artifact name matches the release tag, for example `downloadmgr-0.1.0-Linux.deb`.
- [ ] `SHA256SUMS.txt` is generated and includes the package checksum.
- [ ] The checksum file is uploaded with the GitHub release.
- [ ] Users can verify the package using:
  - `sha256sum -c SHA256SUMS.txt`
  - or `sha256sum downloadmgr-0.1.0-Linux.deb`

## GitHub release checklist
- [ ] The release tag matches the package version.
- [ ] Release notes are published with the artifact.
- [ ] The package is attached to the GitHub release.
- [ ] The checksum file is attached to the GitHub release.
- [ ] Optional: signed checksum file is attached with cosign if the release signing key is configured.

## Post-release
- [ ] Verify the public install command works on a clean Ubuntu/Debian machine.
- [ ] Confirm `sudo dpkg -i` succeeds.
- [ ] Confirm the desktop launcher is discoverable after install.
- [ ] Monitor for packaging or runtime issues from the first users.

## Recommended commands
```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB
sha256sum downloadmgr-0.1.0-Linux.deb > SHA256SUMS.txt
sha256sum -c SHA256SUMS.txt
```
