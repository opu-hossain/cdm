#include "../src/platform/spawn.h"
#include <criterion/criterion.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

Test(spawn, get_self_exe_path) {
  char buf[1024];
  get_self_exe_path(buf, sizeof(buf));
  cr_assert_gt(strlen(buf), 0);
  // It should contain "cdm" or "test_spawn" or something
  // We can just check that it's not empty.
}

// For spawn_daemon_detached, we cannot easily test without forking.
// We'll just test that it doesn't crash when given a dummy path.
// In practice, we might skip this test in CI.
Test(spawn, daemon_detached, .disabled = true) {
  int rc = spawn_daemon_detached("/bin/true");
  cr_assert_eq(rc, 0);
}

Test(spawn, post_action_command_does_not_use_implicit_shell) {
  char directory[] = "/tmp/cdm-spawn-action-XXXXXX";
  cr_assert_not_null(mkdtemp(directory));
  char path[256];
  int written = snprintf(path, sizeof(path), "%s/with space", directory);
  cr_assert_geq(written, 0);
  cr_assert_lt((size_t)written, sizeof(path));
  char command[512];
  written = snprintf(command, sizeof(command), "/usr/bin/touch '%s'", path);
  cr_assert_geq(written, 0);
  cr_assert_lt((size_t)written, sizeof(command));
  cr_assert_eq(spawn_post_action("command", command), 0);
  for (int i = 0; i < 100 && access(path, F_OK) != 0; ++i)
    usleep(10000);
  cr_assert_eq(access(path, F_OK), 0);
  cr_assert_eq(spawn_post_action("command", "/bin/true 'unclosed"), -1);
  cr_assert_eq(spawn_post_action("command", "/definitely/missing/cdm-tool"),
               -1);
  cr_assert_eq(spawn_post_action("none", "/bin/true"), -1);
  unlink(path);
  rmdir(directory);
}

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>

static void ffmpeg_script(const char *path, const char *script) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0700);
  cr_assert_geq(fd, 0);
  size_t length = strlen(script);
  cr_assert_eq(write(fd, script, length), (ssize_t)length);
  close(fd);
}

Test(spawn, ffmpeg_fixed_argv_cache_and_exit_status) {
  char root[] = "/tmp/cdm-ffmpeg-test-XXXXXX";
  cr_assert_not_null(mkdtemp(root));
  char executable[128], input[128], output[128];
  int n = snprintf(executable, sizeof(executable), "%s/ffmpeg", root);
  cr_assert_gt(n, 0); cr_assert_lt((size_t)n, sizeof(executable));
  n = snprintf(input, sizeof(input), "%s/input ; literal.ts", root);
  cr_assert_gt(n, 0); cr_assert_lt((size_t)n, sizeof(input));
  n = snprintf(output, sizeof(output), "%s/output ; literal.mp4", root);
  cr_assert_gt(n, 0); cr_assert_lt((size_t)n, sizeof(output));
  ffmpeg_script(input, "payload");
  ffmpeg_script(executable, "#!/bin/sh\ninput=\nprevious=\nfor value do\n"
      "  if [ \"$previous\" = -i ]; then input=$value; fi\n"
      "  previous=$value\n  output=$value\ndone\n/bin/cp \"$input\" \"$output\"\n");
  cr_assert_eq(setenv("PATH", root, 1), 0);
  spawn_media_tools_init();
  cr_assert(spawn_ffmpeg_available());
  cr_assert_eq(setenv("PATH", "/nonexistent", 1), 0);
  cr_assert(spawn_ffmpeg_available()); // resolved path is cached
  _Atomic bool cancel = false, pause = false;
  cr_assert_eq(spawn_ffmpeg_remux(input, output, &cancel, &pause, 2), 0);
  int fd = open(output, O_RDONLY); char bytes[8] = {0};
  cr_assert_geq(fd, 0); cr_assert_eq(read(fd, bytes, sizeof(bytes)), 7); close(fd);
  cr_assert_str_eq(bytes, "payload");
  unlink(executable);
  ffmpeg_script(executable, "#!/bin/sh\nexit 7\n");
  cr_assert_eq(spawn_ffmpeg_remux(input, output, &cancel, &pause, 2), 7);
  unlink(executable); unlink(input); unlink(output); rmdir(root);
}

Test(spawn, ffmpeg_timeout_reaps_child_and_pause_prevents_spawn) {
  char root[] = "/tmp/cdm-ffmpeg-timeout-XXXXXX";
  cr_assert_not_null(mkdtemp(root));
  char executable[128];
  int n = snprintf(executable, sizeof(executable), "%s/ffmpeg", root);
  cr_assert_gt(n, 0); cr_assert_lt((size_t)n, sizeof(executable));
  ffmpeg_script(executable, "#!/bin/sh\nexec /bin/sleep 30\n");
  cr_assert_eq(setenv("PATH", root, 1), 0);
  _Atomic bool cancel = false, pause = true;
  cr_assert_eq(spawn_ffmpeg_remux("/tmp/input.ts", "/tmp/output.mp4", &cancel, &pause, 1), -2);
  atomic_store(&pause, false);
  cr_assert_eq(spawn_ffmpeg_remux("/tmp/input.ts", "/tmp/output.mp4", &cancel, &pause, 1), 124);
  int status;
  cr_assert_eq(waitpid(-1, &status, WNOHANG), -1);
  cr_assert_eq(errno, ECHILD);
  unlink(executable); rmdir(root);
}

Test(spawn, ffmpeg_absence_is_cached) {
  cr_assert_eq(setenv("PATH", "/nonexistent", 1), 0);
  spawn_media_tools_init();
  cr_assert(!spawn_ffmpeg_available());
  cr_assert_eq(setenv("PATH", "/usr/bin:/bin", 1), 0);
  cr_assert(!spawn_ffmpeg_available());
}
#endif
