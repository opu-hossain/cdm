# Open Questions

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
