# cdm daemon IPC (protocol v2)

Source of truth: `src/platform/ipc_protocol.h`, `src/platform/ipc_socket.h`, and `src/platform/ipc_socket.c`. The transport is a per-user Unix stream socket (`XDG_RUNTIME_DIR/cdm.sock`, otherwise `$HOME/.local/share/cdm/ipc.sock`, otherwise `/tmp/cdm_<uid>.sock`). The socket is created with mode `0600` (`src/platform/ipc_socket.c:81`, `src/platform/ipc_socket.c:903`). The daemon accepts at most 16 clients (`src/platform/ipc_socket.c:32`).

## Framing and negotiation

Every **request** and asynchronous **event** begins with the unchanged v1 `MsgHeader`: `uint32_t length` (payload bytes, excluding the header), then `MsgType type` (C enum; four bytes on the supported ABI). The current header is eight bytes. Integers, `float`, and raw structs use native byte order, size, alignment, and padding; this is a local, same-ABI protocol, not a portable network format. Strings in length-prefixed fields are byte sequences without a wire NUL: `uint32_t length`, then exactly that many bytes. Fixed `char[]` fields in raw structs are NUL-terminated when populated. Request payloads over `IPC_MAX_FRAME_SIZE = 16384` bytes or with an invalid type/length are rejected by closing the connection (`src/platform/ipc_socket.c:397`). **Command replies have no `MsgHeader`**; their layouts are listed below. Event frames do have a header.

`MSG_HELLO` (41) is a v1-framed empty request. Its unframed reply is native `uint16_t IPC_PROTOCOL_VERSION`, currently **2**. `ipc_client_connect_compatible()` uses a bounded HELLO exchange; if an old daemon closes or fails the exchange, the client reconnects and treats it as v1. A version other than the client's `IPC_PROTOCOL_VERSION` also uses v1 messages. Only a confirmed v2 client sends `MSG_SUBSCRIBE_V2`. Existing v1 types and payloads must remain byte-compatible; add a new type/versioned payload for new fields, never append fields to an existing wire struct (`src/platform/ipc_protocol.h:40`, `src/platform/ipc_socket.c:1177`).

## Message registry

Types 1–17 are legacy. Type 7 exists in the enum but has no producer or request handler.

| Type | Name | Request payload → unframed reply, or event payload |
| ---: | --- | --- |
| 1 | `MSG_ADD_DOWNLOAD` | Three length-prefixed strings: URL, destination path, options JSON → `uint32_t download_id` (`0` on failure). Explicit destination basename. |
| 2 | `MSG_PAUSE` | `uint32_t download_id` → `uint8_t IpcResult`. |
| 3 | `MSG_RESUME` | `uint32_t download_id` → `uint8_t IpcResult`. |
| 4 | `MSG_CANCEL` | `uint32_t download_id` → `uint8_t IpcResult`. |
| 5 | `MSG_LIST` | Empty → `uint32_t` count of active plus queued downloads. |
| 6 | `MSG_STATUS_EVENT` | Event: `uint32_t download_id`, `float progress` (fraction 0–1), `uint32_t status_len`, then status text bytes. |
| 7 | `MSG_PROGRESS_EVENT` | Reserved/unimplemented; no event is emitted; rejected as a request. |
| 8 | `MSG_SUBSCRIBE` | Empty → no reply; subscribe to type 6 events. |
| 9 | `MSG_LIST_ALL` | Empty → `uint32_t count`, then `count` rows (layout below). Capped at 200; excess rows are silently omitted. |
| 10 | `MSG_GET_DETAILS` | `uint32_t download_id` → `uint8_t found` (0/1), then details if found (layout below). |
| 11 | `MSG_RELOAD_CONFIG` | Empty → `uint8_t IpcResult`. |
| 12 | `MSG_BROWSER_OFFER` | UTF-8 JSON offer → raw `IpcBrowserOffer` (all zero / `offer_id=0` on failure). |
| 13 | `MSG_BROWSER_GET_OFFER` | `uint32_t offer_id` → raw `IpcBrowserOffer` (zero if absent). |
| 14 | `MSG_BROWSER_CONFIRM` | `uint32_t offer_id`, `uint32_t path_len`, `path_len` destination bytes → `uint32_t download_id` (`0` on failure). |
| 15 | `MSG_BROWSER_DISMISS` | `uint32_t offer_id` → `uint8_t IpcResult`. |
| 16 | `MSG_BROWSER_SUBSCRIBE_PROGRESS` | `uint32_t download_id` → initial raw `IpcBrowserProgress` snapshot; later type 17 events for that ID. Unknown ID yields `status="NOT_FOUND"`. |
| 17 | `MSG_BROWSER_PROGRESS_EVENT` | Event: raw `IpcBrowserProgress`. |
| 32 | `MSG_ADD_DOWNLOAD_AUTO` | Same layout/reply as type 1; daemon may rename an automatically derived destination after probing headers. |
| 33 | `MSG_STATUS_EVENT_V2` | Event: raw `IpcProgressV2`. Sent only to v2 subscribers. |
| 34 | `MSG_SUBSCRIBE_V2` | Empty → no reply; opts this socket into type 33 events. |
| 35 | `MSG_LIST_PAGE` | `uint32_t offset`, `uint32_t limit` → `uint32_t total`, `uint32_t returned`, then `returned` rows in the type 9 row layout. `limit` is capped at 500; `total` is the full database count before paging. |
| 36 | `MSG_LIST_PAGE_WITH_SIZE` | Same request and page header as type 35; each row adds native `uint64_t total_size` (bytes; `0` means unknown) after `float progress`. Used by `cdm cli list`. |
| 41 | `MSG_HELLO` | Empty → raw `uint16_t` daemon protocol version. |

`IpcResult`: `OK=0`, `NOT_FOUND=1`, `REJECTED=2`, `ERROR=3` (`src/platform/ipc_protocol.h`). `MSG_LIST_ALL` and `MSG_LIST_PAGE` rows are **field-by-field**, not raw `DownloadListRecord`: `uint32_t id`, length-prefixed `url`, `dest_path`, `status`, and `float progress` (0–1). Type 36 uses that row layout plus `uint64_t total_size` at the end. `IPC_LIST_ALL_MAX=200` and `IPC_LIST_PAGE_MAX=500` are defined in `src/platform/ipc_socket.h`; listing uses database order, currently newest first (`src/platform/ipc_socket.c`, `src/persistence/db.c`). Old v2 daemons do not know types 35/36 and close the socket when sent one; reconnect and use type 9 when talking to such a daemon. The CLI requires type 36 to report daemon-recorded size and reports an unsupported-daemon error if it is unavailable. `MSG_GET_DETAILS` success data is length-prefixed `cookie`, `referrer`, `extra_headers`, `expected_sha256` strings, followed by `uint64_t speed_limit_bps` (bytes/second). Add options JSON accepts those same keys; `extra_headers` is newline-delimited. Malformed JSON is ignored rather than rejecting the download (`src/platform/ipc_socket.c`). URL capacity is 2048 bytes including NUL, destination 1024 including NUL (`src/platform/ipc_protocol.h`).

## Event structs and subscriptions

`IpcProgressV2` (`src/platform/ipc_protocol.h:51`; raw payload of type 33):

| Field | Type | Meaning |
| --- | --- | --- |
| `download_id` | `uint32_t` | Download identifier. |
| `bytes_received` | `uint64_t` | Completed bytes. |
| `total_bytes` | `uint64_t` | Expected bytes; `0` means unknown. |
| `speed_bps` | `uint64_t` | Daemon-sampled transfer bytes/second; `0` means not yet known. |
| `eta_seconds` | `uint64_t` | Estimated remaining seconds; `UINT64_MAX` means unknown. |
| `progress` | `float` | Fraction 0–1; `-1` means indeterminate. |
| `status` | `char[24]` | NUL-terminated scheduler status text. |
| `error` | `char[256]` | NUL-terminated error text, empty if none. |

`IpcBrowserProgress` (`src/platform/ipc_socket.h:40`; initial reply and raw payload of type 17) contains `uint32_t download_id`, `uint64_t bytes_received` (bytes), `uint64_t total_bytes` (bytes; 0 unknown), `float progress` (fraction 0–1), `char status[24]` (NUL-terminated canonical state such as `ACTIVE`), `char error[256]` (NUL-terminated), and `char dest_path[1024]` (NUL-terminated). This legacy struct has no speed or ETA.

A general type 34 subscriber gets **type 33 then type 6** on each status broadcast; a type 8 subscriber gets only type 6. A browser-specific socket first sends type 16 and consumes its unframed snapshot; it can then send type 34 on the **same socket** to get **type 33 then type 17** for that download. Without type 34 it gets only type 17. Browser-specific v2 subscription does not subscribe to all downloads. Consumers must read or skip both frames in a paired broadcast to keep the stream aligned. Slow or partially written nonblocking event sockets are removed (`src/platform/ipc_socket.c:680`, `:805`, `:1468`).

## Browser offer struct

Type 12 JSON accepts `request_id` (required string, max 127 bytes), `url` (required HTTP(S) string, max 2047), `filename` (optional string, max 511), `mime` (optional string, max 127), `referrer` (optional string, max 2047), and `total_bytes` (optional nonnegative JSON number up to 2^53−1, bytes). The raw `IpcBrowserOffer` reply (`src/platform/ipc_socket.h:28`) has `uint32_t offer_id` (`0` means absent/failure), `uint32_t download_id` (`0` until confirmed), `uint64_t total_bytes` (bytes; `0` unknown), `IpcBrowserOfferState state` (`WAITING=0`, `CONFIRMED=1`, `DISMISSED=2`), and NUL-terminated arrays `request_id[128]`, `url[2048]`, `filename[512]`, `mime[128]`, `referrer[2048]`. Offers are held in 64 in-memory slots for 600 seconds; the same `request_id`/URL retrieves the same offer. Confirming a waiting offer queues once; repeat confirm returns its download ID. Dismiss is idempotent for a dismissed offer (`src/platform/ipc_socket.c:34`, `src/platform/ipc_socket.c:242`, `src/platform/ipc_socket.c:571`).

The socket carries sensitive options and `MSG_GET_DETAILS` can return plaintext cookies and headers. Only connect trusted same-user processes; the protocol has no additional peer authentication or encryption. Raw structs and enum widths must match between client and daemon builds.
