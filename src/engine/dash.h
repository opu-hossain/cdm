// SPDX-License-Identifier: MIT
#ifndef ENGINE_DASH_H
#define ENGINE_DASH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define DASH_URL_MAX 2048
#define DASH_MAX_SEGMENTS 4096
#define DASH_MAX_MANIFEST_BYTES (1024 * 1024)
typedef enum {
  DASH_OK,
  DASH_INVALID,
  DASH_UNSUPPORTED,
  DASH_LIMIT,
  DASH_NO_MEMORY
} DashResult;
typedef struct {
  char url[DASH_URL_MAX];
  uint64_t number, time, duration; /* duration/time in track timescale ticks */
} DashSegment;
typedef struct {
  bool present;
  char representation_id[256];
  uint64_t bandwidth; /* bits/second */
  uint64_t timescale, presentation_time_offset;
  char initialization_url[DASH_URL_MAX]; /* empty when no initialization */
  DashSegment *segments;
  size_t segment_count;
} DashTrack;
typedef struct {
  double
      duration; /* finite period duration in seconds, or zero if unspecified */
  DashTrack video, audio; /* highest bandwidth of each, first wins ties */
} DashManifest;
/* Pure bounded UTF-8 XML parser, HTTP(S) URLs only, no entity/network loading.
 * Static single-period MPDs; inherited BaseURL/SegmentTemplate/SegmentList.
 * Supports Number/Time/RepresentationID/Bandwidth substitutions and timelines.
 * Dynamic, DRM, SegmentBase, byte ranges and multi-period layouts return a
 * clear unsupported error. Initialize out to zero/free before reuse; failure
 * zeros it.
 */
DashResult dash_parse(const char *text, size_t length, const char *base_url,
                      DashManifest *out, char *error, size_t error_size);
void dash_manifest_free(DashManifest *manifest);
struct Download;
int dash_run_download(struct Download *download);
void dash_discard_state(const char *destination);
#endif
