#include "../src/engine/worker_pool.h"
#include "../src/platform/file_io.h"
#include "../src/platform/thread.h"
#include <criterion/criterion.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <threads.h>
#include <unistd.h>

typedef struct {
  dm_mutex_t mutex;
  _Atomic bool locked;
  _Atomic bool bad_split;
  _Atomic bool sampling;
  uint64_t start[MAX_WORKERS], end[MAX_WORKERS];
  int count;
} SplitState;

static void split_lock(void *userdata) {
  SplitState *state = userdata;
  dm_mutex_lock(&state->mutex);
  atomic_store(&state->locked, true);
}

static void split_unlock(void *userdata) {
  SplitState *state = userdata;
  atomic_store(&state->locked, false);
  dm_mutex_unlock(&state->mutex);
}

static void record_split(void *userdata, uint64_t victim_start,
                         uint64_t new_start, uint64_t new_end) {
  SplitState *state = userdata;
  bool missing_lock = !atomic_load(&state->locked);
  if (missing_lock) {
    atomic_store(&state->bad_split, true);
    dm_mutex_lock(&state->mutex);
  }
  int victim = -1;
  for (int i = 0; i < state->count; i++)
    if (state->start[i] == victim_start)
      victim = i;
  if (victim < 0 || state->count >= MAX_WORKERS ||
      new_start <= victim_start || new_end > state->end[victim]) {
    atomic_store(&state->bad_split, true);
  } else {
    state->end[victim] = new_start;
    state->start[state->count] = new_start;
    state->end[state->count] = new_end;
    state->count++;
  }
  if (missing_lock)
    dm_mutex_unlock(&state->mutex);
}

static int sample_splits(void *raw) {
  SplitState *state = raw;
  while (atomic_load(&state->sampling)) {
    dm_mutex_lock(&state->mutex);
    for (int i = 0; i < state->count; i++)
      if (state->start[i] >= state->end[i])
        atomic_store(&state->bad_split, true);
    dm_mutex_unlock(&state->mutex);
    dm_thread_sleep_ms(1);
  }
  return 0;
}

static int reserve_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  cr_assert_geq(fd, 0);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  cr_assert_eq(bind(fd, (struct sockaddr *)&address, sizeof(address)), 0);
  socklen_t length = sizeof(address);
  cr_assert_eq(getsockname(fd, (struct sockaddr *)&address, &length), 0);
  int port = ntohs(address.sin_port);
  close(fd);
  return port;
}

static pid_t start_range_server(int port) {
  static const char script[] =
      "import http.server,sys,time\n"
      "SIZE=8*1024*1024\n"
      "class H(http.server.BaseHTTPRequestHandler):\n"
      " def log_message(self,*args): pass\n"
      " def do_GET(self):\n"
      "  value=self.headers.get('Range','')\n"
      "  if not value.startswith('bytes='): self.send_error(400); return\n"
      "  start,end=map(int,value[6:].split('-'))\n"
      "  slow=start==0\n"
      "  self.send_response(206)\n"
      "  self.send_header('Content-Length',str(end-start+1))\n"
      "  self.send_header('Content-Range',f'bytes {start}-{end}/{SIZE}')\n"
      "  self.end_headers()\n"
      "  while start<=end:\n"
      "   count=min(65536,end-start+1)\n"
      "   self.wfile.write(b'A'*count)\n"
      "   start+=count\n"
      "   if slow: time.sleep(0.01)\n"
      "http.server.ThreadingHTTPServer(('127.0.0.1',int(sys.argv[1])),H).serve_forever()\n";
  pid_t child = fork();
  cr_assert_neq(child, -1);
  if (child == 0) {
    char port_text[16];
    snprintf(port_text, sizeof(port_text), "%d", port);
    int null_fd = open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      dup2(null_fd, STDOUT_FILENO);
      dup2(null_fd, STDERR_FILENO);
      close(null_fd);
    }
    execlp("python3", "python3", "-c", script, port_text, (char *)NULL);
    _exit(127);
  }
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((uint16_t)port);
  for (int i = 0; i < 100; i++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
      close(fd);
      return child;
    }
    close(fd);
    dm_thread_sleep_ms(10);
  }
  kill(child, SIGTERM);
  waitpid(child, NULL, 0);
  cr_assert_fail("local range server did not start");
  return -1;
}

Test(worker_pool, single_worker_failure) {
  const char *test_file = "/tmp/test_worker_pool_fail";
  unlink(test_file);
  file_preallocate(test_file, 100);

  Range ranges[1] = {
      {.start = 0, .end = 99, .resume_offset = 0, .whole_file = false}};
  _Atomic uint64_t total_bytes = 0;
  _Atomic bool cancel = false;
  _Atomic bool pause = false;
  _Atomic uint64_t *slots[1] = {NULL};

  // Use a non‑routable address to force failure.
  WorkerPoolResult res =
      worker_pool_run("http://127.0.0.1:1/", ranges, 1, test_file, &total_bytes,
                      &cancel, &pause, slots, 1024, NULL, NULL);
  cr_assert(!res.all_succeeded);
  unlink(test_file);
}

Test(worker_pool, rebalance_splits_remain_locked_and_cover_file) {
  const uint64_t total = 8ULL * 1024ULL * 1024ULL;
  const uint64_t halfway = total / 2;
  char path[128];
  snprintf(path, sizeof(path), "/tmp/cdm-rebalance-%ld.bin", (long)getpid());
  unlink(path);
  cr_assert_eq(file_preallocate(path, total), 0);
  int port = reserve_port();
  pid_t server = start_range_server(port);

  SplitState state = {0};
  cr_assert_eq(dm_mutex_init(&state.mutex), 0);
  state.start[0] = 0;
  state.end[0] = halfway;
  state.start[1] = halfway;
  state.end[1] = total;
  state.count = 2;
  atomic_store(&state.sampling, true);
  thrd_t sampler;
  cr_assert_eq(thrd_create(&sampler, sample_splits, &state), thrd_success);

  Range ranges[2] = {{.start = 0, .end = halfway - 1},
                     {.start = halfway, .end = total - 1}};
  _Atomic uint64_t bytes = 0, first = 0, second = 0;
  _Atomic uint64_t *progress[MAX_WORKERS] = {&first, &second};
  _Atomic bool cancel = false, pause = false;
  RebalancePool *pool = rebalance_pool_create(ranges, 2, progress,
                                             1024ULL * 1024ULL,
                                             record_split, split_lock,
                                             split_unlock, &state);
  cr_assert_not_null(pool);
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/file", port);
  WorkerPoolResult result = worker_pool_run(
      url, ranges, 2, path, &bytes, &cancel, &pause, progress, 0, NULL, pool);
  rebalance_pool_destroy(pool);
  atomic_store(&state.sampling, false);
  thrd_join(sampler, NULL);
  kill(server, SIGTERM);
  waitpid(server, NULL, 0);

  cr_assert(result.all_succeeded);
  cr_assert_gt(state.count, 2);
  cr_assert(!atomic_load(&state.bad_split));
  bool visited[MAX_WORKERS] = {0};
  uint64_t cursor = 0;
  for (int i = 0; i < state.count; i++) {
    int next = -1;
    for (int j = 0; j < state.count; j++) {
      if (!visited[j] && state.start[j] == cursor) {
        next = j;
        break;
      }
    }
    cr_assert_geq(next, 0);
    cr_assert_gt(state.end[next], cursor);
    visited[next] = true;
    cursor = state.end[next];
  }
  cr_assert_eq(cursor, total);
  int fd = open(path, O_RDONLY);
  cr_assert_geq(fd, 0);
  char block[4096];
  for (uint64_t offset = 0; offset < total; offset += sizeof(block)) {
    cr_assert_eq(pread(fd, block, sizeof(block), (off_t)offset),
                 (ssize_t)sizeof(block));
    for (size_t i = 0; i < sizeof(block); i++)
      cr_assert_eq(block[i], 'A');
  }
  close(fd);
  dm_mutex_destroy(&state.mutex);
  unlink(path);
}
