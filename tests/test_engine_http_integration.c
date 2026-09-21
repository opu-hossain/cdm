#include "../src/core/queue_manager.h"
#include "../src/engine/engine_runner.h"
#include "../src/persistence/db.h"
#include "../src/platform/curl_client.h"
#include "../src/platform/file_io.h"
#include <criterion/criterion.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t g_server_pid = -1;
static char g_server_root[128] = {0};
static char g_download_path[160] = {0};
static int g_server_port = 0;

static int reserve_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  cr_assert_neq(fd, -1);
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  cr_assert_eq(bind(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  cr_assert_eq(getsockname(fd, (struct sockaddr *)&addr, &len), 0);
  int port = ntohs(addr.sin_port);
  close(fd);
  return port;
}

static void start_server(const char *payload, size_t payload_len) {
  snprintf(g_server_root, sizeof(g_server_root), "/tmp/cdm-engine-http-%ld",
           (long)getpid());
  cr_assert_eq(mkdir(g_server_root, 0700), 0);

  char payload_path[160];
  snprintf(payload_path, sizeof(payload_path), "%s/payload.bin", g_server_root);
  FILE *fp = fopen(payload_path, "wb");
  cr_assert_not_null(fp);
  cr_assert_eq(fwrite(payload, 1, payload_len, fp), payload_len);
  fclose(fp);

  g_server_port = reserve_port();
  g_server_pid = fork();
  cr_assert_neq(g_server_pid, -1);
  if (g_server_pid == 0) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", g_server_port);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    execlp("python3", "python3", "-m", "http.server", "--bind",
           "127.0.0.1", "--directory", g_server_root, port_text,
           (char *)NULL);
    _exit(127);
  }

  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/payload.bin", g_server_port);
  for (int i = 0; i < 100; i++) {
    FileInfo info = {0};
    RequestContext ctx = {0};
    if (curl_client_head(url, &ctx, &info) == 0 && info.total_size == payload_len)
      return;
    usleep(10000);
  }
  cr_assert_fail("local HTTP server did not become ready");
}

static void stop_server(void) {
  if (g_server_pid > 0) {
    kill(g_server_pid, SIGTERM);
    waitpid(g_server_pid, NULL, 0);
    g_server_pid = -1;
  }
  char path[160];
  snprintf(path, sizeof(path), "%s/payload.bin", g_server_root);
  unlink(path);
  rmdir(g_server_root);
  g_server_root[0] = '\0';
}

static void setup_engine_http(void) {
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  db_init(":memory:");
}

static void teardown_engine_http(void) {
  db_close();
  if (g_download_path[0] != '\0') {
    unlink(g_download_path);
    g_download_path[0] = '\0';
  }
  if (g_server_pid > 0)
    stop_server();
}

TestSuite(engine_http_integration, .init = setup_engine_http,
          .fini = teardown_engine_http);

Test(engine_http_integration, real_download_succeeds) {
  static const char payload[] =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  size_t payload_len = strlen(payload);
  start_server(payload, payload_len);

  snprintf(g_download_path, sizeof(g_download_path), "/tmp/cdm-engine-http-%ld.bin",
           (long)getpid());
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/payload.bin", g_server_port);

  Download d = {0};
  d.id = 1;
  snprintf(d.url, sizeof(d.url), "%s", url);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);
  RequestOptions opts = {0};
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, &opts), 0);

  int rc = engine_run_download(&d);
  cr_assert_eq(rc, 0);

  uint64_t size = file_get_size(g_download_path);
  cr_assert_eq(size, payload_len);

  FILE *fp = fopen(g_download_path, "rb");
  cr_assert_not_null(fp);
  char buf[256];
  size_t read_len = fread(buf, 1, sizeof(buf), fp);
  fclose(fp);
  cr_assert_eq(read_len, payload_len);
  cr_assert_eq(memcmp(payload, buf, payload_len), 0);

  stop_server();
}

Test(engine_http_integration, real_range_download_succeeds) {
  size_t payload_len = 4 * 1024 * 1024U;
  char *payload = malloc(payload_len);
  cr_assert_not_null(payload);
  for (size_t i = 0; i < payload_len; i++)
    payload[i] = (char)('A' + (i % 26));

  start_server(payload, payload_len);

  snprintf(g_download_path, sizeof(g_download_path), "/tmp/cdm-engine-http-range-%ld.bin",
           (long)getpid());
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/payload.bin", g_server_port);

  Download d = {0};
  d.id = 2;
  snprintf(d.url, sizeof(d.url), "%s", url);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);
  RequestOptions opts = {0};
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, &opts), 0);

  int rc = engine_run_download(&d);
  cr_assert_eq(rc, 0);

  uint64_t size = file_get_size(g_download_path);
  cr_assert_eq(size, payload_len);

  FILE *fp = fopen(g_download_path, "rb");
  cr_assert_not_null(fp);
  char *read_buf = malloc(payload_len);
  cr_assert_not_null(read_buf);
  size_t read_len = fread(read_buf, 1, payload_len, fp);
  fclose(fp);
  cr_assert_eq(read_len, payload_len);
  cr_assert_eq(memcmp(payload, read_buf, payload_len), 0);

  free(read_buf);
  free(payload);
  stop_server();
}

Test(engine_http_integration, real_download_resumes_from_partial_file) {
  static const char payload[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  size_t payload_len = strlen(payload);
  size_t cutoff = payload_len / 2;

  start_server(payload, payload_len);

  snprintf(g_download_path, sizeof(g_download_path), "/tmp/cdm-engine-http-resume-%ld.bin",
           (long)getpid());
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/payload.bin", g_server_port);

  FILE *fp = fopen(g_download_path, "wb");
  cr_assert_not_null(fp);
  cr_assert_eq(fwrite(payload, 1, cutoff, fp), cutoff);
  fclose(fp);

  Download d = {0};
  d.id = 3;
  d.total_size = payload_len;
  d.chunk_count = 1;
  d.chunks[0] = (DownloadChunk){.range_start = 0,
                                .range_end = payload_len - 1,
                                .bytes_done = cutoff};
  d.chunks[0].bytes_done = cutoff;
  snprintf(d.url, sizeof(d.url), "%s", url);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);

  RequestOptions opts = {0};
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, &opts), 0);
  cr_assert_eq(db_insert_chunk(d.id, d.chunks[0].range_start, d.chunks[0].range_end),
               0);
  cr_assert_eq(db_update_chunk_progress(d.id, d.chunks[0].range_start, cutoff), 0);

  int rc = engine_run_download(&d);
  cr_assert_eq(rc, 0);

  uint64_t size = file_get_size(g_download_path);
  cr_assert_eq(size, payload_len);

  fp = fopen(g_download_path, "rb");
  cr_assert_not_null(fp);
  char *buf = malloc(payload_len + 1);
  cr_assert_not_null(buf);
  size_t read_len = fread(buf, 1, payload_len, fp);
  fclose(fp);
  cr_assert_eq(read_len, payload_len);
  cr_assert_eq(memcmp(payload, buf, payload_len), 0);
  free(buf);

  stop_server();
}
