#include "../src/core/queue_manager.h"
#include "../src/engine/engine_runner.h"
#include "../src/persistence/db.h"
#include "../src/platform/curl_client.h"
#include "../src/platform/file_io.h"
#include "../src/utils/config.h"
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
  if (g_server_root[0]) {
    char path[160];
    snprintf(path, sizeof(path), "%s/payload.bin", g_server_root);
    unlink(path);
    rmdir(g_server_root);
  }
  g_server_root[0] = '\0';
}

static void start_unknown_size_server(void) {
  g_server_root[0] = '\0';
  g_server_port = reserve_port();
  g_server_pid = fork();
  cr_assert_neq(g_server_pid, -1);
  if (g_server_pid == 0) {
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
      _exit(1);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)g_server_port);
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listener, 4) != 0)
      _exit(1);
    for (;;) {
      int client = accept(listener, NULL, NULL);
      if (client < 0)
        continue;
      char request[1024] = {0};
      ssize_t count = recv(client, request, sizeof(request) - 1, 0);
      if (count > 0) {
        const char response[] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n";
        send(client, response, sizeof(response) - 1, 0);
        if (strncmp(request, "GET ", 4) == 0) {
          const char body[] = "unknown-size-transfer";
          send(client, body, sizeof(body) - 1, 0);
        }
      }
      close(client);
    }
  }

  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/stream", g_server_port);
  for (int i = 0; i < 100; i++) {
    FileInfo info = {0};
    if (curl_client_head(url, NULL, &info) == 0 && info.total_size == 0)
      return;
    usleep(10000);
  }
  cr_assert_fail("unknown-size HTTP server did not become ready");
}

static void start_validator_server(int mode) {
  g_server_root[0] = '\0';
  g_server_port = reserve_port();
  g_server_pid = fork();
  cr_assert_neq(g_server_pid, -1);
  if (g_server_pid == 0) {
    static const char script[] =
        "import http.server,sys\n"
        "OLD=b'A'*64\n"
        "NEW=b'B'*64\n"
        "MODE=int(sys.argv[2])\n"
        "class H(http.server.BaseHTTPRequestHandler):\n"
        " def log_message(self,*args): pass\n"
        " def do_HEAD(self):\n"
        "  self.send_response(200)\n"
        "  self.send_header('Content-Length','64')\n"
        "  self.send_header('Accept-Ranges','bytes')\n"
        "  self.send_header('ETag','\"new\"' if MODE==2 else '\"old\"')\n"
        "  self.end_headers()\n"
        " def do_GET(self):\n"
        "  value=self.headers.get('Range','')\n"
        "  validator=self.headers.get('If-Range','')\n"
        "  if not value.startswith('bytes='):\n"
        "   self.send_error(400); return\n"
        "  start,end=map(int,value[6:].split('-'))\n"
        "  if MODE==2 and (validator or start>0):\n"
        "   self.send_error(400); return\n"
        "  if start>0 and validator!='\"old\"':\n"
        "   self.send_error(400); return\n"
        "  if MODE==1 and validator:\n"
        "   self.send_response(200); body=NEW\n"
        "  else:\n"
        "   self.send_response(206)\n"
        "   self.send_header('Content-Range',f'bytes {start}-{end}/64')\n"
        "   body=(NEW if MODE else OLD)[start:end+1]\n"
        "  self.send_header('Content-Length',str(len(body)))\n"
        "  self.end_headers()\n"
        "  try: self.wfile.write(body)\n"
        "  except BrokenPipeError: pass\n"
        "http.server.ThreadingHTTPServer(('127.0.0.1',int(sys.argv[1])),H).serve_forever()\n";
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", g_server_port);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    char mode_text[8];
    snprintf(mode_text, sizeof(mode_text), "%d", mode);
    execlp("python3", "python3", "-c", script, port_text,
           mode_text, (char *)NULL);
    _exit(127);
  }
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/file", g_server_port);
  for (int i = 0; i < 100; i++) {
    FileInfo info = {0};
    if (curl_client_head(url, NULL, &info) == 0 && info.total_size == 64)
      return;
    usleep(10000);
  }
  cr_assert_fail("validator HTTP server did not become ready");
}

static void start_auth_server(int second_port) {
  g_server_root[0] = '\0';
  g_server_port = reserve_port();
  while (g_server_port == second_port)
    g_server_port = reserve_port();
  g_server_pid = fork();
  cr_assert_neq(g_server_pid, -1);
  if (g_server_pid == 0) {
    static const char script[] =
        "import base64,http.server,sys,threading\n"
        "FIRST=int(sys.argv[1]); SECOND=int(sys.argv[2])\n"
        "AUTH='Basic '+base64.b64encode(b'auth-user:auth-pass').decode()\n"
        "BODY=b'auth-payload'\n"
        "class First(http.server.BaseHTTPRequestHandler):\n"
        " def log_message(self,*args): pass\n"
        " def reply(self,body):\n"
        "  if self.headers.get('Authorization')!=AUTH:\n"
        "   self.send_response(401)\n"
        "   self.send_header('WWW-Authenticate','Basic realm=\"cdm-test\"')\n"
        "   self.end_headers(); return\n"
        "  if self.path=='/redirect':\n"
        "   self.send_response(302)\n"
        "   self.send_header('Location',f'http://127.0.0.1:{SECOND}/file')\n"
        "   self.end_headers(); return\n"
        "  if self.path not in ('/file','/ua'): self.send_error(404); return\n"
        "  if self.path=='/ua' and self.headers.get('User-Agent')!='cdm-test-agent/2':\n"
        "   self.send_error(400); return\n"
        "  self.send_response(200)\n"
        "  self.send_header('Content-Length',str(len(BODY)))\n"
        "  self.end_headers()\n"
        "  if body: self.wfile.write(BODY)\n"
        " def do_HEAD(self): self.reply(False)\n"
        " def do_GET(self): self.reply(True)\n"
        "class Second(http.server.BaseHTTPRequestHandler):\n"
        " def log_message(self,*args): pass\n"
        " def reply(self,body):\n"
        "  if self.headers.get('Authorization'):\n"
        "   self.send_error(403); return\n"
        "  self.send_response(200)\n"
        "  self.send_header('Content-Length','3')\n"
        "  self.end_headers()\n"
        "  if body: self.wfile.write(b'new')\n"
        " def do_HEAD(self): self.reply(False)\n"
        " def do_GET(self): self.reply(True)\n"
        "threading.Thread(target=lambda: http.server.ThreadingHTTPServer("
        "('127.0.0.1',SECOND),Second).serve_forever(),daemon=True).start()\n"
        "http.server.ThreadingHTTPServer(('127.0.0.1',FIRST),First).serve_forever()\n";
    char first[16], second[16];
    snprintf(first, sizeof(first), "%d", g_server_port);
    snprintf(second, sizeof(second), "%d", second_port);
    execlp("python3", "python3", "-c", script, first, second,
           (char *)NULL);
    _exit(127);
  }
  for (int i = 0; i < 100; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert_neq(fd, -1);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons((uint16_t)g_server_port)};
    int ready = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    if (ready)
      return;
    usleep(10000);
  }
  cr_assert_fail("auth HTTP server did not become ready");
}

static void check_validator_resume(int mode) {
  start_validator_server(mode);
  snprintf(g_download_path, sizeof(g_download_path),
           "/tmp/cdm-validator-resume-%ld.bin", (long)getpid());
  FILE *file = fopen(g_download_path, "wb");
  cr_assert_not_null(file);
  char first_half[32];
  memset(first_half, 'A', sizeof(first_half));
  cr_assert_eq(fwrite(first_half, 1, sizeof(first_half), file),
               sizeof(first_half));
  fclose(file);

  Download d = {.id = 41, .total_size = 64, .chunk_count = 1};
  snprintf(d.url, sizeof(d.url), "http://127.0.0.1:%d/file", g_server_port);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);
  strcpy(d.etag, "\"old\"");
  d.chunks[0] = (DownloadChunk){0, 63, 32};
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, NULL), 0);
  cr_assert_eq(db_update_total_size(d.id, 64), 0);
  cr_assert_eq(db_update_validators(d.id, d.etag, ""), 0);
  cr_assert_eq(db_insert_chunk(d.id, 0, 63), 0);
  cr_assert_eq(db_update_chunk_progress(d.id, 0, 32), 0);

  cr_assert_eq(engine_run_download(&d), 0);
  cr_assert_eq(file_get_size(g_download_path), 64);
  file = fopen(g_download_path, "rb");
  cr_assert_not_null(file);
  char actual[64];
  cr_assert_eq(fread(actual, 1, sizeof(actual), file), sizeof(actual));
  fclose(file);
  for (size_t i = 0; i < sizeof(actual); i++)
    cr_assert_eq(actual[i], mode ? 'B' : 'A');
  stop_server();
}

Test(engine_http_integration, matching_if_range_resumes) {
  check_validator_resume(false);
}

Test(engine_http_integration, stale_if_range_restarts_from_zero) {
  check_validator_resume(1);
}

Test(engine_http_integration, changed_head_validator_restarts_before_get) {
  check_validator_resume(2);
}

static void setup_engine_http(void) {
  setenv("DOWNLOADMGR_ROOT", "/tmp", 1);
  db_init(":memory:");
}

static void teardown_engine_http(void) {
  config_init("/tmp/cdm-engine-no-config.toml");
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

Test(engine_http_integration, basic_auth_probe_worker_and_redirect_boundary) {
  int second_port = reserve_port();
  start_auth_server(second_port);
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/file", g_server_port);
  FileInfo info = {0};
  cr_assert_eq(curl_client_head(url, NULL, &info), -1);
  RequestContext context = {.auth_user = "auth-user",
                            .auth_password = "auth-pass"};
  cr_assert_eq(curl_client_head(url, &context, &info), 0);
  cr_assert_eq(info.total_size, 12);

  snprintf(g_download_path, sizeof(g_download_path),
           "/tmp/cdm-auth-download-%ld.bin", (long)getpid());
  Download d = {.id = 72};
  snprintf(d.url, sizeof(d.url), "%s", url);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);
  RequestOptions options = {0};
  snprintf(options.auth_user, sizeof(options.auth_user), "auth-user");
  snprintf(options.auth_password, sizeof(options.auth_password), "auth-pass");
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, &options), 0);
  d.request = &options;
  cr_assert_eq(engine_run_download(&d), 0);
  cr_assert_eq(file_get_size(g_download_path), 12);
  FILE *download = fopen(g_download_path, "rb");
  cr_assert_not_null(download);
  char actual[12];
  cr_assert_eq(fread(actual, 1, sizeof(actual), download), sizeof(actual));
  fclose(download);
  cr_assert_eq(memcmp(actual, "auth-payload", sizeof(actual)), 0);

  snprintf(url, sizeof(url), "http://127.0.0.1:%d/redirect", g_server_port);
  cr_assert_eq(curl_client_head(url, &context, &info), 0);
  cr_assert_eq(info.total_size, 3);
  stop_server();
}

Test(engine_http_integration, configured_user_agent_reaches_probe_and_worker) {
  char path[160];
  snprintf(path, sizeof(path), "/tmp/cdm-agent-%ld.toml", (long)getpid());
  FILE *config = fopen(path, "w");
  cr_assert_not_null(config);
  fputs("[downloads]\nuser_agent = \"cdm-test-agent/2\"\n", config);
  cr_assert_eq(fclose(config), 0);
  config_init(path);
  unlink(path);

  int second_port = reserve_port();
  start_auth_server(second_port);
  snprintf(g_download_path, sizeof(g_download_path),
           "/tmp/cdm-agent-download-%ld.bin", (long)getpid());
  Download d = {.id = 73};
  snprintf(d.url, sizeof(d.url), "http://127.0.0.1:%d/ua", g_server_port);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);
  RequestOptions options = {0};
  snprintf(options.auth_user, sizeof(options.auth_user), "auth-user");
  snprintf(options.auth_password, sizeof(options.auth_password), "auth-pass");
  d.request = &options;
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, &options), 0);
  cr_assert_eq(engine_run_download(&d), 0);
  cr_assert_eq(file_get_size(g_download_path), 12);
  stop_server();
  config_init("/tmp/cdm-engine-no-config.toml");
}

Test(engine_http_integration, configured_proxy_handles_probe_and_download) {
  const int proxy_port = reserve_port();
  g_server_pid = fork();
  cr_assert_neq(g_server_pid, -1);
  if (g_server_pid == 0) {
    static const char script[] =
        "import base64,http.server,sys\n"
        "AUTH='Basic '+base64.b64encode(b'proxy-user:proxy-pass').decode()\n"
        "BODY=b'proxy-payload'\n"
        "class H(http.server.BaseHTTPRequestHandler):\n"
        " def log_message(self,*args): pass\n"
        " def reply(self,body):\n"
        "  if self.headers.get('Proxy-Authorization')!=AUTH:\n"
        "   self.send_response(407); self.end_headers(); return\n"
        "  if self.path!='http://127.0.0.1:1/file.bin':\n"
        "   self.send_error(404); return\n"
        "  self.send_response(200)\n"
        "  self.send_header('Content-Length',str(len(BODY)))\n"
        "  self.end_headers()\n"
        "  if body: self.wfile.write(BODY)\n"
        " def do_HEAD(self): self.reply(False)\n"
        " def do_GET(self): self.reply(True)\n"
        "http.server.ThreadingHTTPServer(('127.0.0.1',int(sys.argv[1])),H).serve_forever()\n";
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", proxy_port);
    execlp("python3", "python3", "-c", script, port_text, (char *)NULL);
    _exit(127);
  }

  char config_path[160];
  snprintf(config_path, sizeof(config_path), "/tmp/cdm-proxy-%ld.toml",
           (long)getpid());
  FILE *config = fopen(config_path, "w");
  cr_assert_not_null(config);
  cr_assert(fprintf(config,
                    "[proxy]\nmode = 1\nurl = \"http://127.0.0.1:%d\"\n"
                    "username = \"proxy-user\"\npassword = \"proxy-pass\"\n",
                    proxy_port) > 0);
  cr_assert_eq(fclose(config), 0);
  config_init(config_path);
  unlink(config_path);

  for (int i = 0; i < 100; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    cr_assert_neq(fd, -1);
    struct sockaddr_in addr = {.sin_family = AF_INET,
                               .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
                               .sin_port = htons((uint16_t)proxy_port)};
    int ready = connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    close(fd);
    if (ready)
      break;
    usleep(10000);
    if (i == 99)
      cr_assert_fail("local proxy did not start");
  }

  const char *url = "http://127.0.0.1:1/file.bin";
  FileInfo info = {0};
  cr_assert_eq(curl_client_head(url, NULL, &info), 0);
  cr_assert_eq(info.total_size, 13);
  snprintf(g_download_path, sizeof(g_download_path),
           "/tmp/cdm-proxy-download-%ld.bin", (long)getpid());
  Download d = {.id = 71};
  snprintf(d.url, sizeof(d.url), "%s", url);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, NULL), 0);
  cr_assert_eq(engine_run_download(&d), 0);
  cr_assert_eq(file_get_size(g_download_path), 13);
  FILE *download = fopen(g_download_path, "rb");
  cr_assert_not_null(download);
  char actual[13];
  cr_assert_eq(fread(actual, 1, sizeof(actual), download), sizeof(actual));
  fclose(download);
  cr_assert_eq(memcmp(actual, "proxy-payload", sizeof(actual)), 0);
  stop_server();
}

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

Test(engine_http_integration, unknown_size_download_succeeds) {
  static const char payload[] = "unknown-size-transfer";
  start_unknown_size_server();
  snprintf(g_download_path, sizeof(g_download_path),
           "/tmp/cdm-engine-http-unknown-%ld.bin", (long)getpid());
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/stream", g_server_port);

  Download d = {0};
  d.id = 10;
  snprintf(d.url, sizeof(d.url), "%s", url);
  snprintf(d.dest_path, sizeof(d.dest_path), "%s", g_download_path);
  cr_assert_eq(db_insert_download(d.id, d.url, d.dest_path, NULL), 0);
  cr_assert_eq(engine_run_download(&d), 0);
  cr_assert_eq(file_get_size(g_download_path), sizeof(payload) - 1);

  FILE *fp = fopen(g_download_path, "rb");
  cr_assert_not_null(fp);
  char actual[sizeof(payload)] = {0};
  cr_assert_eq(fread(actual, 1, sizeof(payload) - 1, fp),
               sizeof(payload) - 1);
  fclose(fp);
  cr_assert_eq(memcmp(actual, payload, sizeof(payload) - 1), 0);
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
