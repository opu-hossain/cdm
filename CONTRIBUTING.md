# Contributing to Core Download Manager

cdm currently builds and packages for Linux. Windows and macOS are not yet supported. Contributions should include a focused change, a description of its behavior, and the commands used to verify it.

## Development dependencies

- A C11 compiler (GCC or Clang), CMake 3.20+, Python 3, and pkg-config.
- Development files for libcurl 7.60+, SQLite 3.24+, libnotify, SDL2, libepoxy, and OpenGL.
- Criterion when configuring with `-DBUILD_TESTING=ON`.
- Git to clone the repository.

The distro dependency scripts show the package names for [Debian and Ubuntu](packaging/deps/deb.sh), [Fedora](packaging/deps/rpm.sh), and [Arch](packaging/deps/arch.sh). Run a script only if you want it to install packages on your machine. The RPM and Arch scripts prepare package builds without Criterion; install that library separately to run the C test suite.

## Clone, build, and test

```sh
git clone https://github.com/opu-hossain/cdm.git
cd cdm
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Use `-DCMAKE_BUILD_TYPE=Debug` for a debug build. Project warnings are on by default and can be disabled with `-DDOWNLOADMGR_ENABLE_WARNINGS=OFF`. GCC and Clang builds accept `-DDOWNLOADMGR_SANITIZER=address`, `undefined`, or `thread`.

The executable is `build/cdm`; the native messaging host is `build/cdm_native_host`. To install from source using the configured prefix:

```sh
cmake --install build --prefix /usr/local
```

The XDG autostart entry is installed under `/etc/xdg/autostart`, so a system-wide install may require elevated privileges. See [daemon startup](docs/daemon-autostart.md).

## Build packages

Configure with `-DCMAKE_INSTALL_PREFIX=/usr` before making distro packages. `CMakeLists.txt` defines the DEB and RPM generators; the Arch PKGBUILD uses `makepkg`. Package output names follow `.release.toml`, currently `0.2.0-rc1`.

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr -DBUILD_TESTING=OFF
cmake --build build
cpack --config build/CPackConfig.cmake -G DEB
cpack --config build/CPackConfig.cmake -G RPM
```

Build the Arch package from the local checkout with `./scripts/cdm-release arch` on an Arch system with the tools in `packaging/deps/arch.sh`. The public PKGBUILD fetches a tagged source archive and requires that tag to exist. Do not publish a PKGBUILD with `sha256sums=('SKIP')` without generating and reviewing a checksum.

## Submit a change

Keep commits focused and update documentation when behavior changes. Before opening a pull request, build the affected targets and run relevant tests. Describe what changed, why, and how it was checked. For security reports, use the private route in [SECURITY.md](SECURITY.md) instead of a public issue.
