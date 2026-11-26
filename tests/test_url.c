#include "../src/utils/url.h"
#include <criterion/criterion.h>
#include <string.h>

Test(url, normalizes_scheme_host_port_and_fragment) {
  char out[256];
  cr_assert(url_normalize("HTTP://EXAMPLE.COM:80/File?Key=VaLue#part", out,
                          sizeof(out)));
  cr_assert_str_eq(out, "http://example.com/File?Key=VaLue");
  cr_assert(url_normalize("http://Example.COM:080/file", out,
                          sizeof(out)));
  cr_assert_str_eq(out, "http://example.com/file");
  cr_assert(url_normalize("HTTPS://Example.COM:443/Case#fragment", out,
                          sizeof(out)));
  cr_assert_str_eq(out, "https://example.com/Case");
  cr_assert(url_normalize("https://[2001:DB8::1]:443/a?q=UP", out,
                          sizeof(out)));
  cr_assert_str_eq(out, "https://[2001:db8::1]/a?q=UP");
  cr_assert(url_normalize("http://example.com:8080/a#f", out,
                          sizeof(out)));
  cr_assert_str_eq(out, "http://example.com:8080/a");
}

Test(url, preserves_path_query_and_rejects_invalid_input) {
  char out[64];
  cr_assert(url_normalize("https://Example.com/A%2fb?X=1#one", out,
                          sizeof(out)));
  cr_assert_str_eq(out, "https://example.com/A%2fb?X=1");
  cr_assert_not(url_normalize("https:///missing-host", out, sizeof(out)));
  cr_assert_not(url_normalize("ftp://example.com/file", out, sizeof(out)));
  cr_assert_not(url_normalize("http://example.com:invalid/a", out,
                              sizeof(out)));
  cr_assert_not(url_normalize("http://example.com/a", out, 4));
}
