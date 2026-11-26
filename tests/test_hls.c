// SPDX-License-Identifier: MIT
#include "../src/engine/hls.h"
#include <criterion/criterion.h>
#include <stdlib.h>
#include <string.h>

static HlsPlaylist parse(const char *text) {
  HlsPlaylist result = {0};
  char error[128];
  cr_assert_eq(hls_parse(text, strlen(text),
                         "https://example.invalid/path/list.m3u8?q=1", &result,
                         error, sizeof(error)),
               HLS_OK, "%s", error);
  return result;
}

Test(hls, simple_relative_and_absolute_uris) {
  HlsPlaylist p =
      parse("#EXTM3U\r\n#EXT-X-VERSION:3\r\n#EXT-X-TARGETDURATION:8\r\n"
            "#EXT-X-MEDIA-SEQUENCE:42\r\n#EXTINF:7.5,title\r\n../one.ts?x=1\r\n"
            "#EXTINF:8,\r\n//cdn.example.invalid/two.ts\r\n#EXT-X-ENDLIST\r\n");
  cr_assert(!p.is_master);
  cr_assert(p.end_list);
  cr_assert_eq(p.version, 3);
  cr_assert_eq(p.target_duration, 8);
  cr_assert_eq(p.segment_count, 2);
  cr_assert_str_eq(p.segments[0].url, "https://example.invalid/one.ts?x=1");
  cr_assert_str_eq(p.segments[1].url, "https://cdn.example.invalid/two.ts");
  cr_assert_eq(p.segments[0].sequence, 42);
  cr_assert_eq(p.segments[1].sequence, 43);
  cr_assert_float_eq(p.segments[0].duration, 7.5, 0.0001);
  cr_assert_eq(p.segments[0].map_index, HLS_NO_MAP);
  hls_playlist_free(&p);
  cr_assert_eq(p.segment_count, 0);
  hls_playlist_free(&p);
}

Test(hls, master_selects_highest_and_handles_quoted_commas) {
  HlsPlaylist p = parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=100,CODECS="
                        "\"avc1,mp4a\"\nlow.m3u8\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=900\n/high.m3u8\n"
                        "#EXT-X-STREAM-INF:BANDWIDTH=300\nmid.m3u8\n");
  cr_assert(p.is_master);
  cr_assert_eq(p.selected_bandwidth, 900);
  cr_assert_str_eq(p.selected_url, "https://example.invalid/high.m3u8");
  cr_assert_eq(p.segment_count, 0);
  hls_playlist_free(&p);
}

Test(hls, aes_keys_rotate_and_implicit_iv_uses_sequence) {
  HlsPlaylist p = parse(
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:258\n"
      "#EXT-X-KEY:METHOD=AES-128,URI=\"keys/a,key\"\n#EXTINF:1,\na.ts\n"
      "#EXT-X-KEY:METHOD=AES-128,URI=\"/b.key\",IV=0x123\n#EXTINF:1,\nb.ts\n"
      "#EXT-X-KEY:METHOD=NONE\n#EXTINF:1,\nc.ts\n");
  cr_assert(p.segments[0].key.encrypted);
  cr_assert_str_eq(p.segments[0].key.url,
                   "https://example.invalid/path/keys/a,key");
  cr_assert_eq(p.segments[0].key.iv[14], 1);
  cr_assert_eq(p.segments[0].key.iv[15], 2);
  cr_assert_eq(p.segments[1].key.iv[14], 1);
  cr_assert_eq(p.segments[1].key.iv[15], 0x23);
  cr_assert(!p.segments[2].key.encrypted);
  hls_playlist_free(&p);
}

Test(hls, map_applies_until_changed_and_retains_its_key) {
  HlsPlaylist p = parse(
      "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:1\n"
      "#EXT-X-KEY:METHOD=AES-128,URI=\"key\",IV=0x1\n"
      "#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:1,\na.m4s\n"
      "#EXT-X-KEY:METHOD=NONE\n#EXTINF:1,\nb.m4s\n"
      "#EXT-X-MAP:URI=\"other.mp4\"\n#EXTINF:1,\nc.m4s\n#EXT-X-ENDLIST\n");
  cr_assert_eq(p.map_count, 2);
  cr_assert_eq(p.segments[0].map_index, 0);
  cr_assert_eq(p.segments[1].map_index, 0);
  cr_assert_eq(p.segments[2].map_index, 1);
  cr_assert(p.maps[0].key.encrypted);
  cr_assert(!p.maps[1].key.encrypted);
  cr_assert_str_eq(p.maps[0].url, "https://example.invalid/path/init.mp4");
  hls_playlist_free(&p);
}

Test(hls, malformed_and_unsupported_inputs_fail_without_partial_output) {
  const char *bad[] = {
      "not a playlist",
      "#EXTM3U\n",
      "#EXTM3U\n#EXTINF:1,\na.ts\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\na.ts\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:NaN,\na.ts\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:2,\na.ts\n",
      "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-VERSION:3\n",
      "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1,BANDWIDTH=2\na.m3u8\n",
      "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\n",
      "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=18446744073709551616\na.m3u8\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-KEY:METHOD=AES-128\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-KEY:METHOD=AES-128,URI=\"x\","
      "IV=0xz\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-KEY:METHOD=AES-128,URI=\"x\"\n#"
      "EXT-X-MAP:URI=\"i\"\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nfile:///tmp/a\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\na.ts\n#EXT-X-MEDIA-"
      "SEQUENCE:5\n",
      "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXT-X-MEDIA-SEQUENCE:"
      "18446744073709551615\n"
      "#EXTINF:1,\na.ts\n#EXTINF:1,\nb.ts\n",
      "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\na.m3u8\n#EXT-X-TARGETDURATION:"
      "1\n"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    HlsPlaylist p = {0};
    char error[128];
    cr_assert_neq(hls_parse(bad[i], strlen(bad[i]), "https://example.invalid/p",
                            &p, error, sizeof(error)),
                  HLS_OK, "case %zu", i);
    cr_assert_null(p.segments);
    cr_assert_null(p.maps);
    cr_assert_neq(error[0], '\0');
  }
  const char *unsupported[] = {
      "#EXTM3U\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n",
      "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\",KEYFORMAT=\"drm\"\n",
      "#EXTM3U\n#EXT-X-BYTERANGE:4@0\n",
      "#EXTM3U\n#EXT-X-MAP:URI=\"i\",BYTERANGE=\"4@0\"\n"};
  for (size_t i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
    HlsPlaylist p = {0};
    char error[128];
    cr_assert_eq(hls_parse(unsupported[i], strlen(unsupported[i]),
                           "https://example.invalid/p", &p, error,
                           sizeof(error)),
                 HLS_UNSUPPORTED);
    cr_assert_null(p.segments);
  }
}

Test(hls, bounded_input_rejects_nul_invalid_utf8_and_segment_limit) {
  HlsPlaylist p = {0};
  char error[128];
  const char nul[] = "#EXTM3U\n\0#EXTINF:1,\na.ts\n";
  cr_assert_eq(hls_parse(nul, sizeof(nul) - 1, "https://example.invalid/p", &p,
                         error, sizeof(error)),
               HLS_INVALID);
  const char invalid[] = "#EXTM3U\n# bad \xc0\x80\n";
  cr_assert_eq(hls_parse(invalid, sizeof(invalid) - 1,
                         "https://example.invalid/p", &p, error, sizeof(error)),
               HLS_INVALID);
  size_t capacity = 64 + (HLS_MAX_SEGMENTS + 1) * 20;
  char *text = malloc(capacity);
  cr_assert_not_null(text);
  strcpy(text, "#EXTM3U\n#EXT-X-TARGETDURATION:1\n");
  for (size_t i = 0; i <= HLS_MAX_SEGMENTS; i++)
    strcat(text, "#EXTINF:1,\na.ts\n");
  cr_assert_eq(hls_parse(text, strlen(text), "https://example.invalid/p", &p,
                         error, sizeof(error)),
               HLS_LIMIT);
  cr_assert_null(p.segments);
  free(text);
}

Test(hls, deterministic_variant_tie_query_resolution_and_live_marker) {
  HlsPlaylist master =
      parse("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=50\nfirst.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=50\nsecond.m3u8\n");
  cr_assert_str_eq(master.selected_url,
                   "https://example.invalid/path/first.m3u8");
  hls_playlist_free(&master);
  HlsPlaylist live =
      parse("#EXTM3U\n#EXT-X-TARGETDURATION:1\n# comment\n"
            "#EXT-X-INDEPENDENT-SEGMENTS\n#EXTINF:0.5,\n?part=1#fragment\n");
  cr_assert(!live.end_list);
  cr_assert_str_eq(live.segments[0].url,
                   "https://example.invalid/path/list.m3u8?part=1");
  hls_playlist_free(&live);
}

Test(hls, argument_error_buffers_and_resource_limits) {
  HlsPlaylist p = {0};
  char tiny[1] = {'x'};
  cr_assert_eq(hls_parse(NULL, 0, NULL, &p, tiny, sizeof(tiny)), HLS_INVALID);
  cr_assert_eq(tiny[0], '\0');
  cr_assert_eq(hls_parse("x", HLS_MAX_PLAYLIST_BYTES + 1,
                         "https://example.invalid/p", &p, NULL, 0),
               HLS_LIMIT);
  size_t capacity = 64 + (HLS_MAX_MAPS + 1) * 32;
  char *text = malloc(capacity);
  cr_assert_not_null(text);
  strcpy(text, "#EXTM3U\n#EXT-X-TARGETDURATION:1\n");
  for (size_t i = 0; i <= HLS_MAX_MAPS; i++)
    strcat(text, "#EXT-X-MAP:URI=\"init.mp4\"\n");
  cr_assert_eq(
      hls_parse(text, strlen(text), "https://example.invalid/p", &p, NULL, 0),
      HLS_LIMIT);
  cr_assert_null(p.maps);
  free(text);
}
