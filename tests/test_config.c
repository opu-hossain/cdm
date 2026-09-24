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
