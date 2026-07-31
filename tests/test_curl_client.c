#include "../src/platform/curl_client.h"
#include <criterion/criterion.h>
#include <stddef.h>
#include <string.h>

// Copy of the internal header callback to test its logic directly.
static size_t head_header_callback_copy(void *data, size_t size, size_t nmemb,
                                        void *userdata) {
  FileInfo *info = (FileInfo *)userdata;
  size_t total = size * nmemb;
  if (total >= 19 &&
      strncasecmp((char *)data, "Accept-Ranges: bytes", 19) == 0) {
    info->supports_ranges = true;
  }
  return total;
}

Test(curl_client, header_callback_detects_ranges) {
  FileInfo info = {0};
  char header[] = "Accept-Ranges: bytes\r\n";
  size_t ret = head_header_callback_copy(header, 1, strlen(header), &info);
  cr_assert_eq(ret, strlen(header));
  cr_assert(info.supports_ranges);
}

Test(curl_client, header_callback_ignores_other_headers) {
  FileInfo info = {0};
  char header[] = "Content-Type: text/html\r\n";
  size_t ret = head_header_callback_copy(header, 1, strlen(header), &info);
  cr_assert_eq(ret, strlen(header));
  cr_assert(!info.supports_ranges);
}

Test(curl_client, head_failure_on_invalid_url) {
  FileInfo info;
  int rc = curl_client_head("http://this-domain-does-not-exist-1234567890.com/",
                            &info);
  cr_assert_eq(rc, -1);
  cr_assert_eq(info.total_size, 0);
}
