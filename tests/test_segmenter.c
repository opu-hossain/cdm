#include "../src/engine/segmenter.h"
#include <criterion/criterion.h>

Test(segmenter_plan, divides_evenly) {
  Range ranges[8];
  int n = segmenter_plan(1000, 4, ranges);
  cr_assert_eq(n, 4);
  cr_assert_eq(ranges[0].start, 0);
  cr_assert_eq(ranges[0].end, 249);
  cr_assert_eq(ranges[1].start, 250);
  cr_assert_eq(ranges[1].end, 499);
  cr_assert_eq(ranges[2].start, 500);
  cr_assert_eq(ranges[2].end, 749);
  cr_assert_eq(ranges[3].start, 750);
  cr_assert_eq(ranges[3].end, 999);
}

Test(segmenter_plan, handles_remainder) {
  Range ranges[8];
  int n = segmenter_plan(1001, 4, ranges);
  cr_assert_eq(n, 4);
  cr_assert_eq(ranges[3].start, 750);
  cr_assert_eq(ranges[3].end, 1000);
}

Test(segmenter_plan, small_file) {
  Range ranges[8];
  int n = segmenter_plan(5, 8, ranges);
  cr_assert_eq(n, 5);
  for (int i = 0; i < 5; i++) {
    cr_assert_eq(ranges[i].start, i);
    cr_assert_eq(ranges[i].end, i);
  }
}

Test(segmenter_plan, zero_size) {
  Range ranges[8];
  int n = segmenter_plan(0, 4, ranges);
  cr_assert_eq(n, 0);
}

Test(segmenter_plan, negative_workers) {
  Range ranges[8];
  int n = segmenter_plan(100, -1, ranges);
  cr_assert_eq(n, 0);
}

Test(segmenter_plan, supports_sixteen_connections_without_overflow) {
  Range ranges[MAX_WORKERS];
  int n = segmenter_plan(160, 99, ranges);
  cr_assert_eq(n, 16);
  cr_assert_eq(ranges[0].start, 0);
  cr_assert_eq(ranges[15].start, 150);
  cr_assert_eq(ranges[15].end, 159);
}

Test(choose_worker_count, thresholds) {
  cr_assert_eq(choose_worker_count(0, 16), 1);
  cr_assert_eq(choose_worker_count(1023, 16), 1);
  cr_assert_eq(choose_worker_count(1024 * 1024, 8), 4);
  cr_assert_eq(choose_worker_count(20 * 1024 * 1024 - 1, 8), 4);
  cr_assert_eq(choose_worker_count(20 * 1024 * 1024, 8), 8);
  cr_assert_eq(choose_worker_count(100 * 1024 * 1024, 16), 16);
  cr_assert_eq(choose_worker_count(100 * 1024 * 1024, 3), 3);
  cr_assert_eq(choose_worker_count(1024 * 1024, 2), 2);
  cr_assert_eq(choose_worker_count(100 * 1024 * 1024, 99), 16);
  cr_assert_eq(choose_worker_count(100 * 1024 * 1024, 0), 1);
}
