#include "gui.h"
#include "../platform/ipc_socket.h"
#include "../platform/thread.h"
#include "../utils/config.h"
#include "../vendor/tinyfiledialogs.h"
#include "raylib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#define MAX_DISPLAYED 64

#ifdef DM_DEBUG
#define DM_LOG(...) printf(__VA_ARGS__)
#else
#define DM_LOG(...)
#endif

typedef struct {
  uint32_t id;
  char url[IPC_MAX_URL_LEN];
  float progress;
  char status[32];
} GuiRow;

static GuiRow g_rows[MAX_DISPLAYED];
static int g_row_count = 0;
static dm_mutex_t g_rows_mutex;
static uint32_t g_confirm_cancel_id = 0;

// TWO separate connections to the daemon:
// g_ipc_fd       -> used by the main thread for sending commands
// g_listener_fd  -> used by the listener thread for receiving events
static int g_ipc_fd = -1;
static int g_listener_fd = -1;

// Listener thread — uses its OWN socket, never touches g_ipc_fd
static int listener_thread_fn(void *arg) {
  (void)arg;
  for (;;) {
    MsgHeader hdr;
    if (ipc_read_exact(g_listener_fd, &hdr, sizeof(hdr)) != 0) {
      printf("Listener reconnecting...\n");
      dm_thread_sleep_ms(1000);
      if (g_listener_fd >= 0) {
        ipc_client_disconnect(g_listener_fd);
      }
      g_listener_fd = ipc_client_connect();
      continue;
    }

    if (hdr.type == MSG_STATUS_EVENT) {
      uint32_t id;
      float progress;
      char status[32] = {0};

      ipc_read_exact(g_listener_fd, &id, sizeof(id));
      ipc_read_exact(g_listener_fd, &progress, sizeof(progress));

      uint32_t slen = 0;
      ipc_read_exact(g_listener_fd, &slen, sizeof(slen));
      if (slen > 0) {
        if (slen < sizeof(status)) {
          ipc_read_exact(g_listener_fd, status, slen);
          status[slen] = '\0';
        } else {
          char discard[256];
          uint32_t remaining = slen;
          while (remaining > 0) {
            uint32_t chunk =
                remaining < sizeof(discard) ? remaining : sizeof(discard);
            if (ipc_read_exact(g_listener_fd, discard, chunk) != 0)
              break;
            remaining -= chunk;
          }
        }
      }

      dm_mutex_lock(&g_rows_mutex);
      for (int i = 0; i < g_row_count; i++) {
        if (g_rows[i].id == id) {
          g_rows[i].progress = progress;
          if (status[0]) {
            strncpy(g_rows[i].status, status, sizeof(g_rows[i].status) - 1);
          }
          break;
        }
      }
      dm_mutex_unlock(&g_rows_mutex);
    }
  }
  return 0;
}

// URL -> filename, and folder + filename -> full path.
static void filename_from_url(const char *url, char *out, size_t out_size) {
  const char *slash = strrchr(url, '/');
  const char *name = slash ? slash + 1 : url;

  char temp[512];
  strncpy(temp, name, sizeof(temp) - 1);
  temp[sizeof(temp) - 1] = '\0';

  char *query = strchr(temp, '?');
  if (query)
    *query = '\0';

  if (temp[0] == '\0') {
    strncpy(temp, "download.bin", sizeof(temp) - 1);
  }

  strncpy(out, temp, out_size - 1);
  out[out_size - 1] = '\0';
}

static void join_path(const char *dir, const char *filename, char *out,
                      size_t out_size) {
#ifdef _WIN32
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  size_t dir_len = strlen(dir);
  if (dir_len > 0 && dir[dir_len - 1] == sep) {
    snprintf(out, out_size, "%s%s", dir, filename);
  } else {
    snprintf(out, out_size, "%s%c%s", dir, sep, filename);
  }
}

static void default_downloads_dir(char *out, size_t out_size) {
  const char *dir = config_get_default_download_dir();
  strncpy(out, dir, out_size - 1);
  out[out_size - 1] = '\0';
}

static void ApplyModernTheme(void) {
  GuiSetStyle(DEFAULT, TEXT_SIZE, 16);
  GuiSetStyle(DEFAULT, TEXT_SPACING, 1);
  GuiSetStyle(DEFAULT, BORDER_COLOR_NORMAL,
              ColorToInt((Color){70, 70, 75, 255}));
  GuiSetStyle(DEFAULT, BASE_COLOR_NORMAL, ColorToInt((Color){45, 45, 50, 255}));
  GuiSetStyle(DEFAULT, TEXT_COLOR_NORMAL,
              ColorToInt((Color){230, 230, 230, 255}));

  GuiSetStyle(TEXTBOX, BASE_COLOR_NORMAL, ColorToInt((Color){30, 30, 35, 255}));
  GuiSetStyle(TEXTBOX, BASE_COLOR_PRESSED,
              ColorToInt((Color){40, 40, 45, 255}));
  GuiSetStyle(TEXTBOX, TEXT_COLOR_NORMAL,
              ColorToInt((Color){255, 255, 255, 255}));
  GuiSetStyle(TEXTBOX, BORDER_COLOR_PRESSED,
              ColorToInt((Color){90, 150, 255, 255}));
  GuiSetStyle(TEXTBOX, BORDER_WIDTH, 1);

  GuiSetStyle(BUTTON, BASE_COLOR_NORMAL,
              ColorToInt((Color){60, 120, 216, 255}));
  GuiSetStyle(BUTTON, BASE_COLOR_FOCUSED,
              ColorToInt((Color){80, 140, 236, 255}));
  GuiSetStyle(BUTTON, BASE_COLOR_PRESSED,
              ColorToInt((Color){40, 100, 196, 255}));
  GuiSetStyle(BUTTON, TEXT_COLOR_NORMAL,
              ColorToInt((Color){255, 255, 255, 255}));
  GuiSetStyle(BUTTON, BORDER_WIDTH, 0);

  GuiSetStyle(PROGRESSBAR, BASE_COLOR_NORMAL,
              ColorToInt((Color){30, 30, 35, 255}));
  GuiSetStyle(PROGRESSBAR, BASE_COLOR_PRESSED,
              ColorToInt((Color){60, 175, 110, 255}));
  GuiSetStyle(PROGRESSBAR, BORDER_WIDTH, 0);
}

int run_gui(void) {
  dm_mutex_init(&g_rows_mutex);

  // Connect to daemon (main thread's socket)
  g_ipc_fd = ipc_client_connect();
  if (g_ipc_fd < 0) {
    fprintf(stderr,
            "Cannot connect to daemon.\nStart it first: downloadmgr daemon\n");
    return 1;
  }

  // Create a SECOND connection for the listener thread
  g_listener_fd = ipc_client_connect();
  if (g_listener_fd < 0) {
    fprintf(stderr, "Cannot create listener connection.\n");
    ipc_client_disconnect(g_ipc_fd);
    return 1;
  }
  ipc_send_subscribe(g_listener_fd);

  // Start listener thread (uses g_listener_fd, never g_ipc_fd)
  dm_thread_t listener;
  dm_thread_create(&listener, listener_thread_fn, NULL);

  IpcDownloadRecord records[MAX_DISPLAYED];
  int n = ipc_send_list_all(g_ipc_fd, records, MAX_DISPLAYED);
  dm_mutex_lock(&g_rows_mutex);
  g_row_count = 0;
  for (int i = 0; i < n && g_row_count < MAX_DISPLAYED; i++) {
    g_rows[g_row_count].id = records[i].id;
    g_rows[g_row_count].progress = records[i].progress;
    strncpy(g_rows[g_row_count].url, records[i].url,
            sizeof(g_rows[g_row_count].url) - 1);
    g_rows[g_row_count].url[sizeof(g_rows[g_row_count].url) - 1] = '\0';
    strncpy(g_rows[g_row_count].status, records[i].status,
            sizeof(g_rows[g_row_count].status) - 1);
    g_rows[g_row_count].status[sizeof(g_rows[g_row_count].status) - 1] = '\0';
    g_row_count++;
  }
  dm_mutex_unlock(&g_rows_mutex);

  SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
  InitWindow(800, 600, "Download Manager");
  SetTargetFPS(60);
  ApplyModernTheme();

  char url_buffer[IPC_MAX_URL_LEN] = {0};
  char dest_dir[IPC_MAX_PATH_LEN] = {0};
  default_downloads_dir(dest_dir, sizeof(dest_dir));

  bool url_edit = false;
  bool dest_edit = false;

  Color bgColor = (Color){20, 20, 23, 255};
  Color headerColor = (Color){32, 32, 36, 255};

  while (!WindowShouldClose()) {
    BeginDrawing();
    ClearBackground(bgColor);

    DrawRectangle(0, 0, GetScreenWidth(), 180, headerColor);
    DrawLine(0, 180, GetScreenWidth(), 180, (Color){50, 50, 55, 255});

    GuiLabel((Rectangle){30, 20, 300, 20}, "Download URL");
    GuiLabel((Rectangle){30, 95, 300, 20}, "Save To Folder");

    Rectangle url_rect = {30, 45, GetScreenWidth() - 210, 40};
    GuiTextBox(url_rect, url_buffer, sizeof(url_buffer), url_edit);

    Rectangle dest_rect = {30, 120, GetScreenWidth() - 320, 40};
    GuiTextBox(dest_rect, dest_dir, sizeof(dest_dir), dest_edit);

    Rectangle browse_rect = {GetScreenWidth() - 280, 120, 100, 40};
    if (GuiButton(browse_rect, "Browse...")) {
      const char *picked =
          tinyfd_selectFolderDialog("Choose Download Folder", dest_dir);
      if (picked != NULL) {
        strncpy(dest_dir, picked, sizeof(dest_dir) - 1);
        dest_dir[sizeof(dest_dir) - 1] = '\0';
      }
    }

    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
      Vector2 mouse = GetMousePosition();
      if (CheckCollisionPointRec(mouse, url_rect)) {
        url_edit = true;
        dest_edit = false;
      } else if (CheckCollisionPointRec(mouse, dest_rect)) {
        dest_edit = true;
        url_edit = false;
      } else if (!CheckCollisionPointRec(mouse, browse_rect)) {
        url_edit = false;
        dest_edit = false;
      }
    }

    Rectangle btn_rect = {GetScreenWidth() - 160, 45, 130, 115};
    if (GuiButton(btn_rect, "DOWNLOAD") ||
        (IsKeyPressed(KEY_ENTER) && strlen(url_buffer) > 0)) {
      if (strlen(url_buffer) > 0 && strlen(dest_dir) > 0) {
        char filename[512];
        char full_path[IPC_MAX_PATH_LEN];
        filename_from_url(url_buffer, filename, sizeof(filename));
        join_path(dest_dir, filename, full_path, sizeof(full_path));

        DM_LOG("Sending add download: url='%s', dest='%s'\n", url_buffer,
               full_path);

        // Send on g_ipc_fd (the main thread's socket)
        uint32_t id =
            ipc_send_add_download(g_ipc_fd, url_buffer, full_path, NULL);

        DM_LOG("Got download ID: %u\n", id);

        if (id > 0) {
          dm_mutex_lock(&g_rows_mutex);
          if (g_row_count < MAX_DISPLAYED) {
            g_rows[g_row_count].id = id;
            g_rows[g_row_count].progress = 0.0f;
            strncpy(g_rows[g_row_count].url, url_buffer,
                    sizeof(g_rows[g_row_count].url) - 1);
            strcpy(g_rows[g_row_count].status, "Queued");
            g_row_count++;
          }
          dm_mutex_unlock(&g_rows_mutex);

          url_buffer[0] = '\0';
          url_edit = false;
          dest_edit = false;
        } else {
          fprintf(stderr, "Failed to add download (ID=0)\n");
        }
      }
    }

    GuiLabel((Rectangle){30, 200, 300, 20}, "ACTIVE DOWNLOADS");
    dm_mutex_lock(&g_rows_mutex);
    for (int i = 0; i < g_row_count; i++) {
      int y_pos = 240 + i * 85; // NEW — taller row spacing to fit buttons
      DrawRectangleRounded(
          (Rectangle){30, y_pos - 10, GetScreenWidth() - 60, 75}, 0.15f, 6,
          (Color){32, 32, 36, 255});
      Rectangle bar_rect = {40, y_pos + 15, GetScreenWidth() - 300, 20};

      char title_label[512];
      snprintf(title_label, sizeof(title_label), "ID: %d | %s", g_rows[i].id,
               g_rows[i].url);
      DrawText(title_label, 40, y_pos - 5, 14, (Color){200, 200, 200, 255});

      char status_label[128];
      snprintf(status_label, sizeof(status_label), "%s - %.0f%%",
               g_rows[i].status, g_rows[i].progress * 100.0f);
      DrawText(status_label, 40, y_pos + 42, 14, (Color){150, 150, 150, 255});

      GuiProgressBar(bar_rect, NULL, NULL, &g_rows[i].progress, 0.0f, 1.0f);

      // NEW — action buttons, right-aligned, chosen per status
      bool is_active = strcmp(g_rows[i].status, "ACTIVE") == 0;
      bool is_queued = strcmp(g_rows[i].status, "QUEUED") == 0;
      bool is_paused = strcmp(g_rows[i].status, "PAUSED") == 0;
      bool is_error = strcmp(g_rows[i].status, "ERROR") == 0;
      bool is_done = strcmp(g_rows[i].status, "DONE") == 0;

      float btn_x = GetScreenWidth() - 245;
      Rectangle primary_btn = {btn_x, y_pos + 5, 100, 30};
      Rectangle cancel_btn = {btn_x + 110, y_pos + 5, 100, 30};

      if ((is_active || is_queued) && GuiButton(primary_btn, "Pause")) {
        ipc_send_pause(g_ipc_fd, g_rows[i].id);
        strncpy(g_rows[i].status, "PAUSED", sizeof(g_rows[i].status) - 1);
      } else if ((is_paused || is_error) && GuiButton(primary_btn, "Resume")) {
        ipc_send_resume(g_ipc_fd, g_rows[i].id);
        strncpy(g_rows[i].status, "QUEUED", sizeof(g_rows[i].status) - 1);
      }

      if (!is_done && GuiButton(cancel_btn, "Cancel")) {
        g_confirm_cancel_id = g_rows[i].id; // opens confirm modal below
      }
    }
    dm_mutex_unlock(&g_rows_mutex);

    // NEW — Cancel confirmation modal. Drawn last so it overlays everything.
    if (g_confirm_cancel_id != 0) {
      int mw = 420, mh = 140;
      int mx = (GetScreenWidth() - mw) / 2;
      int my = (GetScreenHeight() - mh) / 2;
      DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                    (Color){0, 0, 0, 150});
      DrawRectangleRounded(
          (Rectangle){(float)mx, (float)my, (float)mw, (float)mh}, 0.1f, 6,
          (Color){40, 40, 45, 255});
      char msg[128];
      snprintf(msg, sizeof(msg), "Cancel download %u?", g_confirm_cancel_id);
      DrawText(msg, mx + 30, my + 25, 18, (Color){230, 230, 230, 255});
      DrawText("This deletes the partial file.", mx + 30, my + 50, 14,
               (Color){150, 150, 150, 255});

      Rectangle yes_btn = {(float)(mx + 30), (float)(my + 85), 150, 35};
      Rectangle no_btn = {(float)(mx + 230), (float)(my + 85), 150, 35};

      if (GuiButton(yes_btn, "Yes, Cancel")) {
        ipc_send_cancel(g_ipc_fd, g_confirm_cancel_id);
        dm_mutex_lock(&g_rows_mutex);
        for (int i = 0; i < g_row_count; i++) {
          if (g_rows[i].id == g_confirm_cancel_id) {
            strncpy(g_rows[i].status, "ERROR", sizeof(g_rows[i].status) - 1);
            break;
          }
        }
        dm_mutex_unlock(&g_rows_mutex);
        g_confirm_cancel_id = 0;
      } else if (GuiButton(no_btn, "Keep it")) {
        g_confirm_cancel_id = 0;
      }
    }
    EndDrawing();
  }

  CloseWindow();
  ipc_client_disconnect(g_ipc_fd);
  ipc_client_disconnect(g_listener_fd);
  return 0;
}
