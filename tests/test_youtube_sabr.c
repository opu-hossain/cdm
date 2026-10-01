// SPDX-License-Identifier: MIT
#include "../src/engine/youtube_sabr.h"
#include "../src/engine/youtube_download.h"
#include "../src/engine/youtube_transfer.h"
#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool contains_bytes(const unsigned char *haystack, size_t length,
                           const unsigned char *needle, size_t needle_length) {
  for (size_t i = 0; i + needle_length <= length; i++)
    if (memcmp(haystack + i, needle, needle_length) == 0) return true;
  return false;
}

Test(youtube_sabr, parses_ump_framing_and_rejects_partial_parts) {
  const unsigned char frames[] = {51, 0xac, 0xbb, 0x0a, 0x08};
  size_t offset = 0;
  SabrPart part = {0};
  cr_assert_eq(sabr_read_part(frames, sizeof(frames), &offset, &part), 0);
  cr_assert_eq(offset, 0, "partial UMP part must remain buffered");
  const unsigned char complete[] = {20, 3, 0x08, 0x07, 0x18, 22, 1, 7};
  cr_assert_eq(sabr_read_part(complete, sizeof(complete), &offset, &part), 1);
  cr_assert_eq(part.type, 20);
  cr_assert_eq(part.length, 3);
  cr_assert_eq(offset, 5);
  cr_assert_eq(sabr_read_part(complete, sizeof(complete), &offset, &part), 1);
  cr_assert_eq(part.type, 22);
  cr_assert_eq(offset, sizeof(complete));
}

Test(youtube_sabr, reports_stream_protection_before_transferring_media) {
  const unsigned char accepted[] = {58, 2, 8, 1};
  const unsigned char pending[] = {58, 2, 8, 2};
  const unsigned char rejected[] = {58, 2, 8, 3};
  const unsigned char malformed[] = {58, 2, 8};
  cr_assert_eq(sabr_protection_status(accepted, sizeof(accepted)), 1);
  cr_assert_eq(sabr_protection_status(pending, sizeof(pending)), 2);
  cr_assert_eq(sabr_protection_status(rejected, sizeof(rejected)), 3);
  cr_assert_eq(sabr_protection_status(malformed, sizeof(malformed)), -1);
}

Test(youtube_sabr, decodes_bounded_media_header) {
  const unsigned char data[] = {0x08, 0x05, 0x12, 0x0b,
      'f','i','x','t','u','r','e','1','2','3','4',
      0x18, 0xa0, 0x01, 0x40, 0x01, 0x48, 0x02,
      0x58, 0xe8, 0x07, 0x60, 0xd0, 0x0f, 0x70, 0x64};
  SabrMediaHeader header = {0};
  cr_assert(sabr_decode_media_header(data, sizeof(data), &header));
  cr_assert_eq(header.header_id, 5);
  cr_assert_eq(header.itag, 160);
  cr_assert(header.is_initialization);
  cr_assert_eq(header.segment_number, 2);
  cr_assert_eq(header.start_ms, 1000);
  cr_assert_eq(header.duration_ms, 2000);
  cr_assert_eq(header.end_ms, 3000, "millisecond timing is a valid alternative to time_range");
  cr_assert_eq(header.segment_length, 100);
  cr_assert_str_eq(header.video_id, "fixture1234");
  cr_assert(!sabr_decode_media_header(data, 14, &header));
  const unsigned char nested[] = {0x08, 0x05, 0x12, 0x0b,
      'f','i','x','t','u','r','e','1','2','3','4',
      0x6a, 3, 8, 0xa0, 1, 0x60, 0xe8, 7};
  cr_assert(sabr_decode_media_header(nested, sizeof(nested), &header));
  cr_assert_eq(header.itag, 160);
  cr_assert_eq(header.end_ms, 1000);
  cr_assert(sabr_decode_media_header(nested + 2, sizeof(nested) - 2, &header));
  cr_assert_eq(header.header_id, 0, "omitted optional header_id defaults to zero");
}

Test(youtube_sabr, stores_large_response_without_a_heap_sized_download_buffer) {
  YoutubeResponseFile response = {0};
  char directory[] = "/tmp/cdm-youtube-response-XXXXXX";
  cr_assert_not_null(mkdtemp(directory));
  char destination[128];
  snprintf(destination, sizeof(destination), "%s/video.mp4", directory);
  cr_assert(youtube_response_open(&response, destination));
  cr_assert_eq(rmdir(directory), 0, "spool must already be unlinked");
  unsigned char chunk[65536];
  memset(chunk, 0x5a, sizeof(chunk));
  for (int i = 0; i < 300; i++)
    cr_assert(youtube_response_append(&response, chunk, sizeof(chunk)));
  cr_assert_eq(response.length, 300 * sizeof(chunk));
  cr_assert(youtube_response_map(&response));
  cr_assert_eq(response.data[0], 0x5a);
  cr_assert_eq(response.data[response.length - 1], 0x5a);
  youtube_response_close(&response);
  cr_assert_null(response.file);
  cr_assert_null(response.data);
}

Test(youtube_sabr, builds_selected_video_audio_request_without_overflow) {
  const unsigned char config[] = {0x01, 0x02, 0x03};
  unsigned char output[256];
  size_t length = 0;
  SabrFormatId video = {.itag = 401, .last_modified = 1700000000000000ULL,
                        .xtags = "vtag"};
  SabrFormatId audio = {.itag = 140, .last_modified = 1700000000000001ULL,
                        .xtags = "atag"};
  cr_assert(sabr_encode_request(&video, &audio, 2160, 0, config,
                                sizeof(config), output, sizeof(output), &length));
  cr_assert_gt(length, 20);
  cr_assert_lt(length, sizeof(output));
  cr_assert_eq(output[0], 0x0a, "field 1 is ClientAbrState");
  cr_assert(contains_bytes(output, length, config, sizeof(config)));
  cr_assert(contains_bytes(output, length, (const unsigned char *)"vtag", 4));
  cr_assert(contains_bytes(output, length, (const unsigned char *)"atag", 4));
  cr_assert(!sabr_encode_request(&video, &audio, 2160, 0, config,
                                 sizeof(config), output, 8, &length));
}

Test(youtube_sabr, sends_completed_ranges_for_both_tracks) {
  const SabrFormatId video = {.itag = 160, .last_modified = 123};
  const SabrFormatId audio = {.itag = 140, .last_modified = 456};
  const SabrBufferedRange ranges[] = {
      {.format = video, .end_ms = 11261, .end_segment = 2},
      {.format = audio, .end_ms = 9985, .end_segment = 1}};
  unsigned char output[512];
  size_t length = 0;
  cr_assert(sabr_encode_request_with_ranges(&video, &audio, 144, 9985,
      (const unsigned char *)"cfg", 3, ranges, 2, output,
      sizeof(output), &length));
  cr_assert_gt(length, 50);
  cr_assert(!sabr_encode_request_with_ranges(&video, &audio, 144, 9985,
      (const unsigned char *)"cfg", 3, ranges, 3, output,
      sizeof(output), &length));
}

Test(youtube_sabr, captures_bounded_browser_request_and_reuses_its_context) {
  unsigned char body[128] = {0x2a, 0x03, 1, 2, 3,
      0x8a, 0x01, 0x03, 0x08, 0x91, 0x03,
      0x82, 0x01, 0x03, 0x08, 0x8c, 0x01,
      0x9a, 0x01, 34, 0x12, 32};
  memset(body + 22, 0x5a, 32);
  SabrCapturedRequest captured = {0};
  cr_assert(sabr_parse_captured_request(body, 54, 401, &captured));
  cr_assert_eq(captured.config_length, 3);
  cr_assert_eq(captured.context_length, 34);
  cr_assert_eq(captured.video_format_length, 3);
  cr_assert_eq(captured.audio_format_length, 3);
  cr_assert_eq(captured.audio_itag, 140);
  SabrFormatId video = {.itag = 401, .raw = captured.video_format,
                        .raw_length = captured.video_format_length};
  SabrFormatId audio = {.itag = 140, .raw = captured.audio_format,
                        .raw_length = captured.audio_format_length};
  unsigned char request[256]; size_t length = 0;
  cr_assert(sabr_encode_request_with_context(&video, &audio, 2160, 0,
      captured.config, captured.config_length, NULL, 0,
      captured.context, captured.context_length,
      request, sizeof(request), &length));
  cr_assert(contains_bytes(request, length,
      captured.context, captured.context_length));
  const unsigned char policy[] = {35, 5, 0x3a, 3, 'a', 'b', 'c'};
  cr_assert(sabr_update_playback_context(&captured, policy, sizeof(policy)));
  const unsigned char cookie[] = {0x1a, 3, 'a', 'b', 'c'};
  cr_assert(contains_bytes(captured.context, captured.context_length,
                           cookie, sizeof(cookie)));
  cr_assert(sabr_parse_captured_request(body, 54, 400, &captured));
  cr_assert_eq(captured.video_format_length, 0);
  cr_assert(!sabr_parse_captured_request(body, 53, 401, &captured));
  body[15] = 0xfb; // Web playback may offer Opus 251 instead of AAC 140.
  cr_assert(sabr_parse_captured_request(body, 54, 401, &captured));
  cr_assert_eq(captured.audio_itag, 251);
  // Other offered formats can contain fields we do not need to decode.
  const unsigned char unknown_format[] = {0x8a, 0x01, 0x05,
      0x0d, 1, 2, 3, 4};
  memcpy(body + 54, unknown_format, sizeof(unknown_format));
  cr_assert(sabr_parse_captured_request(body, 54 + sizeof(unknown_format),
                                        401, &captured));
  unsigned char audio_only[54];
  memcpy(audio_only, body, 54);
  memmove(audio_only + 5, audio_only + 11, 43);
  cr_assert(sabr_parse_captured_request(audio_only, 48, 401, &captured));
  cr_assert_eq(captured.video_format_length, 0);
  cr_assert_eq(captured.audio_itag, 251);
  body[21] = 10;
  cr_assert(!sabr_parse_captured_request(body, 54, 401, &captured));
}

Test(youtube_sabr, reads_track_end_and_segment_time_range) {
  const unsigned char metadata[] = {
      0x0a, 0x0b, 'f','i','x','t','u','r','e','1','2','3','4',
      0x12, 0x03, 0x08, 0xa0, 0x01, 0x20, 0x58};
  SabrFormatMetadata parsed = {0};
  cr_assert(sabr_decode_format_metadata(metadata, sizeof(metadata), &parsed));
  cr_assert_eq(parsed.itag, 160);
  cr_assert_eq(parsed.end_segment, 88);
  cr_assert_str_eq(parsed.video_id, "fixture1234");
  const unsigned char header[] = {
      0x08, 0x03, 0x12, 0x0b, 'f','i','x','t','u','r','e','1','2','3','4',
      0x18, 0xa0, 0x01, 0x48, 0x02,
      0x7a, 0x0c, 0x08, 0x8f, 0xa0, 0x08, 0x10, 0xdf, 0x9f, 0x08,
      0x18, 0xc0, 0xbb, 0x01};
  SabrMediaHeader segment = {0};
  cr_assert(sabr_decode_media_header(header, sizeof(header), &segment));
  cr_assert_gt(segment.end_ms, 0);
}

Test(youtube_sabr, selects_advertised_video_and_audio_from_player) {
  const char *json = "{\"playabilityStatus\":{\"status\":\"OK\"},"
      "\"videoDetails\":{\"videoId\":\"fixture1234\",\"isLive\":false},"
      "\"streamingData\":{\"serverAbrStreamingUrl\":"
      "\"https://r.example.invalid/videoplayback\",\"adaptiveFormats\":["
      "{\"itag\":401,\"mimeType\":\"video/mp4; codecs=av01\","
      "\"height\":2160,\"lastModified\":\"123456\",\"contentLength\":\"1000\"},"
      "{\"itag\":140,\"mimeType\":\"audio/mp4; codecs=mp4a\","
      "\"lastModified\":\"123457\",\"contentLength\":\"100\"}]},"
      "\"playerConfig\":{\"mediaCommonConfig\":{\"mediaUstreamerRequestConfig\":{"
      "\"videoPlaybackUstreamerConfig\":\"AQID-_8\"}}}}";
  YoutubeSelection selected = {0};
  cr_assert(youtube_parse_player(json, "fixture1234", 401, &selected));
  cr_assert_eq(selected.video_itag, 401);
  cr_assert_eq(selected.audio_itag, 140);
  cr_assert_eq(selected.height, 2160);
  cr_assert_eq(selected.expected_size, 1100);
  cr_assert_eq(selected.config_length, 5);
  cr_assert_eq(selected.config[3], 0xfb);
  cr_assert(!youtube_parse_player(json, "wrong1234", 401, &selected));
  cr_assert(!youtube_parse_player(json, "fixture1234", 400, &selected));
}

Test(youtube_sabr, replaces_browser_request_sequence_without_duplicate_parameter) {
  char url[128];
  cr_assert(youtube_sequence_url("https://r.googlevideo.com/videoplayback?rn=7&x=1",
                                 8, url, sizeof(url)));
  cr_assert_str_eq(url, "https://r.googlevideo.com/videoplayback?rn=8&x=1");
  cr_assert(youtube_sequence_url("https://r.googlevideo.com/videoplayback?x=1",
                                 0, url, sizeof(url)));
  cr_assert_str_eq(url, "https://r.googlevideo.com/videoplayback?x=1&rn=0");
  cr_assert(!youtube_sequence_url("https://r.googlevideo.com/videoplayback",
                                  1, url, 8));
}

static void append_part(unsigned char *out, size_t *length, unsigned char kind,
                        const unsigned char *payload, size_t payload_length) {
  cr_assert_lt(payload_length, 128);
  out[(*length)++] = kind;
  out[(*length)++] = (unsigned char)payload_length;
  memcpy(out + *length, payload, payload_length);
  *length += payload_length;
}

Test(youtube_sabr, writes_ordered_tracks_and_rolls_back_bad_response) {
  FILE *video_file = tmpfile(), *audio_file = tmpfile();
  cr_assert_not_null(video_file);
  cr_assert_not_null(audio_file);
  YoutubeTrackState video = {.fd = fileno(video_file), .itag = 160,
                             .end_segment = 1};
  YoutubeTrackState audio = {.fd = fileno(audio_file), .itag = 140,
                             .end_segment = 1};
  const unsigned char vh[] = {0x08,0x03,0x12,0x0b,
      'f','i','x','t','u','r','e','1','2','3','4',
      0x18,0xa0,0x01,0x48,0x01,0x70,0x03,
      0x7a,0x08,0x08,0x00,0x10,0xe8,0x07,0x18,0xe8,0x07};
  const unsigned char ah[] = {0x08,0x04,0x12,0x0b,
      'f','i','x','t','u','r','e','1','2','3','4',
      0x18,0x8c,0x01,0x48,0x01,0x70,0x03,
      0x7a,0x08,0x08,0x00,0x10,0xe8,0x07,0x18,0xe8,0x07};
  unsigned char data[256]; size_t length = 0;
  const unsigned char video_chunk[] = {3,'v','i','d'};
  const unsigned char audio_chunk[] = {4,'a','u','d'};
  const unsigned char bad_chunk[] = {3,'b','a','d'};
  append_part(data, &length, 20, vh, sizeof(vh));
  append_part(data, &length, 21, video_chunk, sizeof(video_chunk));
  append_part(data, &length, 22, (const unsigned char *)"\x03", 1);
  append_part(data, &length, 20, ah, sizeof(ah));
  append_part(data, &length, 21, audio_chunk, sizeof(audio_chunk));
  append_part(data, &length, 22, (const unsigned char *)"\x04", 1);
  cr_assert(youtube_process_ump(data, length, "fixture1234", &video, &audio));
  cr_assert_eq(video.last_segment, 1);
  cr_assert_eq(audio.last_segment, 1);
  cr_assert_eq(video.bytes, 3);
  cr_assert_eq(audio.bytes, 3);
  cr_assert_eq(video.end_ms, 1000);
  // Some streams number the first media segment zero.
  unsigned char zero_header[sizeof(vh)];
  memcpy(zero_header, vh, sizeof(vh));
  zero_header[19] = 0;
  FILE *zero_file = tmpfile();
  cr_assert_not_null(zero_file);
  YoutubeTrackState zero_video = {.fd = fileno(zero_file), .itag = 160};
  unsigned char zero_data[128]; size_t zero_length = 0;
  append_part(zero_data, &zero_length, 20, zero_header, sizeof(zero_header));
  append_part(zero_data, &zero_length, 21, video_chunk, sizeof(video_chunk));
  append_part(zero_data, &zero_length, 22, (const unsigned char *)"\x03", 1);
  cr_assert(youtube_process_ump(zero_data, zero_length, "fixture1234",
                                &zero_video, &audio));
  cr_assert_eq(zero_video.bytes, 3);
  fclose(zero_file);
  // segment_length_bytes is optional; MEDIA_END delimits an unsized segment.
  unsigned char unsized_header[sizeof(vh) - 2];
  memcpy(unsized_header, vh, 20);
  memcpy(unsized_header + 20, vh + 22, sizeof(vh) - 22);
  FILE *unsized_file = tmpfile();
  cr_assert_not_null(unsized_file);
  YoutubeTrackState unsized_video = {.fd = fileno(unsized_file), .itag = 160};
  zero_length = 0;
  append_part(zero_data, &zero_length, 20, unsized_header, sizeof(unsized_header));
  append_part(zero_data, &zero_length, 21, video_chunk, sizeof(video_chunk));
  append_part(zero_data, &zero_length, 22, (const unsigned char *)"\x03", 1);
  cr_assert(youtube_process_ump(zero_data, zero_length, "fixture1234",
                                &unsized_video, &audio));
  cr_assert_eq(unsized_video.bytes, 3);
  fclose(unsized_file);
  append_part(data, &length, 21, bad_chunk, sizeof(bad_chunk));
  cr_assert(!youtube_process_ump(data, length, "fixture1234", &video, &audio));
  cr_assert_eq(video.bytes, 3);
  cr_assert_eq(lseek(video.fd, 0, SEEK_END), 3);
  cr_assert_eq(lseek(audio.fd, 0, SEEK_END), 3);
  fclose(video_file);
  fclose(audio_file);
}

Test(youtube_sabr, consumes_media_frames_larger_than_old_response_limit) {
  YoutubeResponseFile response = {0};
  cr_assert(youtube_response_open(&response, NULL));
  // Twenty MiB in one UMP MEDIA part, no declared segment size.
  const unsigned char header[] = {0x08,3,0x12,11,
      'f','i','x','t','u','r','e','1','2','3','4',
      0x18,0xa0,1,0x48,1,0x60,0xe8,7};
  unsigned char prefix[64]; size_t prefix_length = 0;
  append_part(prefix, &prefix_length, 20, header, sizeof(header));
  cr_assert(youtube_response_append(&response, prefix, prefix_length));
  const unsigned char media[] = {21,0xf0,1,0,0x40,1,3}; // size=20MiB+1
  for (size_t i = 0; i < sizeof(media); i++)
    cr_assert(youtube_response_append(&response, media + i, 1));
  unsigned char data[65536]; memset(data, 'v', sizeof(data));
  for (int i = 0; i < 320; i++)
    cr_assert(youtube_response_append(&response, data, sizeof(data)));
  const unsigned char end[] = {22,1,3};
  cr_assert(youtube_response_append(&response, end, sizeof(end)));
  cr_assert_eq(response.media_bytes, 20u * 1024 * 1024,
    "live progress must exclude UMP headers even across curl chunk boundaries");
  cr_assert(youtube_response_map(&response));
  FILE *vf = tmpfile(), *af = tmpfile();
  cr_assert(vf && af);
  YoutubeTrackState video = {.fd=fileno(vf), .itag=160};
  YoutubeTrackState audio = {.fd=fileno(af), .itag=140};
  cr_assert(youtube_process_ump(response.data, response.length, "fixture1234",
                                &video, &audio));
  cr_assert_eq(video.bytes, 20u * 1024 * 1024);
  cr_assert_eq(lseek(video.fd, 0, SEEK_END), video.bytes);
  fclose(vf); fclose(af);
  youtube_response_close(&response);
}
