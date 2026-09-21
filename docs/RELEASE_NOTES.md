# CDM v0.1.0 release notes

## Overview
This is the first public Linux-first release of Core Download Manager (CDM).

The initial release focuses on the core desktop workflow needed for a practical Linux download manager:
- CLI interface
- background daemon
- GUI shell
- SQLite-backed queue persistence
- resumable downloads
- safe destination validation
- local HTTP integration and retry logic

## Included in this release
- Debian package generation for Linux
- daemon startup and lifecycle handling
- GUI asset lookup that works from an installed prefix
- IPC socket path isolation for user runtime directories
- engine resume and partial-file validation
- queue safety checks for duplicate or unsafe destinations
- CI validation through CTest

## Supported scope
This release is intended for Linux desktop use and is supported in the form of a Debian package.

The following are not part of the current public guarantee:
- Windows support
- macOS support
- browser extension packaging
- remote/hosted service features
- broader platform compatibility beyond Linux

## Installation
```bash
curl -LO https://github.com/<OWNER>/<REPO>/releases/download/v0.1.0/downloadmgr-0.1.0-Linux.deb
sha256sum -c SHA256SUMS.txt
sudo dpkg -i downloadmgr-0.1.0-Linux.deb
```

## First-run usage
```bash
downloadmgr gui
downloadmgr daemon
downloadmgr cli
```

## Known limitations
- Packaging and runtime validation are focused on Linux-first usage.
- GUI and daemon integration should be used in a standard desktop session.
- Additional distribution packages and platform support remain future work.

## Verification
The project test suite passes before release:
```bash
ctest --test-dir build --output-on-failure
```

The installed-prefix contract is also verified for the Debian package layout and CLI startup path outside the build tree.
