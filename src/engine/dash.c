// SPDX-License-Identifier: MIT
#include "dash.h"
#include <ctype.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DASH_NS "urn:mpeg:dash:schema:mpd:2011"
typedef struct {
  DashResult result;
  const char *message;
} Parse;
static bool fail(Parse *p, DashResult result, const char *message) {
  if (p->result == DASH_OK) {
    p->result = result;
    p->message = message;
  }
  return false;
}
static bool named(xmlNode *n, const char *name) {
  return n && n->type == XML_ELEMENT_NODE &&
         !xmlStrcmp(n->name, (const xmlChar *)name) &&
         (!n->ns || !n->ns->href ||
          !xmlStrcmp(n->ns->href, (const xmlChar *)DASH_NS));
}
static xmlNode *child(Parse *p, xmlNode *n, const char *name) {
  xmlNode *found = NULL;
  for (xmlNode *c = n ? n->children : NULL; c; c = c->next)
    if (named(c, name)) {
      if (found) {
        fail(p, DASH_INVALID, "duplicate singleton element");
        return NULL;
      }
      found = c;
    }
  return found;
}
static char *property(xmlNode *n, const char *name) {
  return (char *)xmlGetProp(n, (const xmlChar *)name);
}
static bool uint_value(Parse *p, const char *s, uint64_t *out) {
  if (!s || !*s)
    return fail(p, DASH_INVALID, "missing integer");
  uint64_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9' || v > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
      return fail(p, DASH_INVALID, "invalid or overflowing integer");
    v = v * 10 + (unsigned)(*s - '0');
  }
  *out = v;
  return true;
}
static char *inherited(xmlNode **nodes, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++) {
    char *s = property(nodes[i], name);
    if (s)
      return s;
  }
  return NULL;
}
static bool number(Parse *p, xmlNode **nodes, size_t count, const char *name,
                   uint64_t def, uint64_t *out) {
  char *s = inherited(nodes, count, name);
  *out = def;
  if (!s)
    return true;
  bool ok = uint_value(p, s, out);
  xmlFree(s);
  return ok;
}
static bool duration(Parse *p, xmlNode *n, const char *name, double *out) {
  char *s = property(n, name);
  if (!s)
    return true;
  const char *t = s;
  long double seconds = 0;
  int last = 0;
  bool any = false, ok = true;
  if (strncmp(t, "PT", 2))
    ok = false;
  else
    t += 2;
  while (ok && *t) {
    if (!isdigit((unsigned char)*t)) {
      ok = false;
      break;
    }
    long double v = 0;
    while (isdigit((unsigned char)*t)) {
      v = v * 10 + (*t++ - '0');
      if (v > 1e12L) {
        ok = false;
        break;
      }
    }
    bool fraction = false;
    if (*t == '.') {
      fraction = true;
      t++;
      if (!isdigit((unsigned char)*t))
        ok = false;
      long double scale = .1L;
      while (isdigit((unsigned char)*t)) {
        v += (*t++ - '0') * scale;
        scale *= .1L;
      }
    }
    int rank = *t == 'H' ? 1 : *t == 'M' ? 2 : *t == 'S' ? 3 : 0;
    if (!rank || rank <= last || (fraction && rank != 3)) {
      ok = false;
      break;
    }
    seconds += v * (rank == 1 ? 3600 : rank == 2 ? 60 : 1);
    last = rank;
    any = true;
    t++;
  }
  xmlFree(s);
  if (!ok || !any || seconds <= 0 || seconds > 1e12L)
    return fail(p, DASH_INVALID,
                "invalid ISO duration (PT hours/minutes/seconds required)");
  *out = (double)seconds;
  return true;
}
static bool resolve(Parse *p, const char *base, const char *ref, char *out) {
  CURLU *u = curl_url();
  char *url = NULL, *scheme = NULL, *user = NULL, *password = NULL;
  bool ok = u && strlen(ref) < DASH_URL_MAX;
  if (ok)
    ok = curl_url_set(u, CURLUPART_URL, base, 0) == CURLUE_OK &&
         curl_url_set(u, CURLUPART_URL, ref, 0) == CURLUE_OK &&
         curl_url_get(u, CURLUPART_SCHEME, &scheme, 0) == CURLUE_OK &&
         (!strcmp(scheme, "http") || !strcmp(scheme, "https"));
  if (ok && (curl_url_get(u, CURLUPART_USER, &user, 0) == CURLUE_OK ||
             curl_url_get(u, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK))
    ok = false;
  if (ok)
    ok = curl_url_set(u, CURLUPART_FRAGMENT, NULL, 0) == CURLUE_OK &&
         curl_url_get(u, CURLUPART_URL, &url, 0) == CURLUE_OK &&
         strlen(url) < DASH_URL_MAX;
  if (ok)
    strcpy(out, url);
  curl_free(url);
  curl_free(scheme);
  curl_free(user);
  curl_free(password);
  curl_url_cleanup(u);
  return ok || fail(p, DASH_INVALID, "invalid or oversized HTTP(S) URL");
}
static bool base_for(Parse *p, xmlNode **levels, const char *base, char *out) {
  if (!resolve(p, base, base, out))
    return false;
  for (size_t i = 0; i < 4; i++) {
    xmlNode *b = child(p, levels[i], "BaseURL");
    if (p->result)
      return false;
    if (!b)
      continue;
    char *s = (char *)xmlNodeGetContent(b);
    if (!s)
      return fail(p, DASH_NO_MEMORY, "base URL allocation failed");
    char *start = s;
    while (isspace((unsigned char)*start))
      start++;
    size_t len = strlen(start);
    while (len && isspace((unsigned char)start[len - 1]))
      start[--len] = 0;
    char next[DASH_URL_MAX];
    bool ok = *start && resolve(p, out, start, next);
    xmlFree(s);
    if (!ok)
      return fail(p, DASH_INVALID, "empty base URL");
    strcpy(out, next);
  }
  return true;
}
static bool append_text(Parse *p, char *out, size_t *length, const char *text,
                        size_t count) {
  if (count >= DASH_URL_MAX - *length)
    return fail(p, DASH_LIMIT, "expanded segment URL too long");
  memcpy(out + *length, text, count);
  *length += count;
  out[*length] = 0;
  return true;
}
static bool expand(Parse *p, const char *pattern, const DashTrack *track,
                   uint64_t num, uint64_t time, char *out) {
  size_t len = 0;
  out[0] = 0;
  while (*pattern) {
    if (*pattern != '$') {
      if (!append_text(p, out, &len, pattern++, 1))
        return false;
      continue;
    }
    pattern++;
    if (*pattern == '$') {
      if (!append_text(p, out, &len, "$", 1))
        return false;
      pattern++;
      continue;
    }
    const char *end = strchr(pattern, '$');
    if (!end)
      return fail(p, DASH_INVALID, "unterminated template identifier");
    size_t n = (size_t)(end - pattern);
    char token[64];
    if (n >= sizeof(token))
      return fail(p, DASH_UNSUPPORTED, "unsupported template identifier");
    memcpy(token, pattern, n);
    token[n] = 0;
    char *fmt = strchr(token, '%');
    int width = 0;
    if (fmt) {
      *fmt++ = 0;
      if (*fmt++ != '0')
        return fail(p, DASH_UNSUPPORTED, "unsupported template numeric format");
      const char *start = fmt;
      while (isdigit((unsigned char)*fmt)) {
        width = width * 10 + (*fmt++ - '0');
        if (width > 20)
          return fail(p, DASH_LIMIT, "numeric padding too large");
      }
      if (fmt == start || strcmp(fmt, "d"))
        return fail(p, DASH_UNSUPPORTED, "unsupported template numeric format");
    }
    char digits[32];
    const char *value = digits;
    uint64_t v;
    if (!strcmp(token, "RepresentationID")) {
      if (fmt || !track->representation_id[0])
        return fail(p, DASH_INVALID, "missing or formatted representation ID");
      value = track->representation_id;
    } else {
      if (!strcmp(token, "Number"))
        v = num;
      else if (!strcmp(token, "Time"))
        v = time;
      else if (!strcmp(token, "Bandwidth"))
        v = track->bandwidth;
      else
        return fail(p, DASH_UNSUPPORTED, "unsupported template identifier");
      int wrote = snprintf(digits, sizeof(digits), "%0*llu", width,
                           (unsigned long long)v);
      if (wrote < 0 || (size_t)wrote >= sizeof(digits))
        return fail(p, DASH_LIMIT, "template formatting overflow");
    }
    if (!append_text(p, out, &len, value, strlen(value)))
      return false;
    pattern = end + 1;
  }
  return true;
}
static bool add_segment(Parse *p, DashTrack *track, uint64_t number,
                        uint64_t time, uint64_t d) {
  if (track->segment_count == DASH_MAX_SEGMENTS)
    return fail(p, DASH_LIMIT, "too many segments");
  DashSegment *s = &track->segments[track->segment_count++];
  s->number = number;
  s->time = time;
  s->duration = d;
  return true;
}
static bool timeline(Parse *p, xmlNode *tl, DashTrack *track, uint64_t start,
                     double seconds) {
  uint64_t time = 0, num = start;
  bool have = false;
  for (xmlNode *s = tl->children; s; s = s->next) {
    if (s->type != XML_ELEMENT_NODE)
      continue;
    if (!named(s, "S"))
      return fail(p, DASH_UNSUPPORTED, "unsupported timeline element");
    xmlNode *one[] = {s};
    uint64_t d = 0, t = 0;
    char *explicit_t = property(s, "t");
    bool ok = number(p, one, 1, "d", 0, &d) && d;
    if (explicit_t) {
      ok = ok && uint_value(p, explicit_t, &t);
      xmlFree(explicit_t);
      if (have && t < time)
        ok = false;
      time = t;
    }
    if (!ok)
      return fail(p, DASH_INVALID, "invalid timeline duration or overlap");
    char *repeat = property(s, "r");
    uint64_t repetitions = 0, count = 1;
    if (repeat && !strcmp(repeat, "-1")) {
      xmlNode *next = s->next;
      while (next && next->type != XML_ELEMENT_NODE)
        next = next->next;
      uint64_t bound = 0;
      if (next) {
        char *nt = property(next, "t");
        ok = nt && uint_value(p, nt, &bound);
        xmlFree(nt);
        if (!ok) {
          xmlFree(repeat);
          return fail(p, DASH_UNSUPPORTED,
                      "negative repeat needs next explicit time");
        }
      } else if (seconds > 0) {
        long double end = (long double)seconds * track->timescale +
                          track->presentation_time_offset;
        if (end > UINT64_MAX) {
          xmlFree(repeat);
          return fail(p, DASH_INVALID, "period timeline overflow");
        }
        bound = (uint64_t)ceill(end);
      } else {
        xmlFree(repeat);
        return fail(p, DASH_UNSUPPORTED, "unbounded negative repeat");
      }
      if (bound <= time) {
        xmlFree(repeat);
        return fail(p, DASH_INVALID, "invalid repeat boundary");
      }
      count = (bound - time) / d + ((bound - time) % d != 0);
      if (next && (bound - time) % d) {
        xmlFree(repeat);
        return fail(p, DASH_INVALID, "timeline repeat overlaps next entry");
      }
    } else if (repeat) {
      ok = uint_value(p, repeat, &repetitions);
      if (!ok || repetitions == UINT64_MAX) {
        xmlFree(repeat);
        return fail(p, DASH_INVALID, "repeat overflow");
      }
      count = repetitions + 1;
    }
    xmlFree(repeat);
    if (count > DASH_MAX_SEGMENTS - track->segment_count)
      return fail(p, DASH_LIMIT, "too many timeline segments");
    for (uint64_t j = 0; j < count; j++) {
      if (time > UINT64_MAX - d || num > UINT64_MAX - j)
        return fail(p, DASH_INVALID, "segment timeline overflow");
      if (!add_segment(p, track, num + j, time, d))
        return false;
      time += d;
    }
    if (num >
        UINT64_MAX -
            count) { /* Last number may be UINT64_MAX, but cannot advance. */
      xmlNode *next = s->next;
      while (next && next->type != XML_ELEMENT_NODE)
        next = next->next;
      if (next)
        return fail(p, DASH_INVALID, "segment number overflow");
    } else
      num += count;
    have = true;
  }
  return track->segment_count > 0 || fail(p, DASH_INVALID, "empty timeline");
}
static bool fill_track(Parse *p, xmlNode **levels, const char *base,
                       double seconds, DashTrack *track) {
  char resolved[DASH_URL_MAX];
  if (!base_for(p, levels, base, resolved))
    return false;
  xmlNode *templates[4] = {0}, *lists[4] = {0};
  size_t nt = 0, nl = 0;
  bool chose = false, as_list = false;
  for (int i = 3; i >= 0; i--) {
    if (child(p, levels[i], "ContentProtection"))
      return fail(p, DASH_UNSUPPORTED, "DRM/content protection unsupported");
    if (child(p, levels[i], "SegmentBase"))
      return fail(p, DASH_UNSUPPORTED, "SegmentBase unsupported");
    xmlNode *t = child(p, levels[i], "SegmentTemplate"),
            *l = child(p, levels[i], "SegmentList");
    if (p->result)
      return false;
    if (t && l)
      return fail(p, DASH_INVALID, "conflicting segment schemes");
    if (!chose && (t || l)) {
      as_list = l != NULL;
      chose = true;
    }
    if (t)
      templates[nt++] = t;
    if (l)
      lists[nl++] = l;
  }
  if (!chose)
    return fail(p, DASH_UNSUPPORTED, "no supported segment scheme");
  xmlNode **nodes = as_list ? lists : templates;
  size_t count = as_list ? nl : nt;
  char *delta = inherited(nodes, count, "eptDelta");
  bool shifted = delta && strcmp(delta, "0");
  xmlFree(delta);
  if (shifted)
    return fail(p, DASH_UNSUPPORTED, "nonzero eptDelta unsupported");
  if (!number(p, nodes, count, "timescale", 1, &track->timescale) ||
      !track->timescale ||
      !number(p, nodes, count, "presentationTimeOffset", 0,
              &track->presentation_time_offset))
    return fail(p, DASH_INVALID, "invalid timescale");
  uint64_t start = 1, d = 0;
  if (!number(p, nodes, count, "startNumber", 1, &start) ||
      !number(p, nodes, count, "duration", 0, &d))
    return false;
  track->segments = calloc(DASH_MAX_SEGMENTS, sizeof(*track->segments));
  if (!track->segments)
    return fail(p, DASH_NO_MEMORY, "segment allocation failed");
  xmlNode *tl = NULL, *init = NULL, *urls = NULL;
  for (size_t i = 0; i < count; i++) {
    if (!tl)
      tl = child(p, nodes[i], "SegmentTimeline");
    if (!init)
      init = child(p, nodes[i], "Initialization");
    if (!urls)
      for (xmlNode *c = nodes[i]->children; c; c = c->next)
        if (named(c, "SegmentURL")) {
          urls = nodes[i];
          break;
        }
  }
  if (p->result)
    return false;
  if (tl && !timeline(p, tl, track, start, seconds))
    return false;
  if (as_list) {
    if (!urls)
      return fail(p, DASH_INVALID, "empty segment list");
    if (init) {
      char *range = property(init, "range"),
           *source = property(init, "sourceURL");
      bool ranged = range != NULL;
      bool ok = !range && source &&
                resolve(p, resolved, source, track->initialization_url);
      xmlFree(range);
      xmlFree(source);
      if (!ok)
        return fail(p, ranged ? DASH_UNSUPPORTED : DASH_INVALID,
                    "initialization range or URL unsupported");
    }
    size_t index = 0;
    for (xmlNode *c = urls->children; c; c = c->next)
      if (named(c, "SegmentURL")) {
        char *range = property(c, "mediaRange"),
             *irange = property(c, "indexRange"), *media = property(c, "media"),
             *idx = property(c, "index");
        bool ranged = range || irange || idx;
        xmlFree(range);
        xmlFree(irange);
        xmlFree(idx);
        if (ranged) {
          xmlFree(media);
          return fail(p, DASH_UNSUPPORTED,
                      "segment byte ranges/indexes unsupported");
        }
        if (!tl) {
          if (start > UINT64_MAX - index || (d && index > UINT64_MAX / d)) {
            xmlFree(media);
            return fail(p, DASH_INVALID, "segment list overflow");
          }
          if (!add_segment(p, track, start + index, d * index, d)) {
            xmlFree(media);
            return false;
          }
        }
        if (index >= track->segment_count) {
          xmlFree(media);
          return fail(p, DASH_INVALID, "segment list/timeline count mismatch");
        }
        bool ok =
            media && resolve(p, resolved, media, track->segments[index].url);
        xmlFree(media);
        if (!ok)
          return fail(p, DASH_INVALID, "missing segment URL");
        index++;
      }
    if (index != track->segment_count)
      return fail(p, DASH_INVALID, "segment list/timeline count mismatch");
  } else {
    char *media = inherited(nodes, count, "media"),
         *initial = inherited(nodes, count, "initialization");
    if (!media) {
      xmlFree(initial);
      return fail(p, DASH_INVALID, "missing media template");
    }
    bool ok = true;
    if (!tl) {
      long double ticks = (long double)seconds * track->timescale;
      if (!d || seconds <= 0)
        ok = fail(
            p, DASH_INVALID,
            "duration template requires finite period and positive duration");
      else if (ticks > UINT64_MAX)
        ok = fail(p, DASH_INVALID, "period duration overflow");
      else {
        uint64_t total = (uint64_t)ceill(ticks / d);
        if (total > DASH_MAX_SEGMENTS)
          ok = fail(p, DASH_LIMIT, "too many segments");
        for (uint64_t i = 0; ok && i < total; i++) {
          if (start > UINT64_MAX - i ||
              (d && i > (UINT64_MAX - track->presentation_time_offset) / d))
            ok = fail(p, DASH_INVALID, "segment number/time overflow");
          else
            ok = add_segment(p, track, start + i,
                             track->presentation_time_offset + d * i, d);
        }
      }
    }
    char expanded[DASH_URL_MAX];
    if (ok && initial)
      ok = expand(p, initial, track, start, 0, expanded) &&
           resolve(p, resolved, expanded, track->initialization_url);
    for (size_t i = 0; ok && i < track->segment_count; i++)
      ok = expand(p, media, track, track->segments[i].number,
                  track->segments[i].time, expanded) &&
           resolve(p, resolved, expanded, track->segments[i].url);
    xmlFree(media);
    xmlFree(initial);
    if (!ok)
      return false;
  }
  track->present = true;
  return true;
}
static bool validate_tree(Parse *p, xmlNode *n, unsigned depth, size_t *count) {
  if (depth > 32)
    return fail(p, DASH_LIMIT, "XML nesting too deep");
  for (; n; n = n->next) {
    if (++*count > 32768)
      return fail(p, DASH_LIMIT, "too many XML nodes");
    if (n->type == XML_ENTITY_REF_NODE || n->type == XML_DTD_NODE)
      return fail(p, DASH_UNSUPPORTED, "XML entities/DTD unsupported");
    if (n->type == XML_ELEMENT_NODE)
      for (xmlAttr *a = n->properties; a; a = a->next)
        if (a->ns && a->ns->href &&
            (!xmlStrcmp(a->ns->href,
                        (xmlChar *)"http://www.w3.org/1999/xlink") ||
             (!xmlStrcmp(a->ns->href,
                         (xmlChar *)"http://www.w3.org/XML/1998/namespace") &&
              !xmlStrcmp(a->name, (xmlChar *)"base"))))
          return fail(p, DASH_UNSUPPORTED,
                      "external links/xml:base unsupported");
    if (n->children && !validate_tree(p, n->children, depth + 1, count))
      return false;
  }
  return true;
}
void dash_manifest_free(DashManifest *m) {
  if (!m)
    return;
  free(m->video.segments);
  free(m->audio.segments);
  memset(m, 0, sizeof(*m));
}
DashResult dash_parse(const char *text, size_t length, const char *base,
                      DashManifest *out, char *error, size_t error_size) {
  Parse p = {DASH_OK, NULL};
  DashManifest m = {0};
  xmlDoc *doc = NULL;
  if (out)
    memset(out, 0, sizeof(*out));
  if (error && error_size)
    error[0] = 0;
  if (!out || !text || !base || !length || memchr(text, 0, length)) {
    fail(&p, DASH_INVALID, "invalid parser arguments or embedded NUL");
    goto done;
  }
  if (length > DASH_MAX_MANIFEST_BYTES) {
    fail(&p, DASH_LIMIT, "manifest too large");
    goto done;
  }
  /* UTF-8 is forced: ASCII DTD delimiters cannot hide in UTF-16/32. No DTDLOAD,
   * NOENT, RECOVER, XINCLUDE or HUGE; default libxml limits remain in effect.
   */
  for (size_t i = 0; i + 9 <= length; i++)
    if (!memcmp(text + i, "<!DOCTYPE", 9) ||
        !memcmp(text + i, "<!ENTITY ", 9)) {
      fail(&p, DASH_UNSUPPORTED, "XML entities/DTD unsupported");
      goto done;
    }
  int options = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
#if LIBXML_VERSION >= 21300
  options |= XML_PARSE_NO_XXE;
#endif
  doc = xmlReadMemory(text, (int)length, NULL, "UTF-8", options);
  if (!doc) {
    fail(&p, DASH_INVALID, "malformed UTF-8 XML");
    goto done;
  }
  size_t count = 0;
  if (doc->intSubset || doc->extSubset ||
      !validate_tree(&p, doc->children, 0, &count)) {
    fail(&p, DASH_UNSUPPORTED, "XML DTD unsupported");
    goto done;
  }
  xmlNode *root = xmlDocGetRootElement(doc);
  if (!named(root, "MPD")) {
    fail(&p, DASH_INVALID, "expected DASH MPD root");
    goto done;
  }
  char *type = property(root, "type");
  bool live = type && strcmp(type, "static");
  xmlFree(type);
  if (live) {
    fail(&p, DASH_UNSUPPORTED, "dynamic MPD unsupported");
    goto done;
  }
  xmlNode *period = child(&p, root, "Period");
  if (p.result) {
    p.result = DASH_UNSUPPORTED;
    p.message = "multi-period MPD unsupported";
    goto done;
  }
  if (!period) {
    fail(&p, DASH_INVALID, "missing period");
    goto done;
  }
  if (!duration(&p, root, "mediaPresentationDuration", &m.duration) ||
      !duration(&p, period, "duration", &m.duration))
    goto done;
  char *start_time = property(period, "start");
  if (start_time && strcmp(start_time, "PT0S")) {
    xmlFree(start_time);
    fail(&p, DASH_UNSUPPORTED, "nonzero period start unsupported");
    goto done;
  }
  xmlFree(start_time);
  xmlNode *chosen_video = NULL, *chosen_audio = NULL, *video_set = NULL,
          *audio_set = NULL;
  uint64_t video_bw = 0, audio_bw = 0;
  for (xmlNode *set = period->children; set; set = set->next)
    if (named(set, "AdaptationSet")) {
      for (xmlNode *rep = set->children; rep; rep = rep->next)
        if (named(rep, "Representation")) {
          xmlNode *anc[] = {rep, set};
          char *kind = inherited(anc, 2, "contentType"),
               *mime = inherited(anc, 2, "mimeType");
          bool video = kind ? !strcmp(kind, "video")
                            : mime && !strncmp(mime, "video/", 6),
               audio = kind ? !strcmp(kind, "audio")
                            : mime && !strncmp(mime, "audio/", 6);
          xmlFree(kind);
          xmlFree(mime);
          if (!video && !audio)
            continue;
          char *b = property(rep, "bandwidth");
          uint64_t bw = 0;
          bool ok = uint_value(&p, b, &bw) && bw;
          xmlFree(b);
          if (!ok) {
            fail(&p, DASH_INVALID, "invalid representation bandwidth");
            goto done;
          }
          if (video && (!chosen_video || bw > video_bw)) {
            chosen_video = rep;
            video_set = set;
            video_bw = bw;
          }
          if (audio && (!chosen_audio || bw > audio_bw)) {
            chosen_audio = rep;
            audio_set = set;
            audio_bw = bw;
          }
        }
    }
  if (!chosen_video && !chosen_audio) {
    fail(&p, DASH_UNSUPPORTED, "no audio/video representation");
    goto done;
  }
  xmlNode *chosen[] = {chosen_video, chosen_audio},
          *sets[] = {video_set, audio_set};
  DashTrack *tracks[] = {&m.video, &m.audio};
  uint64_t bws[] = {video_bw, audio_bw};
  for (size_t i = 0; i < 2; i++)
    if (chosen[i]) {
      tracks[i]->bandwidth = bws[i];
      char *id = property(chosen[i], "id");
      if (id) {
        if (strlen(id) >= sizeof(tracks[i]->representation_id)) {
          xmlFree(id);
          fail(&p, DASH_LIMIT, "representation ID too long");
          goto done;
        }
        strcpy(tracks[i]->representation_id, id);
        xmlFree(id);
      }
      xmlNode *levels[] = {root, period, sets[i], chosen[i]};
      if (!fill_track(&p, levels, base, m.duration, tracks[i]))
        goto done;
    }
done:
  xmlFreeDoc(doc);
  if (p.result == DASH_OK)
    *out = m;
  else
    dash_manifest_free(&m);
  if (p.result != DASH_OK && error && error_size) {
    const char *s = p.message ? p.message : "parse failed";
    size_t n = strlen(s);
    if (n >= error_size)
      n = error_size - 1;
    memcpy(error, s, n);
    error[n] = 0;
  }
  return p.result;
}
