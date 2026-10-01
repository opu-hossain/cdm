# Media downloads

The Chromium and Firefox extensions can detect direct video files and supported
HLS/DASH manifests after **Media detection** is enabled in extension Options and
the requested permissions are granted. While a video plays, **Download with cdm**
appears at the top right of its frame. Choose a detected stream, review the
native confirmation popup, and confirm. Extension Options also lists retained
candidates. Detection alone does not enqueue a download.

On public YouTube watch pages, cdm discovers available MP4 qualities during
playback. It uses player metadata immediately when available; the fallback
player request is only needed when adaptive metadata is absent. An open picker
refreshes as browser playback supplies authorization, without a quality switch
or page reload. Quality choices keep stable IDs during refresh.

Combined MP4 files use the ordinary HTTP engine. Adaptive video and audio use
cdm's built-in SABR downloader and the browser's bounded, memory-only playback
context; no browser cookies are forwarded. Advertised format IDs can be selected
without first playing that quality. Optional ffmpeg combines the tracks; if it
is missing or fails, cdm retains video and companion audio separately. Available
resolutions and sizes depend on the video. Restarting the daemon or browser can
require a fresh offer. Live/protected video and expired authorization are not
supported by this path.

Other supported sites use the same in-player control. Direct files show
**Original quality** (including ordinary MP4/WebM byte-range playback), while
HLS/DASH show **Automatic quality**: the existing engine chooses its supported
highest-bandwidth variant/representations. Separate resolution choices for
arbitrary HLS/DASH manifests are not implemented. Unknown size is explicit;
the size of a manifest is never presented as the size of the video. Detection
is bounded and heuristic, and does not guarantee support for every site or
browser-playable stream.

The popup shows **Transfer finished / Finishing file…** during local merging,
verification or scanning, instead of presenting that interval as a stalled
network download. Completion is still reported only after that work succeeds.

The former external
site downloader has been removed. Existing
site-tool records remain readable in SQLite, but attempting to resume one
fails with **External site downloads are no longer supported**. Historical IPC
message numbers and database columns remain reserved for upgrade compatibility.

| Format | Implemented path | Limits |
|---|---|---|
| Direct video and combined YouTube MP4 | `src/engine/engine_runner.c` ordinary HTTP engine | Requires a usable direct media URL; protected and blob URLs are unsupported by the ordinary HTTP engine. |
| YouTube adaptive MP4 | `src/engine/youtube_download.c`, SABR parser and transfer helpers | Requires valid playing-browser context; optional ffmpeg for combined output; no live/protected streams. |
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
