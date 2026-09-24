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
