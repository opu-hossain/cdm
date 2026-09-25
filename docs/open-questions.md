# Open Questions

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
