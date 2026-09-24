#include "../src/platform/curl_client.h"
#include "../src/platform/thread.h"
#include <criterion/criterion.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t server_pid = -1;
static char server_root[128];
static int server_port;

static int reserve_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  cr_assert_neq(fd, -1);

  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  cr_assert_eq(bind(fd, (struct sockaddr *)&address, sizeof(address)), 0);

  socklen_t length = sizeof(address);
  cr_assert_eq(getsockname(fd, (struct sockaddr *)&address, &length), 0);
  int port = ntohs(address.sin_port);
  close(fd);
  return port;
}

static void setup_server(void) {
  snprintf(server_root, sizeof(server_root), "/tmp/cdm-http-%ld",
           (long)getpid());
  cr_assert_eq(mkdir(server_root, 0700), 0);
  char file_path[160];
  snprintf(file_path, sizeof(file_path), "%s/file.bin", server_root);
  FILE *file = fopen(file_path, "wb");
  cr_assert_not_null(file);
  cr_assert_eq(fwrite("test payload", 1, 12, file), 12);
  fclose(file);

  server_port = reserve_port();
  server_pid = fork();
  cr_assert_neq(server_pid, -1);
  if (server_pid == 0) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", server_port);
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
    static const char script[] =
        "import http.server,sys\n"
        "class H(http.server.BaseHTTPRequestHandler):\n"
        " def log_message(self,*args): pass\n"
        " def do_HEAD(self):\n"
        "  if self.path in ('/head-405','/range-ignored'):\n"
        "   self.send_error(405); return\n"
        "  if self.path!='/file.bin': self.send_error(404); return\n"
        "  self.send_response(200)\n"
        "  self.send_header('Content-Length','12')\n"
        "  self.end_headers()\n"
        " def do_GET(self):\n"
        "  if self.path not in ('/head-405','/range-ignored'):\n"
        "   self.send_error(404); return\n"
        "  if self.path=='/head-405' and self.headers.get('Range')=='bytes=0-0':\n"
        "   self.send_response(206)\n"
        "   self.send_header('Content-Range','bytes 0-0/12')\n"
        "   self.send_header('Accept-Ranges','bytes')\n"
        "   body=b't'\n"
        "  else:\n"
        "   self.send_response(200)\n"
        "   body=b'test payload'\n"
        "  self.send_header('Content-Length',str(len(body)))\n"
        "  self.end_headers()\n"
        "  self.wfile.write(body)\n"
        "http.server.ThreadingHTTPServer(('127.0.0.1',int(sys.argv[1])),H).serve_forever()\n";
    execlp("python3", "python3", "-c", script, port_text, (char *)NULL);
    _exit(127);
  }

  char url[128];
  FileInfo info = {0};
  RequestContext context = {0};
  for (int attempt = 0; attempt < 50; attempt++) {
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/file.bin", server_port);
    if (curl_client_head(url, &context, &info) == 0)
      return;
    dm_thread_sleep_ms(20);
  }
  cr_assert_fail("local HTTP server did not start");
}

static void teardown_server(void) {
  if (server_pid > 0) {
    kill(server_pid, SIGTERM);
    waitpid(server_pid, NULL, 0);
    server_pid = -1;
  }
  char file_path[160];
  snprintf(file_path, sizeof(file_path), "%s/file.bin", server_root);
  unlink(file_path);
  rmdir(server_root);
}

TestSuite(curl_http, .init = setup_server, .fini = teardown_server);

Test(curl_http, head_reads_local_file) {
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/file.bin", server_port);
  FileInfo info = {0};
  RequestContext context = {0};

  cr_assert_eq(curl_client_head(url, &context, &info), 0);
  cr_assert_eq(info.total_size, 12);
}

Test(curl_http, head_rejects_http_errors) {
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/missing.bin", server_port);
  FileInfo info = {0};
  RequestContext context = {0};

  cr_assert_eq(curl_client_head(url, &context, &info), -1);
}

Test(curl_http, head_405_falls_back_to_get_range) {
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/head-405", server_port);
  FileInfo info = {0};

  cr_assert_eq(curl_client_head(url, NULL, &info), 0);
  cr_assert_eq(info.total_size, 12);
  cr_assert(info.supports_ranges);
}

Test(curl_http, ignored_range_uses_full_get_length) {
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/range-ignored", server_port);
  FileInfo info = {0};

  cr_assert_eq(curl_client_head(url, NULL, &info), 0);
  cr_assert_eq(info.total_size, 12);
  cr_assert(!info.supports_ranges);
}
