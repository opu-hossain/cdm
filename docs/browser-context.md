# Browser request context forwarding (task 3.1.1)

## Consent and data flow

Request context is **off by default**. The extension keeps a user-facing
`Use browser session for cdm downloads` toggle. Turning it on is a user gesture:
explain that cookies can authenticate as the user, then request `cookies` and
`webRequest` plus host access for the selected site. A refused permission leaves
the toggle off. Capture only for an actual HTTP(S) download offer after the
toggle and relevant host grant are active. The native cdm popup says `Includes
browser cookies` (and indicates available Referer/User-Agent) without showing
values; Save confirms the transfer, Dismiss discards it. Never auto-add. Turning
the toggle off stops collection immediately and clears extension-side pending
context. Do not persist the toggle as a blanket host permission grant beyond
what the browser stores; revocation must be handled on each offer.

Data path: browser `downloads.onCreated` + observed request headers → extension
offer JSON → native messaging host → daemon versioned browser-offer IPC →
daemon-owned pending offer → explicit popup confirmation → ephemeral per-download
context → curl request → zero/clear on finalization, cancellation, error, or
daemon shutdown. Pausing may retain context in daemon memory for same-process
resume; crash/restart loses it and must require a fresh browser offer before a
protected download resumes. Neither browser extension storage nor SQLite may
hold cookies, User-Agent, or Referer captured by this feature.

| Field | Source and limit | Handling |
|---|---|---|
| Cookie | `cookies.getAll({url})` for the offered URL after grant; UTF-8 bytes capped at 4096 | Build one `Cookie: name=value; …` header, reject CR/LF and oversize. Do not include unrelated domains or duplicate an observed Cookie header. |
| User-Agent | Matching `webRequest.onSendHeaders` header; 256 bytes | Forward as curl's User-Agent only for this download. If unavailable, use cdm's configured default. |
| Referer | Matching `webRequest.onSendHeaders` header, falling back to `downloadItem.referrer`; 2048 bytes | Forward as curl Referer only. Preserve the original HTTP spelling in JSON (`referer`); map explicitly to cdm's `referrer` field. |

Correlate `onSendHeaders` with the `downloads` offer by browser request ID or a
bounded URL/time lookup; do not attach headers from a different tab, redirect,
incognito store, or profile. If correlation is uncertain, omit the field. The
extension must not claim exact browser request parity: partitioned cookies,
third-party rules, redirects, and browser-restricted headers can differ.

## Wire and ownership contract

Current native messages are length-prefixed JSON with a 1 MiB frame cap
(`src/native_host/main_host.c:19,70`). Daemon IPC browser-offer JSON is capped at
16 KiB (`src/platform/ipc_protocol.h:82`); reject an oversize context before
sending to the daemon rather than truncate it. Validate UTF-8/string type,
per-field byte caps, no NUL/CR/LF in header values, and the combined serialized
frame. Existing `IpcBrowserOffer` is a raw-wire reply struct
(`src/platform/ipc_socket.h:29`); **do not add fields to it**. Add a distinct
versioned offer request and, if the popup needs a context-present flag, a
versioned reply type/message. Retain the v1 offer flow for older peers without
context. Keep sensitive fields outside the raw-wire popup reply.

The daemon's pending-offer array is owned by its IPC poll thread
(`src/platform/ipc_socket.c:173-182`), holds at most 64 offers, and expires
ordinary offers after 600 seconds (`src/platform/ipc_socket.c:35-36,185-201`).
Store context in a separate bounded daemon-owned buffer keyed by offer ID;
clear it on dismiss, expiry, confirmation transfer, and shutdown. The worker
must receive a separate ephemeral context keyed by download ID with a mutex or
documented ownership transfer. Copy it only while starting curl and clear after
the final request. A paused job may keep it in memory until resumed, but never
rehydrate it from SQLite. Explicitly zero sensitive buffers before freeing where
the platform allows.

Existing `RequestOptions` and database insert paths persist `cookie` and
`referrer` in plaintext (`src/core/queue_manager.h:22-24`,
`src/persistence/db.c` download insert code). Browser-captured context must
**bypass those persistence paths**, including resume serialization, details
responses, and list events. Existing non-browser user-supplied options remain
a separate feature. The popup receives only boolean presence indicators. Do not
log context, native JSON payloads, auth-bearing full URLs, or curl verbose
headers. Existing offer registration logs the URL
(`src/platform/ipc_socket.c:680`); redact its query/credentials before enabling
context forwarding.

## Threats and verification

A local process running as the user can attempt to speak to the native host or
daemon socket. Treat every context field and popup confirmation as untrusted;
validate lengths and URL schemes at the native host and daemon, and keep the
existing safe-destination check. Other local users must not read the socket or
database; verify permissions during implementation. A compromised extension or
local same-user process may still exfiltrate granted cookies, so request only
per-site host access and make the toggle easy to revoke. Diagnostic logs and
core dumps are additional exposure paths; disable payload logging and verify
tests do not print real values.

Required tests for tasks 3.1.2–3.1.4: permissions refused/off produce no
context; matched request fields are forwarded; unmatched/oversize/CRLF values
are dropped or rejected; native 1 MiB and daemon 16 KiB limits are enforced;
popup shows presence only; confirm applies context to curl; dismiss, expiry,
completion, cancellation, and restart clear it; SQLite and logs contain no
browser-captured sentinel. Use only `127.0.0.1` test servers and fake tokens.

API references: [Chrome optional permissions](https://developer.chrome.com/docs/extensions/reference/api/permissions),
[Chrome cookies](https://developer.chrome.com/docs/extensions/reference/api/cookies),
[Chrome webRequest](https://developer.chrome.com/docs/extensions/reference/api/webRequest).
