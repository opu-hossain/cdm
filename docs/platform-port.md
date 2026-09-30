# Windows and macOS port plan

Status: design only, 2026-09-28. Linux is the supported runtime. This document
resolves the blocker list in `PROJECT_CONTEXT.md` §6 against the current source;
it does not claim a Windows or macOS build has passed. Preserve the existing
download, database, IPC message, and browser JSON formats during a port.

## 1. Port boundaries and order

The core is C11, but platform code is spread across the daemon, media engines,
GUI, host installer, and build system. Keep the platform-neutral protocol in
`src/platform/ipc_protocol.h`, the SQLite schema in `src/persistence/db.c`,
and the queue and worker contracts in `src/core/queue_manager.h` and
`src/engine/worker_pool.h`. Add platform implementations behind the existing
`src/platform/*.h` interfaces, then move direct OS calls in other directories
behind those interfaces. Do not add fields to raw-wire structs. A new IPC
capability uses a new message number or versioned struct.

Implement in this dependency order:

1. **Build and basic file/runtime primitives.** Make CMake resolve each OS's
   GUI/notification libraries, verify `dm_thread_*`, filesystem paths,
   positional file I/O, disk space, and monotonic clocks. A CLI-only build is
   useful before GUI dependencies are available.
2. **Local IPC and daemon lifecycle.** Implement same-user transport,
   singleton ownership, bounded framing, timeout, disconnect, and event fanout.
   Bring up daemon plus `cdm cli list/add/pause/resume` before the browser host.
3. **HTTP downloads and persistence.** Run localhost HTTP integration tests,
   resume, cancellation, checksum, and quarantine on each target OS.
4. **GUI, autostart, notifications, and tray.** Keep these independent options
   where possible, so the CLI daemon remains usable when a desktop service is
   absent.
5. **Browser native messaging and media tools.** Install signed/absolute host
   manifests, then port child-process cancellation, HLS/DASH staging, ffmpeg,
   and scanner hooks.
6. **Package and release gates.** Build, install/uninstall, upgrade, and test
   under a standard account. Verify privacy, path quoting, and browser handoff.

## 2. Blocker map from `PROJECT_CONTEXT.md` §6

| Linux/POSIX assumption and evidence | Windows decision | macOS decision | Acceptance gate |
|---|---|---|---|
| `CMakeLists.txt:59-72` requires pkg-config `libnotify`, SDL2, epoxy; `CMakeLists.txt:270-290` links OpenGL and Linux GIO. | Split GUI and notification dependencies by OS; use SDL2/OpenGL, replace Linux epoxy/portal integration as needed. | Use SDL2/OpenGL and a macOS GL loader/backend; replace Linux portal integration. | Configure and link CLI, daemon, GUI, host without Linux-only pkg-config modules. |
| `src/platform/ipc_socket.c:19-33`, `:67-163`, `:1455-1640` use Unix sockets, `flock`, UID, and `XDG_RUNTIME_DIR`. | Same-user named pipe with explicit user SID ACL, independent singleton mutex, bounded nonblocking reads/writes. | Unix-domain socket under a private per-user directory plus `flock` or a launchd-owned socket. | Two users cannot control each other's daemon; stale endpoint recovery and slow clients work. |
| `src/daemon/daemon.c:97-147`, `:190`, `:250-253` use HOME/XDG, double fork, `isatty`, POSIX signals. | Foreground worker process launched detached through `CreateProcess`; process handle or named mutex for singleton; console-control/shutdown handler. | Foreground process supervised by per-user launchd agent, avoiding double fork under launchd; signal handling retained. | GUI/CLI auto-start, second-daemon rejection, graceful shutdown and database close. |
| `src/platform/file_io.c:11-133`, `src/engine/finalize.c:110-151`, `src/platform/diskspace.c:3-34` use `open`, `pwrite`, `link`, `statvfs`, POSIX modes. | Use wide path APIs, exclusive creation, positioned `WriteFile`, volume free-space query, and atomic no-replace quarantine publication. | Existing POSIX calls mostly apply; validate APFS permissions, file naming and durability. | Paths with spaces/non-ASCII, collision safety, resume writes and quarantine all pass. |
| `src/engine/hls.c:543-550`, `:1546`; `src/engine/dash.c:724-725`, `:1009`; HLS and DASH guard staging with POSIX files and `flock`. | Implement private staging, locking and durable publish without symlink/reparse-point escapes; keep format/restart compatibility. | Reuse POSIX path with macOS filesystem tests; verify `flock`/rename behavior. | HLS/DASH restart, lock collision and companion cleanup tests pass. |
| `src/platform/spawn.c:11-101`, `:128-243`, `:381-602` use fork/exec or `posix_spawn`, pipes, `waitpid`, signals. | One argv-safe `CreateProcessW` helper with redirected standard handles, timeout, process-tree termination and reaping; adapt ffmpeg/scanner/post-action. | Existing POSIX helper is a starting point; verify sandboxed path lookup and process-tree cancellation. | Missing tool, nonzero exit, timeout, cancel, spaces/quotes and long paths are covered. |
| `src/platform/daemon_autostart.c:38-81`, `resources/autostart/cdm-daemon.desktop.in`, `CMakeLists.txt:349-357` use XDG autostart. | Per-user opt-in startup registration owned by installer or user command; uninstall removes only cdm's entry. | Per-user LaunchAgent with a foreground `cdm daemon`; enable/disable/status map to launchd state. | Login start is idempotent, opt-in status accurate, uninstall clean. |
| `src/platform/tray.c:7-11`, `:190-280` use StatusNotifier/GLib D-Bus; `src/daemon/daemon.c:35-84` owns action requests. | OS tray adapter behind `src/platform/tray.h`; callbacks post atomic requests to daemon owner. | macOS menu-bar adapter behind same header; daemon/GUI process ownership must be settled. | Pause/resume/quit work without cross-thread queue mutations. |
| `src/utils/notify.c:7-59` uses libnotify or a silent stub. | Add native toast adapter with app identity and permission/activation handling. | Add native user-notification adapter with authorization handling. | Complete/error notifications work and degrade quietly when denied. |
| `src/native_host/browser_install.c:14-93`, `:174`; `src/native_host/main_host.c:19-20`, `:46-90` assume Linux manifest dirs, `/` and `select`. | Resolve per-browser Windows manifest registration and absolute `.exe` paths; set stdin/stdout binary mode; replace `select` polling. | Resolve per-browser macOS manifest dirs and app-bundle executable path; `select` may remain. | Chromium-family/Firefox host handshake and popup handoff pass with real browsers. |
| `src/utils/config.c:95-118`, `src/utils/i18n.c:330-341`, `src/gui/theme.c:73-104` use Linux config/data/catalog paths and XDG portal. | `%APPDATA%`/local app data policy, bundle-relative locale, OS theme query. | `~/Library/Application Support` policy, bundle-relative locale, OS theme query. | Config and Spanish locale survive restart; theme auto follows desktop preference. |

The Windows named-pipe design must supply its own security descriptor: the
documented default pipe ACL can grant read access beyond the owner
([Microsoft named-pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)).
Overlapped I/O is the documented way to avoid blocking the server on a slow
client ([Microsoft named-pipe modes](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-open-modes)).

## 3. IPC contract to retain

The daemon owns up to 16 client connections and sends status events from
workers (`src/platform/ipc_socket.c:35-38`, `:166-170`, `:2364`). CLI, GUI,
native host and popup all use `ipc_client_connect*`, `ipc_read_exact`, and
`ipc_write_exact` (`src/platform/ipc_socket.c:1619-1812`, `:2322-2363`).
The port should isolate transport handles from protocol serialization. Avoid
casting Windows `HANDLE` to POSIX `int`; provide a transport handle type or
platform-specific transport module and keep message framing tests shared.
Preserve HELLO/version negotiation, limits, malformed-frame rejection, event
subscription, and browser offer expiry. `docs/ipc.md` is the wire reference.

Windows: choose one same-user pipe name (containing the logged-on user SID or
equivalent opaque identifier) and reject remote pipe clients; use an explicit
owner-only ACL. A named mutex handles daemon singleton ownership separately
from the pipe. Use bounded overlapped operations and cancel pending I/O during
shutdown. macOS: retain a Unix socket at a per-user private runtime path;
ensure owner and mode checks on both socket and lock, handle stale files, and
respect `sockaddr_un.sun_path` length. Do not choose TCP loopback merely to
avoid pipe/socket work: the current IPC has no network authentication.

## 4. Process, filesystem and media rules

`src/platform/thread.c:7-80` already has a Windows thread/critical-section
branch; validate create/join/detach and mutex tests on Windows. Its signature
for `dm_mutex_destroy` matches `src/platform/thread.h`; no remaining mismatch
is evident in the current tree. `src/platform/bandwidth.c:8-19` already uses
`GetTickCount64` on Windows. The POSIX file layer is not portable as-is:
preserve exclusive creation and positional writes in
`src/platform/file_io.c:50-133`, plus the 1024-byte path bounds used by
`src/core/queue_manager.h:58-79`. Decide whether internal paths become UTF-8
and convert only at Windows API boundaries; do not silently truncate a path.

Media and scanner files must stay private until publication. The new scanner
boundary is `src/engine/engine_runner.c:173-234`: every published primary and
DASH companion output is checked before scheduler `DONE`; nonzero/timeout or
launch failure moves files under `.quarantine` and returns nonretryable `-6`.
`src/engine/finalize.c:110-151` currently uses same-filesystem hard links for
no-replace quarantine names. Windows needs an equivalent collision-safe move,
plus a documented behavior for locked/open files. On macOS, verify hard-link
permissions and private directory mode on supported filesystems. Both ports
must keep scanner argv parsing shell-free and append the file path last
(`src/platform/spawn.c:381-442`).

The native host requires a companion `cdm` executable beside it
(`src/native_host/main_host.c:46-65`, `src/native_host/browser_install.c:82-92`).
The install format differs between Chromium-family and Firefox: the former
uses `allowed_origins`, the latter `allowed_extensions`
(`src/native_host/browser_install.c:94-128`). Preserve the 4-byte native
message length prefix and 1 MiB cap (`src/native_host/main_host.c:22`,
`:67-90`); verify the browser's endianness and stdio behavior on each OS.

## 5. Desktop and packaging choices

Keep `daemon_autostart_command()`'s enable/disable/status behavior
(`src/platform/daemon_autostart.c:458`) while replacing the XDG entry. Windows
startup registration should be per-user and visible to the OS startup UI;
Microsoft documents both per-user Run keys and packaged startup tasks
([Microsoft startup apps](https://learn.microsoft.com/en-us/windows/win32/w8cookbook/startup-apps)).
Select one after deciding whether cdm ships as a conventional installer or
MSIX. On macOS, a user LaunchAgent is appropriate for a user-owned daemon;
Apple documents per-user LaunchAgents and their launchd ownership
([Apple LaunchAgents](https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html)).

The current CMake file unconditionally requires libnotify and libepoxy
(`CMakeLists.txt:60-70`) and installs XDG desktop files plus DEB/RPM metadata
(`CMakeLists.txt:345-405`). Split runtime target dependencies and install rules
by OS. Preserve SQLite >=3.24, libcurl >=7.60, libxml2 >=2.9, OpenSSL Crypto,
SDL2 and Criterion where available (`CMakeLists.txt:60-119`, `:325-341`).
`DOWNLOADMGR_ENABLE_TRAY` is Linux-only already (`CMakeLists.txt:68-72`,
`:206-212`); a new OS tray backend should be an independent feature option.
Do not call DEB/RPM/Arch scripts on other OSes. Decide Windows installer and
macOS app bundle/signing/notarization as separate release work; these are
`UNKNOWN` until tested with native build agents.

## 6. Verification matrix and unresolved decisions

| Gate | Windows | macOS |
|---|---|---|
| Configure/build with warnings and `BUILD_TESTING=ON` | UNKNOWN; native CI runner required | UNKNOWN; native CI runner required |
| Unit tests: thread, file, config, DB, IPC codec, curl, engine | Run all; use 127.0.0.1 fixtures | Run all; use 127.0.0.1 fixtures |
| Integration: daemon singleton, CLI, GUI, native host | Test under a standard user with non-ASCII/space paths | Test under a standard user with a signed bundle path |
| Media: HLS/DASH/site resume, ffmpeg, scanner quarantine | Test cancellation and process-tree cleanup | Test cancellation and launchd process ownership |
| Desktop: autostart, tray, notification, locale/theme | Interactive session required | Interactive session required |
| Browser registration | Installed Chromium-family and Firefox; confirm absolute host path | Installed Chromium-family and Firefox; confirm bundle host path |
| Packaging | Installer install/upgrade/uninstall and no orphan startup/manifest | Bundle/pkg install/upgrade/uninstall, signing and notarization |

Open design choices: Windows installer format (conventional vs MSIX), GUI and
daemon process ownership for tray/notifications, macOS sandboxing/signing
requirements, and whether binary IPC scalar encoding needs an explicit
endianness migration before cross-architecture support. Record decisions in
`docs/open-questions.md` before implementation. Keep Linux tests green during
each platform extraction. Native behavior remains `UNKNOWN` until a runner or
interactive machine executes the matrix above.
