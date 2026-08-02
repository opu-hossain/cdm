#ifndef GUI_GUI_WORKER_H
#define GUI_GUI_WORKER_H

#include "webview/webview.h"
#include <stdbool.h>
#include <stdint.h>

void gui_worker_start(webview_t w);
void gui_worker_stop(void);

void gui_worker_enqueue_add_download(const char *seq, const char *url,
                                     const char *dest_path, const char *cookie,
                                     const char *referrer,
                                     const char *extra_headers,
                                     const char *expected_sha256,
                                     uint64_t speed_limit_bps);
void gui_worker_enqueue_pause(const char *seq, uint32_t id);
void gui_worker_enqueue_resume(const char *seq, uint32_t id);
void gui_worker_enqueue_cancel(const char *seq, uint32_t id);

// NEW — on-demand details fetch, dispatched to JS via c_get_details's
// Promise (seq), not via the periodic model-refresh path.
void gui_worker_enqueue_get_details(const char *seq, uint32_t id);

#endif
