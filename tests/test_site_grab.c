// SPDX-License-Identifier: MIT
#include "../src/engine/site_grab.h"
#include <criterion/criterion.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
Test(site_grab, host_allowlist) {
  cr_assert(site_grab_host_allowed("www.youtube.com"));
  cr_assert(site_grab_host_allowed("youtu.be"));
  cr_assert(site_grab_host_allowed("player.vimeo.com"));
  cr_assert(site_grab_host_allowed("www.dailymotion.com"));
  cr_assert_not(site_grab_url_allowed("http://127.0.0.1/media"));
  cr_assert_not(
      site_grab_host_allowed("youtube.com.evil.invalid"));
  cr_assert_not(
      site_grab_url_allowed("https://example.invalid/watch"));
  cr_assert_not(site_grab_url_allowed("https://user@example.invalid/watch"));
  cr_assert_not(site_grab_url_allowed("file:///tmp/a"));
  SiteGrabProbe probe;
  memset(&probe, 0xff, sizeof(probe));
  cr_assert_eq(site_grab_probe("https://example.invalid/watch", NULL, &probe), -1);
  cr_assert_eq(probe.format_count, 0);
}
Test(site_grab, progress_lines) {
  SiteGrabProgress p = {0};
  cr_assert(site_grab_parse_progress("CDM|1048576|2097152|524288|2|50.0%", &p));
  cr_assert_eq(p.downloaded_bytes, 1048576);
  cr_assert_eq(p.total_bytes, 2097152);
  cr_assert_eq(p.speed_bps, 524288);
  cr_assert_eq(p.eta_seconds, 2);
  cr_assert_float_eq(p.percent, 50, 0.01);
  cr_assert(site_grab_parse_progress("CDM|100|200|1234.75|3| 50.0% ", &p));
  cr_assert_eq(p.speed_bps, 1234);
  cr_assert_float_eq(p.percent, 50, 0.01);
  cr_assert(site_grab_parse_progress("CDM|100|NA|NA|NA|NA", &p));
  cr_assert_eq(p.downloaded_bytes, 100);
  cr_assert_eq(p.total_bytes, 0);
  cr_assert_eq(p.eta_seconds, UINT64_MAX);
  cr_assert_float_eq(p.percent, -1, 0.01);
  cr_assert_not(site_grab_parse_progress("CDM|1|2|3|4|NaN", &p));
  cr_assert_not(site_grab_parse_progress("CDM|-1|2|3|4|50%", &p));
  cr_assert_not(
      site_grab_parse_progress("CDM|18446744073709551616|2|3|4|50%", &p));
  cr_assert_not(site_grab_parse_progress("other text", &p));
}

Test(site_grab, parse_bounded_format_probe) {
  const char json[] = "{\"title\":\"Fixture video\",\"formats\":["
    "{\"format_id\":\"v720\",\"ext\":\"mp4\",\"width\":1280,\"height\":720,"
    "\"filesize\":1048576,\"vcodec\":\"avc1\",\"acodec\":\"none\"},"
    "{\"format_id\":\"audio\",\"ext\":\"m4a\",\"filesize_approx\":4096,"
    "\"vcodec\":\"none\",\"acodec\":\"mp4a\"}]}";
  SiteGrabProbe probe = {0};
  cr_assert(site_grab_parse_probe_json(json, strlen(json), &probe));
  cr_assert_str_eq(probe.title, "Fixture video");
  cr_assert_eq(probe.format_count, 2);
  cr_assert_str_eq(probe.formats[0].id, "v720");
  cr_assert_eq(probe.formats[0].width, 1280);
  cr_assert_eq(probe.formats[0].height, 720);
  cr_assert_eq(probe.formats[0].size_bytes, 1048576);
  cr_assert(probe.formats[0].has_video);
  cr_assert_not(probe.formats[0].has_audio);
  cr_assert(probe.formats[1].size_estimated);
  cr_assert_not(probe.formats[1].has_video);
  cr_assert(probe.formats[1].has_audio);
  cr_assert_not(site_grab_parse_probe_json("{}", 2, &probe));
  cr_assert_not(site_grab_parse_probe_json("{", 1, &probe));
  const char unsafe[] = "{\"title\":\"Fixture\",\"formats\":[{"
    "\"format_id\":\"v+other\",\"ext\":\"mp4\",\"vcodec\":\"avc1\","
    "\"acodec\":\"none\"}]}";
  cr_assert_not(site_grab_parse_probe_json(unsafe, strlen(unsafe), &probe));
  const char projected[] = "\"Fixture video\"\n[{\"format_id\":\"v720\","
    "\"ext\":\"mp4\",\"height\":720,\"vcodec\":\"avc1\","
    "\"acodec\":\"none\"}]\n";
  cr_assert(site_grab_parse_probe_output(projected, strlen(projected), &probe));
  cr_assert_str_eq(probe.title, "Fixture video");
  cr_assert_eq(probe.formats[0].height, 720);
}

Test(site_grab, probe_retains_higher_resolutions_when_format_count_is_bounded) {
  char json[18000];
  size_t used = 0;
  int n = snprintf(json, sizeof(json), "{\"title\":\"Fixture\",\"formats\":[");
  cr_assert_geq(n, 0); cr_assert_lt((size_t)n, sizeof(json)); used = (size_t)n;
  for (int i = 0; i < 65; i++) {
    n = snprintf(json + used, sizeof(json) - used,
      "%s{\"format_id\":\"f%d\",\"ext\":\"mp4\",\"height\":%d,"
      "\"vcodec\":\"avc1\",\"acodec\":\"none\"}", i ? "," : "", i, i + 1);
    cr_assert_geq(n, 0); cr_assert_lt((size_t)n, sizeof(json) - used);
    used += (size_t)n;
  }
  n = snprintf(json + used, sizeof(json) - used, "]}");
  cr_assert_geq(n, 0); cr_assert_lt((size_t)n, sizeof(json) - used);
  used += (size_t)n;
  SiteGrabProbe probe = {0};
  cr_assert(site_grab_parse_probe_json(json, used, &probe));
  cr_assert_eq(probe.format_count, SITE_GRAB_PROBE_MAX_FORMATS);
  bool highest_found = false;
  for (size_t i = 0; i < probe.format_count; i++)
    if (strcmp(probe.formats[i].id, "f64") == 0) highest_found = true;
  cr_assert(highest_found);
}
