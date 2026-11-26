// SPDX-License-Identifier: MIT
#ifndef ENGINE_HLS_H
#define ENGINE_HLS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HLS_URL_MAX 2048
#define HLS_MAX_SEGMENTS 4096
#define HLS_MAX_MAPS 256
#define HLS_MAX_PLAYLIST_BYTES (1024 * 1024)
#define HLS_NO_MAP SIZE_MAX

typedef enum {
  HLS_OK = 0,
  HLS_INVALID,
  HLS_UNSUPPORTED,
  HLS_LIMIT,
  HLS_NO_MEMORY
} HlsResult;

typedef struct {
  bool encrypted; // AES-128 CBC, identity key format only
  char url[HLS_URL_MAX];
  unsigned char iv[16]; // explicit IV or sequence number, big endian
} HlsKey;

typedef struct {
  char url[HLS_URL_MAX];
  HlsKey key; // snapshot at EXT-X-MAP; encrypted maps require explicit IV
} HlsMap;

typedef struct {
  char url[HLS_URL_MAX];
  double duration; // seconds
  uint64_t sequence;
  HlsKey key; // snapshot at segment, independent of subsequent key rotations
  size_t map_index; // HLS_NO_MAP, or index into playlist maps
} HlsSegment;

typedef struct {
  bool is_master, end_list;
  uint64_t version, target_duration,
      media_sequence; // target duration in seconds
  uint64_t
      selected_bandwidth; // bits/second; first variant wins equal bandwidth
  char selected_url[HLS_URL_MAX]; // master: caller fetches and parses this next
  HlsSegment *segments;
  size_t segment_count;
  HlsMap *maps;
  size_t map_count;
} HlsPlaylist;

/* Pure, bounded parser: no I/O, no global mutable state. HTTP(S) URIs only.
 * Initialize out to zero (or free its prior value) before use. On failure out
 * stays zero; optional error contains a bounded description without URLs.
 * Media playlists need TARGETDURATION and at least one segment; ENDLIST is
 * reported, not required (live capture/reload is a downloader policy).
 * Byte ranges, discontinuities, I-frame-only and DRM are explicitly
 * unsupported. Unknown advisory tags are ignored per RFC 8216; no live/LL-HLS
 * reconstruction.
 */
HlsResult hls_parse(const char *text, size_t length, const char *base_url,
                    HlsPlaylist *out, char *error, size_t error_size);
void hls_playlist_free(HlsPlaylist *playlist);
struct Download;
/* Linux VOD engine; same return convention as engine_run_download. */
int hls_run_download(struct Download *download);
/* Shared manifest/asset transfer for DASH: no queue/DB completion or remux.
 * Uses parent's cancellation/context/progress and retains track resume state
 * until the caller durably publishes all outputs. Caller owns playlist. */
int hls_run_asset_playlist(struct Download *download, const char *destination,
                           const HlsPlaylist *playlist, const char *fingerprint,
                           uint64_t *bytes);
int hls_fetch_manifest(struct Download *download, unsigned char *buffer,
                       size_t capacity, size_t *length, char *base,
                       char *fingerprint);
/* Non-active download only; refuses a locked or unowned staging directory. */
void hls_discard_state(const char *destination);
#endif
