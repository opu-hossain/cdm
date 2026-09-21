# Contributing to Core Download Manager

Thank you for your interest in contributing to **Core Download Manager (CDM)**.

CDM is built with the goal of creating a modern, reliable, and community-driven open-source download manager. Contributions of all kinds are welcome — from fixing bugs and improving documentation to implementing new features.

This document explains how to set up your development environment, submit changes, and follow the project's development practices.

---

## Ways to Contribute

There are many ways to contribute:

- Report bugs
- Suggest features
- Improve documentation
- Write tests
- Fix issues
- Improve performance
- Add platform support
- Review pull requests
- Improve the user interface

Not every contribution needs to be code.

---

## Development Setup

### Requirements

You need:

- C11 compatible compiler
    - GCC
    - Clang
    - MSVC
- CMake 3.20+
- Git

Optional:

- Ninja build system
- clang-format
- clang-tidy
- cppcheck

---

### Clone the Repository

```bash
git clone https://github.com/<username>/cdm.git
cd cdm
```

### Configure Build

```bash
cmake -S . -B build -DBUILD_TESTING=ON

```

For a debug build:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug

```

### Build

```bash
cmake --build build

```

### Run Tests

```bash
ctest --test-dir build --output-on-failure
```

### Create the first Debian package

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
cpack --config build/CPackConfig.cmake -G DEB
sha256sum *.deb > SHA256SUMS.txt
```

For AddressSanitizer validation:

```bash
cmake -S . -B build-asan -DBUILD_TESTING=ON \
    -DDOWNLOADMGR_SANITIZER=address
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build-asan --output-on-failure
```

---

## Project Structure

```text
src/
├── cli/
├── core/
├── daemon/
├── engine/
├── gui/
├── native_host/
├── persistence/
├── platform/
└── utils/

```

Each module has a specific responsibility. Avoid placing unrelated functionality into existing modules.

---

## Branch Guidelines

Create branches from `main`. Recommended naming conventions:

* `feature/add-download-scheduler`
* `bugfix/fix-resume-crash`
* `docs/update-install-guide`
* `refactor/improve-worker-pool`

Keep branches focused on one purpose.

---

## Commit Guidelines

Use clear commit messages.

**Good:**

* `engine: add segmented download scheduler`
* `gui: fix download list rendering`
* `database: improve migration handling`

**Avoid:**

* `fixed stuff`
* `changes`
* `update`

---

## Pull Requests

Before opening a pull request:

* Make sure the project builds successfully.
* Test your changes.
* Update documentation if necessary.
* Keep commits clean.
* Explain what changed and why.

A good pull request contains:

* **Description:** What does this change do?
* **Motivation:** Why is this change needed?
* **Testing:** How was it tested?

---

## Code Review

All changes are reviewed before merging. Review focuses on:

* Correctness
* Maintainability
* Performance
* Portability
* Security
* Code style

Suggestions during review are meant to improve the project.

---

## Coding Expectations

CDM follows these principles:

* Prefer simple solutions.
* Avoid unnecessary dependencies.
* Keep modules independent.
* Write portable C whenever possible.
* Handle errors explicitly.
* Document non-obvious code.

---

## Reporting Bugs

Before opening an issue:

* Check existing issues.
* Make sure the problem is reproducible.
* Include system information.
* Include logs if available.

Useful information to include in your bug report:

* **Operating System:**
* **Compiler:**
* **CMake Version:**
* **CDM Version:**
* **Steps to reproduce:**
* **Expected behavior:**
* **Actual behavior:**

---

## Feature Requests

Feature requests should explain:

* The problem being solved.
* Why the feature is useful.
* Possible implementation ideas.

Large features should be discussed before implementation.

---

## First Contributions

New contributors are welcome! Good starting points include:

* Documentation improvements
* Small bug fixes
* Tests
* UI improvements
* Platform testing

Look for issues tagged with `good first issue` or `help wanted`.

