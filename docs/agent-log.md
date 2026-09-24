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
