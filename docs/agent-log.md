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
