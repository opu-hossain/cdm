# Core Download Manager (CDM)

<h1 align="center">Core Download Manager</h1>

<p align="center">
A fast, modern, open-source, cross-platform download manager written in C.
</p>

<p align="center">

![License](https://img.shields.io/badge/license-MIT-blue.svg)
![C Standard](https://img.shields.io/badge/C-C11-00599C.svg)
![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-success)
![Status](https://img.shields.io/badge/status-Active%20Development-orange)
![Contributions](https://img.shields.io/badge/contributions-welcome-brightgreen)

</p>

---

> **Project Status**
>
> **Core Download Manager (CDM)** is currently in active development and this is the first public Linux-first release.
>
> The current release focuses on a stable Linux desktop workflow: CLI, daemon, GUI, queue persistence, and resumable HTTP downloads.
>
> Windows and macOS are not part of this public release scope and are not yet claimed as supported platforms.
>
> Contributions, testing, bug reports, and design feedback are welcome.

---

# What is CDM?

**Core Download Manager (CDM)** is a community-driven download manager designed to provide a modern, reliable, and truly cross-platform alternative to existing download managers.

Many download managers today are either:

* Proprietary and require paid licenses
* No longer actively maintained
* Closed source
* Limited to a single platform
* Difficult to extend or contribute to

CDM aims to solve those problems by building a clean, modular download manager entirely in **standard C11**, with the first public release focused on **Linux** support and a long-term path toward broader platform coverage.

The project is built around a reusable download engine that powers multiple frontends—including a command-line interface, graphical interface, daemon, and browser integration.

Our goal isn't simply to clone existing download managers.

Our goal is to build the download manager the open-source community deserves.

---

# Why CDM?

## Fully Open Source

Released under the MIT License.

No subscriptions.

No advertisements.

No premium features.

No vendor lock-in.

---

## Linux-first release

The current public release is intentionally Linux-first.

* Linux: supported for the first release package
* macOS: planned, not claimed yet
* Windows: planned, not claimed yet

Platform-specific code remains isolated behind thin abstraction layers, but only Linux is part of the current public support promise.

---

## Modular Design

Every major component is independent.

* Download Engine
* Daemon
* CLI
* GUI
* Browser Native Host
* Database Layer

This makes the project easier to maintain and allows contributors to work on individual components without understanding the entire codebase.

---

## Built for Developers

CDM is designed with maintainability in mind.

* Modern C11
* Clean architecture
* Minimal dependencies
* Well-defined modules
* Portable code
* Easy to build

---

# Project Goals

CDM is guided by a few long-term goals.

* Build a download manager that works consistently across every major desktop platform.
* Keep the core download engine independent from the user interface.
* Make contributing approachable for both new and experienced developers.
* Keep dependencies lightweight and carefully chosen.
* Favor correctness, readability, and maintainability over unnecessary complexity.
* Provide both graphical and command-line interfaces without duplicating business logic.

---

# Features

## Currently Planned

* Parallel segmented downloads
* Resume interrupted downloads
* Persistent download queue
* HTTP and HTTPS support
* Automatic redirect handling
* Download verification (SHA-256)
* Global and per-download speed limits
* Retry with exponential backoff
* Browser integration
* Background daemon
* Graphical interface
* Command-line interface
* Download scheduling
* Proxy support
* Desktop notifications
* Custom request headers
* Cookies
* Referrer support
* Download history
* Configuration file
* Logging
* SQLite-backed persistence

---

## Future Ideas

These ideas are being explored but are **not yet part of the development roadmap.**

* Torrent support
* FTP/SFTP
* Plugin system
* Remote Web UI
* REST API
* Download categories
* Automatic file organization
* Theme support
* Localization
* Package manager integration

---

# Architecture

```
                   Browser Extension
                           │
                           │
                Native Messaging Host
                           │
        ┌──────────────────┴──────────────────┐
        │                                     │
      GUI                                   CLI
        │                                     │
        └──────────────┬──────────────────────┘
                       │
                 Background Daemon
                       │
                Download Scheduler
                       │
                 Download Engine
                       │
        ┌──────────────┴──────────────┐
        │                             │
 Segment Workers              File Manager
        │                             │
        └──────────────┬──────────────┘
                       │
                    libcurl
                       │
                  Remote Server

                SQLite Persistence
```

The download engine is completely independent from the GUI, allowing different frontends to reuse the same implementation.

---

# Project Structure

```
src/
├── cli/
├── core/
├── daemon/
├── engine/
├── gui/
├── native_host/
├── persistence/
├── platform/
├── utils/
└── vendor/

docs/
tests/
cmake/
resources/
```

Each module has a single responsibility and communicates through well-defined interfaces.

---

# Building

## Requirements

* CMake 3.20+
* C11 compiler

  * GCC
  * Clang
  * MSVC

System libraries and development headers:

* libcurl
* SQLite3
* SDL2
* OpenGL
* libepoxy

Bundled in `src/vendor/` (no build-time fetch):

* Nuklear
* cJSON
* tomlc17
* tinyfiledialogs

Linux builds require **libnotify** development files through `pkg-config`.

---

## Install from the first release package

Download the Debian package from GitHub Releases and verify its checksum before installing:

```bash
curl -LO https://github.com/<OWNER>/<REPO>/releases/download/v0.1.0/cdm-0.1.0-Linux.deb
sha256sum -c SHA256SUMS.txt
sudo dpkg -i cdm-0.1.0-Linux.deb
```

If you are installing from a locally downloaded checksum file, verify the artifact with:

```bash
sha256sum -c SHA256SUMS.txt
```

---

## Clone

```bash
git clone https://github.com/opu-hossain/cdm.git
cd cdm
```

---

## Configure

```bash
cmake -S . -B build -DBUILD_TESTING=ON
```

Project warnings are enabled by default. They can be disabled with
`-DDOWNLOADMGR_ENABLE_WARNINGS=OFF`. Sanitizers are available for GCC and
Clang builds through `-DDOWNLOADMGR_SANITIZER=address`, `undefined`, or
`thread`.

---

## Build

```bash
cmake --build build
```

## Test

```bash
ctest --test-dir build --output-on-failure
```

---

## Run

Daemon

```bash
./build/cdm daemon
```

GUI

```bash
./build/cdm gui
```

CLI

```bash
./build/cdm cli
```

---

# Usage

Add a download

```bash
cdm cli add URL DESTINATION
```

Pause

```bash
cdm cli pause ID
```

Resume

```bash
cdm cli resume ID
```

Cancel

```bash
cdm cli cancel ID
```

---

# Configuration

CDM reads a `config.toml`.

Example:

```toml
[downloads]
max_concurrent = 3

default_directory = "/home/user/Downloads"

[retry]
max_attempts = 5

base_delay_sec = 2

max_delay_sec = 60

[throttle]
max_speed_bytes_per_sec = 0
```

Missing values automatically use built-in defaults.

---

# Screenshots

Screenshots will be added as the project matures.

Planned screenshots include:

* GUI
* CLI
* Queue Manager
* Download Details
* Settings
* Browser Extension

---

# Coding Principles

CDM follows a few simple principles.

* Correctness before cleverness.
* Readability over micro-optimizations.
* Platform-specific code stays isolated.
* The download engine never depends on the GUI.
* Keep dependencies minimal.
* Prefer standard C whenever practical.

---

# License

This project is licensed under the **MIT License**.

See the `LICENSE` file for more information.

Every project-owned source file should include the appropriate SPDX license identifier.

```c
// SPDX-License-Identifier: MIT
```

---

# Acknowledgements

CDM builds upon several excellent open-source projects.

* libcurl
* SQLite
* SDL2
* OpenGL
* libepoxy
* Nuklear
* tinyfiledialogs
* tomlc17
* libnotify on Linux

Thanks to the maintainers and contributors of these projects.

---

# Join the Project

CDM is still in its early stages.

Whether you're an experienced systems programmer or writing your first line of C, your contributions are welcome.

If you have an idea, found a bug, or want to improve the project, we'd love to hear from you.

Together, we can build a modern, open-source download manager for everyone.

---

<p align="center">
<strong>Core Download Manager</strong><br>

Fast. Portable. Open.<br>
Built by the community.

</p>
