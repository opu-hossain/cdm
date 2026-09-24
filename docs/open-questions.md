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
