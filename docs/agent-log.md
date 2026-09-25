## 2026-09-24 — Codex

Branch: `cdm` (user-requested in place of `phase-0-foundations`)
Commit: `fix(thread): align dm_mutex_destroy signature across platforms` (this entry's commit)

Tasks completed:
- 0.1.1 Align the Windows `dm_mutex_destroy` definition with the `void` declaration and POSIX implementation.

Tests:
- Baseline: `cmake --build build -j` and CTest 24/24 passed.
- After change: `cmake --build build -j` and CTest 24/24 passed.
- Smoke: `./build/tests/test_thread --filter thread/mutex_reinitialize_after_destroy --verbose` passed (1/1).
- Windows cross-compiler was unavailable, so the Windows translation unit was not compiled locally. The test-first case passed on Linux; the pre-fix Windows definition conflicts with the header at compile time.
- Criterion requires local IPC sockets; sandboxed runs failed at socket bind and the successful runs used the permitted test environment.

Open questions:
- None for task 0.1.1.

Next:
- 0.1.2 Fix CLI `--header` dangling pointer.

## 2026-09-24 — Codex, task 0.1.2

Branch: `cdm`
Commit: `fix(cli): own --header buffer in caller, not callee stack` (this entry's commit)

Tasks completed:
- 0.1.2 Move repeated CLI header storage to a caller-owned buffer that remains live through IPC serialization.

Tests:
- Test first: new real-daemon CLI integration test passed on the ordinary build but failed on the unfixed ASan build with `stack-use-after-return` in `ipc_send_add_download`.
- After fix: focused ASan test passed with `detect_stack_use_after_return=1`.
- Full `cmake --build build -j` and CTest: 25/25 passed.
- Smoke: integration test runs `cdm cli add` against `127.0.0.1` and verifies both repeated headers through `MSG_GET_DETAILS`.

Open questions:
- None for task 0.1.2.

Next:
- 0.1.3 Fix unknown-size full-file request.

## 2026-09-24 — Codex, task 0.1.3

Branch: `cdm`
Commit: `fix(engine): accept unknown-size transfers when total is absent` (this entry's commit)

Tasks completed:
- 0.1.3 Mark unknown-size whole-file ranges and require a successful HTTP/curl transfer without comparing against a fabricated one-byte range.

Tests:
- Test first: local HTTP integration route omitted `Content-Length` and returned a 21-byte body; the original engine failed with HTTP 200 and curl success.
- After fix: focused unknown-size transfer test passed and verified the file bytes.
- Full `cmake --build build -j` and CTest: 25/25 passed.

Open questions:
- None for task 0.1.3.

Next:
- 0.1.4 Make CLI add fail on a rejected ID.

## 2026-09-24 — Codex, task 0.1.4

Branch: `cdm`
Commit: `fix(cli): report rejected add as failure with non-zero exit` (this entry's commit)

Tasks completed:
- 0.1.4 Return failure and suppress the success line when the daemon returns download ID zero.

Tests:
- Test first: new daemon/CLI integration test rejected a destination outside `DOWNLOADMGR_ROOT`; the old CLI returned zero and printed `Download added (ID: 0, ...)`.
- After fix: focused rejection test passed; full build and CTest passed 26/26.
- Smoke: the test runs the real CLI and daemon over a local Unix socket, with a loopback-only URL.

Open questions:
- None for task 0.1.4.

Next:
- 0.1.5 Handle truncated fallback socket paths.

## 2026-09-24 — Codex, task 0.1.5

Branch: `cdm`
Commit: `fix(ipc): handle truncated fallback socket and lock paths` (this entry's commit)

Tasks completed:
- 0.1.5 Select a shorter HOME or `/tmp` IPC base when a runtime/home socket path cannot fit `sockaddr_un`, and validate formatted temporary socket/lock paths.

Tests:
- Test first: new daemon integration test passed without `XDG_RUNTIME_DIR` but failed with an overlong runtime directory because the daemon exited before HOME fallback.
- After fix: focused fallback smoke test passed, IPC source compiled without the two `-Wformat-truncation` warnings, and the full build/CTest passed 27/27.

Open questions:
- None for task 0.1.5.

Next:
- 0.1.6 Reset the chunk plan before single-stream fallback.

## 2026-09-24 — Codex, task 0.1.6

Branch: `cdm`
Commit: `fix(engine): reset chunk plan before single-stream fallback` (this entry's commit)

Tasks completed:
- 0.1.6 Delete persisted/in-memory segmented chunks before a whole-file fallback, set its known-size end correctly, and remove a failed fallback file so the retry starts fresh.

Tests:
- Test first: mocked parallel failure followed by a partial single-stream failure failed on the old `end=0` fallback range.
- After fix: the focused test confirmed no stale chunks or file, then verified all 4 MiB of the fresh retry.
- Full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.1.6.

Next:
- 0.1.7 Protect rebalance split mutations with the queue mutex.

## 2026-09-24 — Codex, task 0.1.7

Branch: `cdm`
Commit: `fix(engine): lock queue around rebalance split mutation` (this entry's commit)

Tasks completed:
- 0.1.7 Serialize rebalance split mutations with queue snapshots using the queue mutex, while releasing the pool mutex around the split callback.

Tests:
- Test first: a localhost 8 MiB range transfer triggered work stealing and failed because the split callback ran without the mutation lock.
- After fix: focused transfer passed with concurrent interval sampling and an exact, gap-free interval coverage check.
- Full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.1.7.

Next:
- 0.2.1 Add GET `Range: 0-0` fallback to HEAD probing.

## 2026-09-24 — Codex, task 0.2.1

Branch: `cdm`
Commit: `feat(curl): fall back to GET Range 0-0 when HEAD is rejected` (this entry's commit)

Tasks completed:
- 0.2.1 Retry failed HEAD probes with `GET Range: bytes=0-0`, derive complete size from `Content-Range` on 206 or `Content-Length` on an ignored 200, and stop reading the response body immediately.

Tests:
- Test first: a localhost route returning HEAD 405 and GET 206 failed before implementation.
- After fix: both the 206 route and an ignored-range 200 route passed; the existing curl failure test now uses 127.0.0.1.
- Full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.2.1.

Next:
- 0.2.2 Parse `Content-Disposition` filenames.

## 2026-09-24 — Codex, task 0.2.2

Branch: `cdm`
Commit: `feat(path): derive filename from Content-Disposition` (this entry's commit)

Tasks completed:
- 0.2.2 Capture `Content-Disposition` from the final probe response and parse plain or UTF-8 extended filenames, preferring valid `filename*` and rejecting path separators, traversal dots, controls, and malformed encoding.

Tests:
- Test first: parser and FileInfo capture tests failed to compile because the interfaces did not exist.
- A focused parser run found and fixed a partial decoded filename overriding a valid fallback.
- Full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.2.2.

Next:
- 0.2.3 Percent-decode URL-derived filenames.

## 2026-09-24 — Codex, task 0.2.3

Branch: `cdm`
Commit: `feat(path): percent-decode URL-derived filenames safely` (this entry's commit)

Tasks completed:
- 0.2.3 Decode safe percent escapes once in the URL path segment after stripping query and fragment; preserve encoded separators and invalid escapes, and avoid traversal filenames.

Tests:
- Test first: a URL with `%20` failed the new filename test.
- After fix: focused filename decoding passed; full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.2.3.

Next:
- 0.2.4 Wire filename resolution into engine and CLI.

## 2026-09-24 — Codex, task 0.3.1

Branch: `cdm`
Commit: `feat(db): store etag and last_modified for resume validation` (this entry's commit)

Tasks completed:
- 0.3.1 Add default-empty `etag` and `last_modified` columns to fresh and migrated downloads tables; advance SQLite `user_version` to 2.

Tests:
- Test first: a legacy DB row migration test failed because the schema version was 1.
- After fix: old row retained its status and gained empty validator columns; full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- The 0.2.4 filename ownership question was answered by the user: daemon renames after probe for automatic names.

Next:
- Return to 0.2.4 with the selected daemon-side design.

## 2026-09-24 — Codex, task 0.2.4

Branch: `cdm`
Commit: `feat(engine): use Content-Disposition and decoded URL for names` (this entry's commit)

Tasks completed:
- 0.2.4 Add `MSG_ADD_DOWNLOAD_AUTO` (type 32, same payload as add) for CLI and normal GUI adds; browser-confirmed and retry paths keep their explicit filenames.
- Persist automatic filename provenance (`auto_filename`, schema version 3), resolve a safe `Content-Disposition` name after daemon probing, claim the new path without overwriting an existing file, and update DB and in-memory destination together under the queue mutex.
- CLI reports its path as provisional because the final name can change after probing.

Tests:
- Test first: an engine test for a reserved automatic destination failed to compile before the provenance field existed.
- Engine tests cover automatic rename and explicit-name preservation; SQLite test covers provenance across restart and clearing after resolution.
- CLI integration uses 127.0.0.1 and verifies the final file appears under the server name while the provisional URL name disappears.
- Full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.2.4; the user selected daemon-side rename.

Next:
- 0.3.2 Capture validators during probe.

## 2026-09-24 — Codex, task 0.3.2

Branch: `cdm`
Commit: `feat(curl): capture etag and last-modified during probe` (this entry's commit)

Tasks completed:
- 0.3.2 Capture raw ETag and Last-Modified values from the final HEAD or GET probe response, persist changes in SQLite, and restore them into Download state.

Tests:
- Test first: the local HTTP integration test failed to compile before `FileInfo` had validator fields.
- Focused curl, engine, and DB tests passed after implementation. One full-suite run saw an intermittent engine test failure; a retry passed. The engine tests had a shared `/tmp/test_engine_out` teardown path, so the success test now uses a PID-based path and cleans up its own file.
- Final `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.3.2.

Next:
- 0.3.3 Send `If-Range` on resume.

## 2026-09-25 — Codex, task 0.3.3

Branch: `cdm`
Commit: `feat(engine): validate resume with If-Range and validators` (this entry's commit)

Tasks completed:
- 0.3.3 Send the stored strong ETag or Last-Modified in `If-Range` on resumed range requests. Abort a full HTTP 200 response before writing, discard stale chunks and file, and retry from byte zero.

Tests:
- Test first: local `127.0.0.1` HTTP integration cases for matching and stale validators both failed before implementation.
- After implementation, both focused cases passed; `cmake --build build -j` and CTest passed 27/27.
- Smoke: the same integration cases verified the final file contains all old bytes for a matching validator and all new bytes after a stale validator triggers restart.

Open questions:
- None for task 0.3.3.

Next:
- 0.3.4 Stale-validator refusal path.

## 2026-09-25 — Codex, task 0.3.4

Branch: `cdm`
Commit: `feat(engine): restart on validator mismatch instead of resuming` (this entry's commit)

Tasks completed:
- 0.3.4 Compare stored ETag and Last-Modified with fresh probe metadata before updating stored validators. On a mismatch, clear chunks and the partial file and begin a fresh download.

Tests:
- Test first: a local `127.0.0.1` server changed its ETag while preserving size and rejected resumed GETs; the existing engine failed.
- After implementation, the focused test passed, showing a fresh GET at byte zero; full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.3.4.

Next:
- 0.4.1 Add protocol version and handshake.

## 2026-09-25 — Codex, task 0.4.1

Branch: `cdm`
Commit: `feat(ipc): add protocol version and HELLO handshake` (this entry's commit)

Tasks completed:
- 0.4.1 Add `MSG_HELLO` (41) with a uint16 version response. Keep the v1 `MsgHeader` at 8 bytes to honor the wire compatibility rule. CLI, GUI, browser popup, and native host negotiate on connect; they reconnect and use v1 messages when an older daemon rejects HELLO.
- Long-lived GUI listener connections restore blocking reads after the bounded handshake.

Tests:
- Test first: the handshake test failed to compile before `IPC_PROTOCOL_VERSION` and `ipc_client_hello` existed.
- Local IPC integration verifies version 2 and a legacy 8-byte `MSG_LIST` frame on the same connection, plus blocking reads for the compatible listener connection.
- Full `cmake --build build -j` and CTest passed 27/27. CLI, native host, browser, and daemon integration tests exercise negotiated connections.

Open questions:
- None for task 0.4.1; the explicit wire compatibility rule takes precedence over the plan's request to add a raw field to `MsgHeader`.

Next:
- 0.4.2 Extend status event with rich fields.

## 2026-09-25 — Codex, task 0.4.2

Branch: `cdm`
Commit: `feat(ipc): add versioned progress event with speed and eta` (this entry's commit)

Tasks completed:
- 0.4.2 Add `IpcProgressV2` with received bytes, total bytes, speed, ETA, progress, status, and error fields. Emit it before the legacy status frame for clients that explicitly use `MSG_SUBSCRIBE_V2`; existing `MSG_SUBSCRIBE` clients receive only v1.
- Assign `MSG_STATUS_EVENT_V2 = 33` and `MSG_SUBSCRIBE_V2 = 34` because type 32 already belongs to `MSG_ADD_DOWNLOAD_AUTO`; retain all existing numeric values.
- Speed is zero and ETA is `UINT64_MAX` until task 0.4.3 computes these values. Unknown total produces indeterminate progress (-1).

Tests:
- Test first: the new v2 integration assertions failed to compile before the protocol types and subscribe function existed.
- Local IPC test validates v2 fields, followed by a v1 fallback frame, and verifies a separate legacy subscriber receives only v1.
- Full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.4.2; type 33 preserves the already published type 32 assignment.

Next:
- 0.4.3 Compute speed and ETA in the daemon.

## 2026-09-25 — Codex, task 0.4.3

Branch: `cdm`
Commit: `feat(scheduler): compute per-download speed and eta in daemon` (this entry's commit)

Tasks completed:
- 0.4.3 Sample received bytes on each daemon progress tick using CLOCK_MONOTONIC. Seed from the first measured interval, then smooth with an EMA (alpha 0.3). Publish rounded-up ETA when total and speed are known, otherwise UINT64_MAX.
- Store per-download samples under the queue mutex, reset them on a new active run, and put computed speed/ETA into the opt-in v2 progress event.

Tests:
- Test first: synthetic speed/ETA test failed to compile before the metrics type and calculation existed.
- Synthetic samples verify EMA, ETA, unknown total, and byte-counter reset. The local IPC integration verifies computed fields propagate into the v2 payload.
- Full `cmake --build build -j` and CTest passed 27/27.

Open questions:
- None for task 0.4.3.

Next:
- 0.4.4 Update GUI to consume v2 events.

## 2026-09-25 — Codex, task 0.4.4

Branch: `cdm`
Commit: `feat(gui): show speed, eta and size from v2 progress events` (this entry's commit)

Tasks completed:
- 0.4.4 Subscribe to v2 status frames when the daemon reports protocol version 2, parse them into controller and model events, and drain the following v1 fallback frame. Older daemons still use v1 subscription and clear stale v2 statistics.
- Preserve transfer fields across periodic list snapshots. Render received/total bytes, daemon speed, ETA, and an error reason in the existing Nuklear row theme. Add byte and ETA formatting helpers.

Tests:
- Test first: synthetic v2 controller/model test failed to compile before new fields and formatting helpers existed.
- Focused model test covers v2 fields, snapshot retention, v1-only fallback, byte formatting, and ETA formatting.
- Full `cmake --build build -j` and CTest passed 27/27.
- Isolated offscreen GUI smoke ran for two seconds with a loopback download row present. This verified the GUI event loop and row rendering path without using the user's daemon.

Open questions:
- None for task 0.4.4.

Next:
- 0.4.5 Update browser popup to consume v2 events.

## 2026-09-25 — Codex, task 0.4.5

Branch: `cdm`
Commit: `feat(browser-popup): show speed and eta from v2 events` (this entry's commit)

Tasks completed:
- 0.4.5 Let a browser-specific progress subscriber opt into a v2 frame for its own download while retaining the legacy browser progress frame and path/control data. The popup parses both frame sizes and displays daemon speed and ETA; a v1 daemon retains the existing fallback display.
- Keep native-host browser subscribers on their legacy frame because they do not opt into v2.

Tests:
- Test first: the same-socket browser/v2 subscription expected a v2 frame but received only the browser frame before implementation.
- Local IPC integration verifies the v2 frame followed by the browser frame; full `cmake --build build -j` and CTest passed 27/27.
- `python3 tests/smoke_browser_popup.py build/cdm` passed offscreen with an isolated daemon and a 127.0.0.1 origin. It verified popup readiness and received speed/ETA fields on the popup's v2 progress stream. Visual inspection of the rendered labels was not available in the offscreen run.

Open questions:
- None for task 0.4.5.

Next:
- 0.4.6 Document IPC v2.

## 2026-09-25 — Codex, task 0.4.6

Branch: `cdm`
Commit: `docs(ipc): document protocol v2 and v1 compatibility` (this entry's commit)

Tasks completed:
- 0.4.6 Documented the full current message registry, request framing and raw response layouts, v2 negotiation and paired event subscriptions, field units and sentinel values, browser offer payloads, ABI limitations, and v1 compatibility rules in `docs/ipc.md`.

Tests:
- Documentation registry check found all 21 `MSG_*` enum values in the document.
- `cmake --build build -j` and full CTest passed 27/27.

Open questions:
- None for task 0.4.6.

Next:
- 0.5.1 Add offset/limit pagination.

## 2026-09-25 — Codex, task 0.5.1

Branch: `cdm`
Commit: `feat(ipc): add paginated history listing` (this entry's commit)

Tasks completed:
- 0.5.1 Added `MSG_LIST_PAGE = 35` with offset/limit request and total/returned/rows response, capped at 500 rows. The existing `MSG_LIST_ALL` remains a 200-row legacy shim. Added the client helper and database page visitor and documented the new wire type.

Tests:
- Test first: new 600-row IPC test failed to build because `ipc_send_list_page` was absent.
- Focused test then passed, covering two pages without gaps/duplicates, exhausted offset, 500-row cap, and legacy 200-row response.
- `cmake --build build -j` and full CTest passed 27/27. Focused IPC test also served as the affected-surface smoke check.

Open questions:
- None for task 0.5.1. Type 35 is new within protocol v2; an earlier v2 daemon closes on it, so clients should reconnect and use the legacy type 9 response in that case.

Next:
- 0.5.2 Update GUI to page.

## 2026-09-25 — Codex, task 0.5.2

Branch: `cdm`
Commit: `feat(gui): paginate history list on scroll` (this entry's commit)

Tasks completed:
- 0.5.2 Moved history reads to the GUI controller worker. The all-history view loads 64 rows initially, adds 64 as the user scrolls near the end, and advances a bounded 256-row window after it fills. It displays loading text and a loaded/total count, and adjusts scroll position when the window advances. The GUI client reconnects and uses the legacy list response when an older daemon rejects type 35.

Tests:
- Test first: a 600-row history-window test failed to build before the new controller API existed; it then passed and checks the 256-row bound and complete traversal.
- `cmake --build build -j` and full CTest passed 27/27.
- An isolated offscreen GUI stayed live with a temporary daemon and 600 completed database rows. The headless smoke did not visually exercise scrolling.

Open questions:
- None for task 0.5.2. Category, search, and status filters apply to the currently loaded window; a future server-side filter protocol would be needed for global filtered history.

Next:
- 0.5.3 Add a CLI list command.

## 2026-09-25 — Codex, task 0.5.3

Branch: `cdm`
Commit: `feat(cli): add paginated list command` (this entry's commit)

Tasks completed:
- 0.5.3 Added `cdm cli list [--offset N] [--limit N] [--status S]` with tab-separated id, status, percent, recorded total size in bytes, and filename. Status filtering scans all daemon pages before applying the filtered offset. Zero stored size prints `?`.
- Following the user's choice, added `MSG_LIST_PAGE_WITH_SIZE = 36` with an extra `uint64_t total_size` per row; kept type 35 byte-compatible. Documented the protocol and resolved the size question in `docs/open-questions.md`.

Tests:
- Test first: isolated 600-row CLI integration failed because `list` was unknown.
- After implementation the focused integration passed, checking rows 100..1 beyond the first 500, recorded sizes, and a filtered 50-row slice.
- `cmake --build build -j` and full CTest passed 28/28. The focused integration is also the CLI smoke check.

Open questions:
- None for task 0.5.3.

Next:
- 0.6.1 Update `PROJECT_CONTEXT.md`.

## 2026-09-25 — Codex, task 0.6.1

Branch: `cdm`
Commit: `docs: refresh PROJECT_CONTEXT for phase 0` (this entry's commit)

Tasks completed:
- 0.6.1 Refreshed the self-contained project audit with the current branch/commit, complete current tracked-file inventory, v2 IPC including page types 35/36, validator and filename provenance columns, current engine/CLI/GUI behavior, test inventory, fixed-vs-open risks, and reconciliation of the seven supplied claims.
- Kept the absent Claude report as `UNKNOWN` with instructions for verification. Phase 0 added no new config keys; the settings section states that explicitly.

Tests:
- Checked that all 146 files tracked before this document's commit appear in the inventory, plus this document; all 23 IPC enum names appear; all 23 required numbered sections and code fences are structurally present.
- `cmake --build build -j` and full CTest passed 28/28.

Open questions:
- The original Claude report text was not provided; Appendix B remains `UNKNOWN`.

Next:
- 0.6.2 Add phase 0 release notes.

## 2026-09-25 — Codex, task 0.6.2

Branch: `cdm`
Commit: `docs: add phase 0 release notes` (this entry's commit)

Tasks completed:
- 0.6.2 Added an Unreleased section covering probe/naming/resume fixes, engine and CLI fixes, rich progress, paginated history, and current local verification. Corrected the draft package version to the configured 0.3.0-rc1 without claiming publication.

Tests:
- `cmake --build build -j` and full CTest passed 28/28. Reviewed the rendered Markdown structure and configured release version.

Open questions:
- None for task 0.6.2.

Next:
- 0.6.3 Merge phase 0: plan names `phase-0-foundations` into `dev`, but the user requested the `cdm` branch. Reconcile branch/merge instruction against the user's branch preference before changing branches.

## 2026-09-25 — Codex, phase 0 post-check

Branch: `cdm`
Commit: `docs(plan): record phase 0 merge gate blockers` (this entry's commit)

Tasks completed:
- Audited task 0.6.3 without merging or pushing. It remains unchecked because the user requested continued work on `cdm`, CTest increased only from 24 to 28 against a +12 acceptance gate, plaintext cookies remain unresolved, and TSan cannot yet be called clean.
- Recorded those issues and the sanitizer evidence in `docs/open-questions.md` so the next independent task can proceed.

Tests:
- Debug CTest 28/28 and ASan CTest 28/28 passed.
- TSan build succeeded. Criterion-based TSan cases crashed in harness setup; daemon CLI list and browser integration completed, but browser integration emitted TSan reports in GLib/GIO through libnotify. No new cdm-source TSan race was isolated from that run.

Open questions:
- Task 0.6.3 merge destination, CTest +12 gate, plaintext-cookie defer/fix, and TSan gate strategy.

Next:
- 1.1.1 Extend config with proxy fields on the user-selected `cdm` branch; 0.6.3 remains deferred.

## 2026-09-25 — Codex, task 1.1.1

Branch: `cdm`
Commit: `feat(config): add proxy settings` (this entry's commit)

Tasks completed:
- 1.1.1 Added HTTP and SOCKS5 proxy mode, URL, username, and password to the config model and TOML load/save path. Invalid mode or missing scheme/host falls back to no proxy. Proxy state is guarded by a mutex.

Tests:
- Test first: `test_config` failed to compile before the fields were implemented.
- `cmake --build build -j` and full CTest passed 29/29. Focused `test_config` passed and exercises the config file save/load surface.

Open questions:
- None for this task. Existing GUI settings save needs to preserve or edit proxy values in task 1.1.3.

Next:
- 1.1.2 Apply proxy to all curl handles. Phase 0 merge gate 0.6.3 remains deferred as logged above.

## 2026-09-25 — Codex, task 1.1.2

Branch: `cdm`
Commit: `feat(curl): route probe and workers through configured proxy` (this entry's commit)

Tasks completed:
- 1.1.2 Added a shared libcurl proxy helper for HTTP and SOCKS5 with remote DNS, proxy credentials, and explicit bypass of environment `NO_PROXY` for configured proxies. Probe and worker curl handles use the same config snapshot path.

Tests:
- Test first: a loopback authenticated HTTP proxy test failed at the probe because the configured proxy was not applied.
- After implementation: HTTP proxy HEAD plus download and a local SOCKS5 remote-hostname handshake passed. Full build and CTest passed 29/29.

Open questions:
- None for task 1.1.2.

Next:
- 1.1.3 GUI proxy fields. Phase 0 merge gate 0.6.3 remains deferred.

## 2026-09-25 — Codex, task 1.1.3

Branch: `cdm`
Commit: `feat(gui): add proxy settings to settings dialog` (this entry's commit)

Tasks completed:
- 1.1.3 Added proxy mode, URL, username, and masked native password entry to the existing Nuklear settings dialog. The dialog shows an inline URL error and saves a complete config snapshot, preserving proxy values when other settings change.

Tests:
- `cmake --build build -j` and full CTest passed 29/29.
- GUI startup smoke with an isolated temporary home and daemon stayed active for three seconds and loaded its embedded font atlas; both processes were terminated afterward. Interactive settings clicks were not automated in this environment.

Open questions:
- None for this task.

Next:
- 1.2.1 Per-download credentials. Phase 0 merge gate 0.6.3 remains deferred.

## 2026-09-25 — Codex, task 1.2.1

Branch: `cdm`
Commit: `feat(auth): support per-download http basic credentials` (this entry's commit)

Tasks completed:
- 1.2.1 Added per-download Basic auth user/password to request options, SQLite schema version 4 and migration, queue restore, and IPC add options JSON. New `MSG_GET_DETAILS_V2` returns username and a `has_password` flag while the legacy details response remains unchanged and neither response sends the password.
- Recorded the missing CLI/GUI credential entry step and plaintext storage policy in `docs/open-questions.md`.

Tests:
- Test first: DB and IPC credential cases failed to compile without the new fields and helper. Focused tests pass after implementation.
- Isolated each Criterion IPC test socket before its first path lookup to prevent parallel test collisions. Full build and CTest passed 29/29. The IPC test exercises add, persisted queue options, both details message types, and password redaction.

Open questions:
- See task 1.2.1 in `docs/open-questions.md` for credential entry and storage policy.

Next:
- 1.2.2 Apply credentials in curl. Phase 0 merge gate 0.6.3 remains deferred.

## 2026-09-25 — Codex, task 1.2.2

Branch: `cdm`
Commit: `feat(curl): send basic auth on probe and workers` (this entry's commit)

Tasks completed:
- 1.2.2 Applied per-download HTTP Basic auth to probe and worker curl handles via a shared helper. Explicitly disabled forwarding credentials on redirects to a different origin.

Tests:
- Test first: local Basic auth integration case failed to compile before request context fields existed. After implementation it verifies an unauthenticated 401, authenticated HEAD, authenticated file bytes, and redirect to another `127.0.0.1` port without an Authorization header.
- `cmake --build build -j` and full CTest passed 29/29.

Open questions:
- User-facing credential entry and plaintext storage policy remain recorded under task 1.2.1 in `docs/open-questions.md`.

Next:
- 1.3.1 Config keys for connections, User-Agent and timeouts. Phase 0 merge gate 0.6.3 remains deferred.

## 2026-09-25 — Codex, task 1.3.1

Branch: `cdm`
Commit: `feat(config): add connection, user-agent and timeout knobs` (this entry's commit)

Tasks completed:
- 1.3.1 Added download connection cap, User-Agent, connect timeout and transfer timeout settings with defaults 8, `cdm/0.1`, 10 seconds and 30 seconds. TOML loader clamps numeric values to 1..16, 1..600 and 1..3600 respectively; empty User-Agent falls back to default. New shared values use the existing network settings mutex.

Tests:
- Test first: new config tests failed to compile before fields existed. Round-trip and boundary clamp test passed after implementation. Full build and CTest passed 29/29.

Open questions:
- None for this task. These config values are wired into segmenter and curl in task 1.3.2.

Next:
- 1.3.2 Use connection cap, User-Agent and timeouts in segmenter and curl. Phase 0 merge gate 0.6.3 remains deferred.
