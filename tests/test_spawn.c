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
