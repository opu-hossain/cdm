// SPDX-License-Identifier: MIT
#include "../src/engine/site_grab.h"
#include <criterion/criterion.h>
#include <stdint.h>
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
