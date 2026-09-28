# Open Questions

## Task 5.1.1 — JSON export scope and schema (resolved)

The brief lists only `src/utils/config.c` and a new
`src/persistence/export.c`, yet requires a CLI command and an optional history
export. The CLI, CMake and tests are necessary for an end-to-end command. A
versioned JSON schema also needs an explicit field list and history source.
The user approved extending to CLI/CMake/tests and reading history through
paged daemon IPC on 2026-09-28. Schema version 1 exports non-secret settings
and optional visible history fields. `--include-secrets` adds stored Cookie,
Referer, extra headers, auth username and proxy credentials with an explicit
warning; HTTP Basic passwords remain unavailable over details IPC and are
never exported. URLs and destination paths may themselves contain private
values; `--include-history` warns. A concurrent queue mutation can change
page membership, and secret export requests one details record per download,
which is slow for large histories. Import round-trip is deferred to 5.1.2.
`TODO(platform)` in `src/persistence/export.c` tracks a private temporary-file
and sync implementation for Windows.

## Task 4.5.1 — Phase 4 release gates

Phase 4 documentation and local fixtures are complete. Debug and ASan builds
each passed 39/39 CTest targets on 2026-09-28. The TSan build succeeded, but
23/39 tests failed before test assertions inside Criterion 2.4.3's runner
(`libcriterion.so.3` initialization/segfault); 16 non-Criterion targets passed.
This repeats the earlier Criterion/TSan environment blocker and does not
establish a cdm race result. A Criterion/TSan-compatible environment is needed
for the formal gate. Manual smoke against real HLS/DASH streams and a real
yt-dlp site selection was not run in this restricted, URL-free test session;
use consented test media and a desktop browser before closing the gate. Task
4.5.1 remains unchecked; work can continue on independent Phase 5 tasks on the
user's single `cdm` branch. No phase merge or push is authorized.

## Task 4.4.1 — Optional site extractor constraints

The site extractor is explicit opt-in, disabled by default, and limited to HTTPS
URLs on YouTube, Vimeo, and Dailymotion hostnames. It passes only the URL to a
startup-resolved yt-dlp executable; captured browser cookies and headers are
discarded. The URL is visible in the child process argument list while running.
The configured global speed cap is applied to each yt-dlp process, so multiple
concurrent site jobs can exceed an aggregate cap. A future shared limiter would
need cross-process accounting. A crash between publishing the completed artifact
and the SQLite checkpoint can leave an orphaned file; a future recovery pass
should identify it using the private `.siteparts` marker. Changing the configured
yt-dlp path requires a daemon restart because executable resolution is cached.
`TODO(platform)` in `src/platform/spawn.c` and `src/engine/site_grab.c` tracks
Windows process launching and secure staging. No site credentials are stored.

## Task 2.2.1 — Equal schedule bounds and platform clock (partly resolved)

Decision (user, 2026-09-25): Reject equal nonempty start/stop times; empty
start and stop mean always active. Overnight windows remain valid. Scheduled
pauses use a persisted marker so a restart resumes only downloads paused by
the schedule. The local wall clock implementation uses POSIX `localtime_r`.
`TODO(platform)` in `src/core/scheduler.c` tracks the Windows `localtime_s`
port if Windows support is implemented later.

## Task 2.1.3 — Queue protocol IDs and priority range (resolved)

The plan assigns queue messages IDs 35–39, but 35–37 already mean paginated
history and download details. Queue commands use IDs 45–49 and protocol version
5. The user chose priorities 0–1000 on 2026-09-25; IPC create, update, and
reorder reject values outside that range. The lower-level queue database API
still accepts signed integers for existing data and migration compatibility.

## Task 1.5.2 — Remove command message ID (resolved)

The plan assigns `MSG_REMOVE_DOWNLOAD = 34`, but `src/platform/ipc_protocol.h`
already assigns 34 to `MSG_SUBSCRIBE_V2`. Reusing 34 would make the protocol
ambiguous. The remove command uses the next free ID, 44, and advertises
protocol version 4. Existing wire message definitions remain unchanged.

## Task 0.2.4 — Filename resolution ownership

The CLI and GUI submit a complete destination path before the daemon performs
the HTTP metadata probe. The daemon therefore cannot distinguish a URL-derived
filename from a caller-chosen filename after enqueueing. The browser confirmation
popup also lets the user edit the offered name before submission.

Decision (user, 2026-09-24): The daemon should rename after probing when the
filename was automatic, while preserving a caller-chosen name. Implementation
must track filename provenance and handle reserved files, DB paths, and already
reported CLI paths. This question is resolved and task 0.2.4 implements this
choice.

## Task 0.5.3 — Meaning and source of the CLI size column

The planned `cdm cli list` columns include `size`, but `MSG_LIST_PAGE` rows only
contain id, URL, destination, status, and progress. The daemon stores
`downloads.total_size`, but that value is not exposed in a list response.
Reading the local destination file would report bytes currently on disk, which
can differ from expected total size. Extending type 35 in place would break
clients already using that wire layout. Options: add a new page message type
with a versioned row that includes total size, or display local file size and
`?` when absent. The choice was raised on 2026-09-25.

Decision (user, 2026-09-25): Use the daemon's recorded total size via a new
versioned IPC message. Task 0.5.3 adds type 36 and leaves type 35 unchanged.

## Task 0.6.3 — Phase 0 merge gates and branch destination

The user instructed work on branch `cdm` and said they will push only after the
full plan; `PLAN.md` instead specifies merging `phase-0-foundations` into
`dev` after phase 0. Do not merge or switch branches without reconciling that
instruction. Continue subsequent independent work on `cdm` as requested.

The phase 0 acceptance checklist asks for CTest count to increase by at least
12; it rose from 24 at baseline to 28 (four additional targets). Inventing
empty or duplicate tests merely to meet the count would not add coverage.
The seven-item reconciliation also leaves plaintext cookies open; a privacy
design and code TODO/defer decision are needed for the formal gate.

Post-phase sanitizer check: the ASan build in `/tmp/cdm-asan-build` passed
28/28. The TSan build compiled, and TSan-instrumented CLI list/browser daemon
integrations completed, but Criterion targets crashed during harness setup
inside `libcriterion`/nanomsg. Browser integration produced TSan reports
inside uninstrumented GLib/GIO invoked by libnotify from
`src/core/scheduler.c:86`. A focused TSan strategy or suppression/notification
isolation needs review before claiming a clean TSan phase gate. The browser
reports were written only in temporary build/test output, not committed.

Task 0.6.3 remains unchecked. Clarify whether the `cdm` branch replaces the
plan's per-phase merges and how to treat the quantitative test-count and TSan
gates before any merge. No push is authorized by the user's latest preference.

## Task 1.2.1 — Credential entry and storage policy

Step 1.2 defines persistence, IPC ingestion, and curl use of per-download HTTP
Basic credentials, but specifies no CLI flag or GUI input for users to provide
them. The IPC API can submit them after task 1.2.1. Decide where the user-facing
entry belongs before calling HTTP Basic auth complete.

The requested `auth_password` database column stores the password in plaintext,
like the existing cookie column. A future credential storage policy should
cover database access permissions, export behavior, and whether encryption or
OS keyring integration is required. The new details response exposes only
`auth_user` and a boolean indicating whether a password exists.

## Task 2.3.1 — Post-action platform support

`src/platform/spawn.c` leaves `TODO(platform)` for a safely quoted Windows
process launch and for macOS shutdown/sleep integration. Linux invokes
`systemctl poweroff` or `systemctl suspend` only when the matching
`[post_actions]` flag is explicitly enabled. Decide the native platform
equivalents before enabling these actions on Windows or macOS.

## Task 2.7.1 — TSan verification environment

The phase-2 TSan build succeeds, but 20 Criterion test targets abort before
assertions because Criterion cannot initialize its inheritable arena or crashes
inside `libcriterion.so.3`. Debug and ASan suites both pass 33/33. Which
Criterion/TSan-compatible runner should be used for the release gate? Repeat
the full TSan suite there before treating it as race-clean.

## Task 3.1.4 — Ephemeral context scope and restart marker

The brief lists IPC, worker pool, and popup files, but `RequestOptions` is
persisted and caps Cookie at 1024 bytes while the agreed browser contract allows
4096. May this task extend queue/engine/curl interfaces and persist only a
`requires_browser_context` boolean (never captured values), so restart/resume
requires a fresh browser offer? Root AGENTS.md restricts edits to listed files;
The user approved this expansion and boolean marker on 2026-09-28; captured
values remain memory-only. The implementation uses ephemeral `RequestOptions`
and refuses resume after context loss until an explicit fresh offer confirms.

## Task 3.3.1 — Link refresh scope

The listed IPC/GUI files omit the protocol header, curl effective-URL output,
and queue/persistence APIs needed for a safe, atomic metadata refresh. May this
task extend those files and integration tests while retaining an ID-only request
that resolves the existing URL through redirects? Contextual redirects still
require a fresh browser offer under the agreed privacy policy. Awaiting user
scope decision; proceed with independent task 3.4.1 meanwhile.
Resolved: user approved the expanded scope and existing-URL redirect resolution
on 2026-09-28. Contextual redirects retain the fresh-offer requirement.

## Task 3.5.1 — Installed-browser verification gate

Debug build/CTest and JS/native/HTTP/IPC/installer fixtures pass. Chromium and
Firefox executables are available, but no installed-browser visual/handshake
smoke was performed. Chrome, Edge, Brave, Opera and Vivaldi are unavailable.
Run the local-fixture consent/menu/filter/exclusion/refresh smoke checklist in
`docs/browser-integration.md` on all seven browsers before closing this gate.
Phase 3 documentation is updated; task 3.5.1 stays unchecked for this gate.

## Task 4.1.1 — Media detection offer policy

The brief matches video/* responses, which includes individual streaming
segments. Should the opt-in detector collect bounded candidates and send a
media_offer after user selection, or automatically open confirmations for each
matched response? Automatic offers can flood the desktop on segmented streams.
Resolved: user chose collection and offer only after selection on 2026-09-28.
The options page lists a bounded, expiring in-memory collection; detection alone
does not open native offers. Browser-session context remains separately gated.

## Task 4.2.2 — HLS dispatch and AES dependency scope

The brief lists only hls.c/hls.h. Connecting confirmed HLS offers needs IPC
confirmation, persisted media kind, queue/engine dispatch, and integration tests;
AES-128 needs an audited crypto implementation (proposed OpenSSL dependency).
Asked whether to extend scope for working end-to-end downloads or keep the
standalone downloader API for now. Resolved: user approved working end-to-end
HLS dispatch, persistence and OpenSSL dependency on 2026-09-28.

## Task 4.2.2 — Platform and supported-stream constraints

TODO(platform): secure HLS staging, file locking and resume need a Windows
implementation; the current downloader uses POSIX files and flock.
Current scope supports finite ENDLIST VOD, a selected master variant, AES-128
identity keys, and whole-file initialization maps. Byte ranges, discontinuities,
DRM, live reload and separate HLS audio renditions are not implemented.
Captured browser context is restricted to its original origin and does not
follow redirects; cross-origin resources require a future context design.
State JSON stores layout/key/file hashes and HTTP validators, never captured
header values or AES key bytes. Missing validators force a segment re-download.
Native output is concatenated media bytes; optional remux is task 4.2.3.

## Task 4.2.3 — Remux startup and destination scope

The brief lists hls.c/spawn.c but asks for startup presence caching and a new
MP4 output. Asked to include spawn.h/daemon startup and queue/DB path updates,
publish successful remuxes to a uniquely reserved .mp4 destination, and retain
TS on missing/failed ffmpeg. Resolved: user approved the expanded scope and
unique .mp4 publication policy on 2026-09-28.

## Task 4.2.3 — Remux platform constraint

TODO(platform): argv-safe ffmpeg spawning, waiting and cancellation on Windows
are not implemented; remux falls back to native output there. POSIX runs fixed
argv via posix_spawn, limits demuxers/protocols, reaps the child, and honors
pause/cancel and the configured transfer timeout. Explicit checksum verification
retains native bytes so remux cannot invalidate the requested checksum.

## Task 4.3.1 — XML dependency and supported layouts

Resolved: user approved libxml2 and CMake/package dependency scope on
2026-09-28. Parser uses system libxml2 >= 2.9 with forced UTF-8, DTD rejection,
no expansion/DTD/XInclude flags, NONET, and NO_XXE on libxml2 >= 2.13.
Current bounded scope is static single-period audio/video, whole-file
SegmentList and inherited SegmentTemplate Number/Time addressing. Dynamic,
DRM, SegmentBase, byte ranges/indexes, multiple BaseURL choices, external
xlink/xml:base, nonzero period start/eptDelta and calendar ISO durations are
rejected explicitly. Downloader/merge wiring is task 4.3.2.

## Task 4.3.2 — Dispatch, merge and retained-output scope

Resolved: user approved engine/browser dispatch, two-input ffmpeg spawning,
cleanup/tests and persistence of the retained companion audio path on
2026-09-28. SQLite v12 companion_path is internal; raw wire structs are unchanged.
DASH reuses HLS whole-file asset jobs and private validator/hash resume state.
Successful merge publishes one unique MP4; missing/failed ffmpeg publishes
native video/audio separately, atomically records both paths, and keeps them.
A supplied checksum applies to the final primary file (merged when available).
TODO(platform): secure DASH staging/publication needs a Windows implementation.
The static parser restrictions from 4.3.1 also apply to the downloader; companion
path display in the GUI/CLI and crash-orphan staging discovery remain future work.
