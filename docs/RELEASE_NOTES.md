# CDM 0.2.0-rc1 release notes (draft)

These notes describe the current source and packaging configuration. This draft does not state that a tag or downloadable package has been published.

## Linux scope

cdm provides a command-line interface, a background daemon, and an SDL2/Nuklear GUI. It downloads HTTP(S) URLs with segmented transfer and resume, stores its queue in SQLite, and supports per-user XDG login autostart. The CLI offers `add`, `pause`, `resume`, and `cancel` commands.

Chrome, Chromium, and Firefox extensions can hand supported URL-only GET downloads to cdm through a native messaging host. The extensions require manual installation and per-user host registration. Browser cancellation is best effort; authenticated, POST, Blob, and data URL downloads are outside this integration's current scope. See [browser integration](browser-integration.md).

Windows and macOS are not yet supported.

## Packaging

CMake can build DEB and RPM packages. The Arch PKGBUILD and local release helper can build an Arch package. All package targets install `cdm`, `cdm_native_host`, the desktop launcher, browser extension files, and an XDG autostart entry. The autostart entry starts the daemon at the next graphical login; it does not start it during package installation. See [daemon startup](daemon-autostart.md).

The configured version is `0.2.0-rc1`. Local CPack artifacts use names such as `cdm-0.2.0-rc1-Linux.deb` and `cdm-0.2.0-rc1-Linux.rpm`. Release workflows copy them to architecture-specific names before upload. These names describe build output, not a published download URL.

## Verification before publication

Use the [release checklist](RELEASE_CHECKLIST.md) to record build, test, package, runtime, and install checks on Debian or Ubuntu, Fedora, and Arch. Create checksums from the exact artifacts being published. The PKGBUILD currently uses `sha256sums=('SKIP')` and needs a reviewed checksum before publication.

## Known limitations

- Browser extension setup is manual; Firefox temporary add-ons must be reloaded after restart unless signed and installed persistently.
- Browser download cancellation can leave a partial browser file.
- Graphical login autostart requires a session that implements XDG autostart. Manual and on-demand daemon startup remain available without it.
- Public release availability and target-distro acceptance are not established by these draft notes.
