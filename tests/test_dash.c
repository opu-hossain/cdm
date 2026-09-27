// SPDX-License-Identifier: MIT
#include "../src/engine/dash.h"
#include <criterion/criterion.h>
#include <string.h>
static DashManifest parse_ok(const char *xml) {
  DashManifest out = {0};
  char error[128];
  cr_assert_eq(dash_parse(xml, strlen(xml),
                          "https://example.invalid/dir/main.mpd", &out, error,
                          sizeof(error)),
               DASH_OK, "%s", error);
  return out;
}
Test(dash, number_inheritance_and_selection) {
  DashManifest m = parse_ok(
      "<MPD xmlns='urn:mpeg:dash:schema:mpd:2011' "
      "mediaPresentationDuration='PT5S'><BaseURL>../media/</"
      "BaseURL><Period><AdaptationSet mimeType='video/mp4'><SegmentTemplate "
      "timescale='10' duration='20' startNumber='7' "
      "initialization='$RepresentationID$/init.mp4' "
      "media='$RepresentationID$/s-$Number%03d$-$Bandwidth$.m4s'/"
      "><Representation id='low' bandwidth='10'/><Representation id='high' "
      "bandwidth='20'/><Representation id='tie' "
      "bandwidth='20'/></AdaptationSet><AdaptationSet "
      "contentType='audio'><Representation id='audio' "
      "bandwidth='5'><SegmentList timescale='2' duration='2'><Initialization "
      "sourceURL='audio/init.mp4'/><SegmentURL "
      "media='audio/a.m4s'/><SegmentURL "
      "media='audio/b.m4s'/></SegmentList></Representation></AdaptationSet></"
      "Period></MPD>");
  cr_assert_eq(m.video.bandwidth, 20);
  cr_assert_str_eq(m.video.representation_id, "high");
  cr_assert_eq(m.video.segment_count, 3);
  cr_assert_str_eq(m.video.segments[0].url,
                   "https://example.invalid/media/high/s-007-20.m4s");
  cr_assert_str_eq(m.video.initialization_url,
                   "https://example.invalid/media/high/init.mp4");
  cr_assert_eq(m.video.segments[2].time, 40);
  cr_assert_eq(m.audio.segment_count, 2);
  cr_assert_eq(m.audio.timescale, 2);
  dash_manifest_free(&m);
}
Test(dash, timeline_time_and_negative_repeat) {
  DashManifest m = parse_ok(
      "<MPD><Period duration='PT6S'><AdaptationSet "
      "contentType='video'><SegmentTemplate timescale='10' "
      "presentationTimeOffset='100' startNumber='4' "
      "media='s-$Time$.m4s'><SegmentTimeline><S t='100' d='20' r='1'/><S "
      "d='10' r='-1'/></SegmentTimeline></SegmentTemplate><Representation "
      "bandwidth='100' id='v'/></AdaptationSet></Period></MPD>");
  cr_assert_eq(m.video.segment_count, 4);
  cr_assert_eq(m.video.segments[0].time, 100);
  cr_assert_eq(m.video.segments[3].time, 150);
  cr_assert_eq(m.video.segments[3].number, 7);
  dash_manifest_free(&m);
  m = parse_ok(
      "<MPD><Period><AdaptationSet contentType='audio'><Representation "
      "bandwidth='1'><SegmentTemplate media='s-$Time$.m4s'><SegmentTimeline><S "
      "t='0' d='2' r='-1'/><S t='6' "
      "d='3'/></SegmentTimeline></SegmentTemplate></Representation></"
      "AdaptationSet></Period></MPD>");
  cr_assert_eq(m.audio.segment_count, 4);
  cr_assert_eq(m.audio.segments[3].time, 6);
  dash_manifest_free(&m);
}
Test(dash, list_timeline_and_relative_base) {
  DashManifest m = parse_ok(
      "<MPD><Period><BaseURL>v/</BaseURL><AdaptationSet "
      "contentType='video'><SegmentList timescale='100'><Initialization "
      "sourceURL='init.mp4'/><SegmentTimeline><S d='25' "
      "r='1'/></SegmentTimeline><SegmentURL media='a.m4s'/><SegmentURL "
      "media='b.m4s'/></SegmentList><Representation "
      "bandwidth='10'><BaseURL>../chosen/</BaseURL></Representation></"
      "AdaptationSet></Period></MPD>");
  cr_assert_str_eq(m.video.segments[1].url,
                   "https://example.invalid/dir/chosen/b.m4s");
  cr_assert_eq(m.video.segments[1].time, 25);
  dash_manifest_free(&m);
}
static void reject(const char *xml, DashResult expected) {
  DashManifest m = {0};
  char e[128];
  cr_assert_eq(dash_parse(xml, strlen(xml), "https://example.invalid/m.mpd", &m,
                          e, sizeof(e)),
               expected, "%s", e);
  cr_assert_not_null(e);
  cr_assert_neq(e[0], 0);
  cr_assert_null(m.video.segments);
  cr_assert_null(m.audio.segments);
}
Test(dash, unsupported_layouts) {
  reject("<MPD type='dynamic'><Period/></MPD>", DASH_UNSUPPORTED);
  reject("<MPD><Period/><Period/></MPD>", DASH_UNSUPPORTED);
  reject("<MPD><Period><AdaptationSet contentType='video'><Representation "
         "bandwidth='1'><SegmentBase/></Representation></AdaptationSet></"
         "Period></MPD>",
         DASH_UNSUPPORTED);
  reject("<MPD><Period><AdaptationSet "
         "contentType='video'><ContentProtection/><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_UNSUPPORTED);
  reject("<MPD><Period><AdaptationSet contentType='video'><Representation "
         "bandwidth='1'><SegmentList><SegmentURL media='x' "
         "mediaRange='0-1'/></SegmentList></Representation></AdaptationSet></"
         "Period></MPD>",
         DASH_UNSUPPORTED);
}
Test(dash, xml_security_and_validation) {
  reject(
      "<!DOCTYPE MPD [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><MPD>&x;</MPD>",
      DASH_UNSUPPORTED);
  reject("<MPD><Period></MPD>", DASH_INVALID);
  reject("<x:MPD xmlns:x='urn:foreign'><x:Period/></x:MPD>", DASH_INVALID);
  reject("<MPD><Period><AdaptationSet contentType='video'><Representation "
         "bandwidth='1'><SegmentList><SegmentURL "
         "media='file:///tmp/x'/></SegmentList></Representation></"
         "AdaptationSet></Period></MPD>",
         DASH_INVALID);
  reject("<MPD mediaPresentationDuration='PT1S'><Period><AdaptationSet "
         "contentType='video'><SegmentTemplate duration='0' "
         "media='x'/><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_INVALID);
  reject("<MPD><Period><AdaptationSet contentType='video'><SegmentTemplate "
         "media='x-$Bogus$'><SegmentTimeline><S "
         "d='1'/></SegmentTimeline></SegmentTemplate><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_UNSUPPORTED);
}
Test(dash, bounds_and_overflow) {
  reject("<MPD mediaPresentationDuration='PT5000S'><Period><AdaptationSet "
         "contentType='video'><SegmentTemplate duration='1' "
         "media='x-$Number$'/><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_LIMIT);
  reject("<MPD><Period><AdaptationSet contentType='video'><SegmentTemplate "
         "startNumber='18446744073709551615' "
         "media='x-$Number$'><SegmentTimeline><S d='1' "
         "r='1'/></SegmentTimeline></SegmentTemplate><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_INVALID);
  reject("<MPD><Period><AdaptationSet contentType='video'><SegmentTemplate "
         "media='x'><SegmentTimeline><S d='1' "
         "r='-1'/></SegmentTimeline></SegmentTemplate><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_UNSUPPORTED);
  char xml[] = "<MPD>\0</MPD>";
  DashManifest m = {0};
  cr_assert_eq(
      dash_parse(xml, sizeof(xml) - 1, "https://example.invalid/", &m, NULL, 0),
      DASH_INVALID);
}
Test(dash, simple_time_offset_and_overrides) {
  DashManifest m = parse_ok(
      "<MPD mediaPresentationDuration='PT2.5S'><Period><AdaptationSet "
      "contentType='video'><SegmentTemplate timescale='10' duration='10' "
      "presentationTimeOffset='50' media='parent-$Time$.m4s'/><Representation "
      "bandwidth='1'><SegmentTemplate "
      "media='s-$Time$.m4s'/></Representation></AdaptationSet></Period></MPD>");
  cr_assert_eq(m.video.segment_count, 3);
  cr_assert_eq(m.video.segments[0].time, 50);
  cr_assert_str_eq(m.video.segments[2].url,
                   "https://example.invalid/dir/s-70.m4s");
  dash_manifest_free(&m);
  reject("<MPD mediaPresentationDuration='PT1S'><Period><AdaptationSet "
         "contentType='video'><SegmentTemplate duration='1' eptDelta='-1' "
         "media='x-$Number$'/><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_UNSUPPORTED);
}
Test(dash, additional_xml_and_resource_guards) {
  reject("<MPD xmlns:xlink='http://www.w3.org/1999/xlink'><Period "
         "xlink:href='https://example.invalid/external'/></MPD>",
         DASH_UNSUPPORTED);
  reject("<MPD xml:base='https://example.invalid/'><Period/></MPD>",
         DASH_UNSUPPORTED);
  reject("<MPD><BaseURL>a/</BaseURL><BaseURL>b/</"
         "BaseURL><Period><AdaptationSet contentType='video'><Representation "
         "bandwidth='1'><SegmentList><SegmentURL "
         "media='x'/></SegmentList></Representation></AdaptationSet></Period></"
         "MPD>",
         DASH_INVALID);
  reject("<MPD><Period><AdaptationSet contentType='video'><SegmentTemplate "
         "media='$Number%099d$'><SegmentTimeline><S "
         "d='1'/></SegmentTimeline></SegmentTemplate><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_LIMIT);
  reject("<MPD><Period><AdaptationSet contentType='video'><SegmentTemplate "
         "media='$Number$'><SegmentTimeline><S d='1' "
         "r='4096'/></SegmentTimeline></SegmentTemplate><Representation "
         "bandwidth='1'/></AdaptationSet></Period></MPD>",
         DASH_LIMIT);
  char invalid[] = {'<', 'M', 'P', 'D', '>', (char)0xff, '<',
                    '/', 'M', 'P', 'D', '>', 0};
  reject(invalid, DASH_INVALID);
}
