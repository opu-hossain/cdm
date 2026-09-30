# Media downloads

The Chromium and Firefox extensions can detect direct video files and supported
HLS/DASH manifests after **Media detection** is enabled in extension Options and
the requested permissions are granted. While a video plays, **Download with cdm**
appears at the top right of its frame. Choose a detected stream, review the
native confirmation popup, and confirm. Extension Options also lists retained
candidates. Detection alone does not enqueue a download.

YouTube site streams and other streams that require site extraction are not
supported. The former external site downloader has been removed. Existing
site-tool records remain readable in SQLite, but attempting to resume one
fails with **External site downloads are no longer supported**. Historical IPC
message numbers and database columns remain reserved for upgrade compatibility.

| Format | Implemented path | Limits |
|---|---|---|
| Direct video | `src/engine/engine_runner.c` ordinary HTTP engine | Requires a usable media URL; protected, blob, or site-specific streams are unsupported. |
| HLS | `src/engine/hls.c`, `hls_parse()` and `hls_run_download()` | Finite VOD with `#EXT-X-ENDLIST`; highest-bandwidth master variant; AES-128 identity keys and whole-file initialization maps. No live reload, byte ranges, discontinuities, DRM, or separate audio rendition. |
| DASH | `src/engine/dash.c`, `dash_parse()` and `dash_run_download()` | Static single-period MPD with inherited BaseURL, SegmentTemplate/SegmentList and finite timelines; highest-bandwidth audio/video representation, first on ties. No dynamic/live, DRM, SegmentBase, byte ranges or multiple periods. |

HLS and DASH accept bounded 1 MiB manifests and at most 4096 segments per
track. HLS uses OpenSSL Crypto for AES-128 CBC; DASH parses XML with libxml2
without entity or network loading. Unsupported layouts return errors.

HLS keeps private `.hlsparts` assets and `.hlsstate` metadata beside the
reserved destination. DASH uses `.dashparts` and reuses the HLS asset
downloader per track. Resume state identifies manifest layout and saved
validators/hashes; missing or changed validators trigger another asset download.
Browser context is memory-only and is lost on daemon restart.

After a complete HLS transfer, cdm tries a fixed-argument `ffmpeg -c copy`
remux when ffmpeg was found at startup. Missing or failed ffmpeg retains the
native output. DASH downloads video and audio separately and tries a
fixed-argument merge; when ffmpeg is unavailable or fails, it keeps both
tracks and persists the companion audio path in `downloads.companion_path`.
Successful publication uses a unique path and a SQLite completion checkpoint.

Run `cmake --build build -j` and
`ctest --test-dir build --output-on-failure`. Focused tests include
`test_hls`, `test_hls_download`, `test_dash`, `test_dash_download`,
`test_spawn`, `test_db`, `test_ipc_socket`, and `test_browser_extensions`.
HTTP fixtures bind to `127.0.0.1`. Installed-browser and real-stream behavior
remain manual release checks; see `docs/RELEASE_CHECKLIST.md`.
