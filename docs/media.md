# Media downloads (phase 4)

cdm treats a browser media offer as an explicit request. The extension collects
bounded media candidates; detection alone does not open a desktop popup. Select
a candidate in the extension, review the destination in the popup, and confirm.
The popup identifies HLS, DASH, or video offers. `src/platform/ipc_socket.c`
records the selected engine in `downloads.media_kind`; `src/engine/engine_runner.c`
dispatches the download. A plain video offer follows the ordinary HTTP engine
unless the user selects the optional site extractor.

| Format | Implemented path | Limits |
|---|---|---|
| HLS | `src/engine/hls.c`, `hls_parse()` and `hls_run_download()` | Finite VOD with `#EXT-X-ENDLIST`; highest-bandwidth master variant; AES-128 identity keys and whole-file initialization maps. No live reload, byte ranges, discontinuities, DRM, or separate audio rendition. |
| DASH | `src/engine/dash.c`, `dash_parse()` and `dash_run_download()` | Static single-period MPD with inherited BaseURL, SegmentTemplate/SegmentList and finite timelines; highest-bandwidth audio/video representation, first on ties. No dynamic/live, DRM, SegmentBase, byte ranges or multiple periods. |
| Site extractor | `src/engine/site_grab.c`, `site_grab_run_download()` | Explicit opt-in, disabled by default; HTTPS YouTube, Vimeo and Dailymotion hostnames only. Requires a local yt-dlp executable. |

## Download and recovery behavior

- HLS and DASH parse a bounded 1 MiB manifest and at most 4096 segments per
  track. The HLS parser also bounds initialization maps to 256. HLS uses
  OpenSSL Crypto for AES-128 CBC. DASH XML parsing uses libxml2 without entity
  or network loading. Unsupported layouts return errors rather than guessing.
- HLS stores private `.hlsparts` assets and `.hlsstate` metadata beside the
  reserved destination. DASH uses `.dashparts` and reuses the HLS asset
  downloader per track. Resume state identifies manifest layout and saved
  validators/hashes; missing or changed validators cause an asset download
  again. Browser context remains memory-only and is lost on daemon restart.
- After a complete HLS transfer, cdm tries a fixed-argument `ffmpeg -c copy`
  remux when ffmpeg was found at startup. Missing or failed ffmpeg retains the
  native output. DASH downloads video and audio separately and tries a
  fixed-argument merge; when ffmpeg is unavailable or fails, it keeps both
  tracks and persists the companion audio path in `downloads.companion_path`.
  Successful publication uses a unique path and a SQLite completion checkpoint.
- The optional site extractor runs a startup-resolved yt-dlp executable via
  `posix_spawn`, without a shell, with `--ignore-config`, `--no-playlist`,
  `--newline` and an explicit progress template. It parses byte progress,
  speed in bytes/second, and ETA in seconds. A selected site job persists
  `downloads.site_grab=1`; the last redacted stderr line and exit code persist
  in `downloads.last_error`. The selected output extension is published to a
  unique destination. Child cancellation and inactivity terminate and reap the
  process. The configured global/per-download byte rate is applied to each
  yt-dlp job; simultaneous jobs are not capped as one aggregate.

## Configuration and security

```toml
[sites]
use_yt_dlp = false
yt_dlp_path = "yt-dlp"
yt_dlp_format = "bestvideo+bestaudio/best"
```

`yt_dlp_path` is resolved once when the daemon starts; restart after changing
it. No executable is bundled. The popup's site checkbox defaults off and is
shown only when the daemon enables the feature, finds the executable, and the
offer URL is allowlisted. Selecting it discards captured browser cookies,
headers, User-Agent and Referer before enqueueing. The site URL remains in the
child's process arguments while it runs. HLS/DASH browser context is origin
bound and cannot be silently forwarded across redirects.

The database is at schema version 13. Protocol HELLO reports version 11;
media kind lookup uses message 58, site capability uses 59, and explicit site
confirmation uses 60. Existing raw-wire layouts were not extended. A current
popup should negotiate HELLO before using these messages.

## Verification

Run `cmake --build build -j` and
`ctest --test-dir build --output-on-failure`. Focused targets include
`test_hls`, `test_hls_download`, `test_dash`, `test_dash_download`,
`test_site_grab`, `test_site_grab_download`, `test_spawn`, `test_db`, and
`test_ipc_socket`. HTTP fixtures bind to `127.0.0.1`; the site test uses a
mock executable and never visits the offered URL. Manual browser and real
stream checks are separate release gates. Windows staging and child spawning
remain `TODO(platform)`; see `docs/open-questions.md`.
