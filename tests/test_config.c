#include "../src/utils/config.h"
#include <criterion/criterion.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void isolated_home(char out[64]) {
  strcpy(out, "/tmp/cdm-config-XXXXXX");
  cr_assert_not_null(mkdtemp(out));
  cr_assert_eq(setenv("HOME", out, 1), 0);
  config_init(NULL);
}

static void remove_home(const char *home) {
  char path[256];
  snprintf(path, sizeof(path), "%s/.local/share/cdm/config.toml", home);
  unlink(path);
  snprintf(path, sizeof(path), "%s/.local/share/cdm", home);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/.local/share", home);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/.local", home);
  rmdir(path);
  snprintf(path, sizeof(path), "%s/Downloads", home);
  rmdir(path);
  rmdir(home);
}

Test(config, proxy_modes_round_trip_without_losing_credentials) {
  char home[64];
  isolated_home(home);
  struct {
    ProxyMode mode;
    const char *url;
  } cases[] = {{PROXY_NONE, ""},
               {PROXY_HTTP, "http://127.0.0.1:3128"},
               {PROXY_SOCKS5, "socks5h://127.0.0.1:1080"}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    DownloadManagerConfig config;
    config_get(&config);
    config.proxy_mode = cases[i].mode;
    snprintf(config.proxy_url, sizeof(config.proxy_url), "%s", cases[i].url);
    snprintf(config.proxy_username, sizeof(config.proxy_username),
             "example-user");
    snprintf(config.proxy_password, sizeof(config.proxy_password),
             "example\"value\\end");
    cr_assert(config_save(&config));
    config_init(NULL);
    DownloadManagerConfig loaded;
    config_get(&loaded);
    cr_assert_eq(loaded.proxy_mode, cases[i].mode);
    cr_assert_str_eq(loaded.proxy_url, cases[i].url);
    cr_assert_str_eq(loaded.proxy_username, "example-user");
    cr_assert_str_eq(loaded.proxy_password, "example\"value\\end");
  }
  remove_home(home);
}

Test(config, post_actions_default_off_and_round_trip) {
  char home[64];
  isolated_home(home);
  cr_assert_not(config_post_action_enabled("shutdown"));
  cr_assert_not(config_post_action_enabled("sleep"));
  cr_assert_not(config_post_action_enabled("command"));
  cr_assert_not(config_post_action_enabled("unknown"));
  DownloadManagerConfig config;
  config_get(&config);
  config.allow_command = true;
  cr_assert(config_save(&config));
  config_init(NULL);
  cr_assert(config_post_action_enabled("command"));
  cr_assert_not(config_post_action_enabled("shutdown"));
  cr_assert_not(config_post_action_enabled("sleep"));
  remove_home(home);
}

Test(config, clipboard_monitor_defaults_off_and_round_trips) {
  char home[64];
  isolated_home(home);
  DownloadManagerConfig config;
  config_get(&config);
  cr_assert_not(config.clipboard_monitor);
  config.clipboard_monitor = true;
  cr_assert(config_save(&config));
  config_init(NULL);
  config_get(&config);
  cr_assert(config.clipboard_monitor);
  remove_home(home);
}

Test(config, invalid_mode_and_missing_proxy_host_fall_back_to_none) {
  char home[64];
  isolated_home(home);
  char path[256];
  snprintf(path, sizeof(path), "%s/proxy.toml", home);
  FILE *fp = fopen(path, "w");
  cr_assert_not_null(fp);
  fputs("[proxy]\nmode = 99\nurl = \"http://127.0.0.1:3128\"\n", fp);
  fclose(fp);
  config_init(path);
  DownloadManagerConfig loaded;
  config_get(&loaded);
  cr_assert_eq(loaded.proxy_mode, PROXY_NONE);

  fp = fopen(path, "w");
  cr_assert_not_null(fp);
  fputs("[proxy]\nmode = 1\nurl = \"http://\"\n", fp);
  fclose(fp);
  config_init(path);
  config_get(&loaded);
  cr_assert_eq(loaded.proxy_mode, PROXY_NONE);

  fp = fopen(path, "w");
  cr_assert_not_null(fp);
  fputs("[proxy]\nmode = 1\nurl = \"\"\n", fp);
  fclose(fp);
  config_init(path);
  config_get(&loaded);
  cr_assert_eq(loaded.proxy_mode, PROXY_NONE);

  fp = fopen(path, "w");
  cr_assert_not_null(fp);
  fputs("[proxy]\nmode = 1\nurl = \"127.0.0.1:3128\"\n", fp);
  fclose(fp);
  config_init(path);
  config_get(&loaded);
  cr_assert_eq(loaded.proxy_mode, PROXY_NONE);
  unlink(path);
  remove_home(home);
}

Test(config, network_controls_round_trip_and_clamp_on_load) {
  char home[64];
  isolated_home(home);
  DownloadManagerConfig config;
  config_get(&config);
  cr_assert_eq(config.max_connections_per_download, 8);
  cr_assert_str_eq(config.user_agent, "cdm/0.1");
  cr_assert_eq(config.connect_timeout_sec, 10);
  cr_assert_eq(config.transfer_timeout_sec, 30);
  config.max_connections_per_download = 12;
  snprintf(config.user_agent, sizeof(config.user_agent), "cdm-test/1.0");
  config.connect_timeout_sec = 45;
  config.transfer_timeout_sec = 240;
  cr_assert(config_save(&config));
  config_init(NULL);
  config_get(&config);
  cr_assert_eq(config.max_connections_per_download, 12);
  cr_assert_str_eq(config.user_agent, "cdm-test/1.0");
  cr_assert_eq(config.connect_timeout_sec, 45);
  cr_assert_eq(config.transfer_timeout_sec, 240);

  char path[256];
  snprintf(path, sizeof(path), "%s/network.toml", home);
  FILE *fp = fopen(path, "w");
  cr_assert_not_null(fp);
  fputs("[downloads]\nmax_connections_per_download = -3\n"
        "user_agent = \"\"\n[timeouts]\nconnect_sec = 0\n"
        "transfer_sec = 99999\n", fp);
  cr_assert_eq(fclose(fp), 0);
  config_init(path);
  config_get(&config);
  cr_assert_eq(config.max_connections_per_download, 1);
  cr_assert_str_eq(config.user_agent, "cdm/0.1");
  cr_assert_eq(config.connect_timeout_sec, 1);
  cr_assert_eq(config.transfer_timeout_sec, 3600);

  fp = fopen(path, "w");
  cr_assert_not_null(fp);
  fputs("[downloads]\nmax_connections_per_download = 99\n"
        "[timeouts]\nconnect_sec = 999\ntransfer_sec = -2\n", fp);
  cr_assert_eq(fclose(fp), 0);
  config_init(path);
  config_get(&config);
  cr_assert_eq(config.max_connections_per_download, 16);
  cr_assert_eq(config.connect_timeout_sec, 600);
  cr_assert_eq(config.transfer_timeout_sec, 1);
  unlink(path);
  remove_home(home);
}
