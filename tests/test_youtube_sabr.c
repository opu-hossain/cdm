// SPDX-License-Identifier: MIT
#include "../src/engine/youtube_sabr.h"
#include <criterion/criterion.h>
#include <string.h>

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
  cr_assert_eq(header.segment_length, 100);
  cr_assert_str_eq(header.video_id, "fixture1234");
  cr_assert(!sabr_decode_media_header(data, 14, &header));
}

Test(youtube_sabr, builds_selected_video_audio_request_without_overflow) {
  const unsigned char config[] = {0x01, 0x02, 0x03};
  unsigned char output[256];
  size_t length = 0;
  cr_assert(sabr_encode_request(401, 140, 2160, 0, config,
                                sizeof(config), output, sizeof(output), &length));
  cr_assert_gt(length, 20);
  cr_assert_lt(length, sizeof(output));
  cr_assert_eq(output[0], 0x0a, "field 1 is ClientAbrState");
  cr_assert(contains_bytes(output, length, config, sizeof(config)));
  cr_assert(!sabr_encode_request(401, 140, 2160, 0, config,
                                 sizeof(config), output, 8, &length));
}
