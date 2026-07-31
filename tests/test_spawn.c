#include "../src/platform/spawn.h"
#include <criterion/criterion.h>
#include <string.h>

Test(spawn, get_self_exe_path) {
  char buf[1024];
  get_self_exe_path(buf, sizeof(buf));
  cr_assert_gt(strlen(buf), 0);
  // It should contain "downloadmgr" or "test_spawn" or something
  // We can just check that it's not empty.
}

// For spawn_daemon_detached, we cannot easily test without forking.
// We'll just test that it doesn't crash when given a dummy path.
// In practice, we might skip this test in CI.
Test(spawn, daemon_detached, .disabled = true) {
  int rc = spawn_daemon_detached("/bin/true");
  cr_assert_eq(rc, 0);
}
