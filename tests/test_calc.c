/* Unit tests for the pure sizing arithmetic in src/calc.c.
 *
 * calc.c is included directly so that the tests exercise exactly the code that
 * goes into the daemon without exposing an otherwise internal API.
 */
#include <stdio.h>
#include <unistd.h>

#include "../src/calc.c"

static int failures = 0;

#define CHECK(expr) \
  do \
  { \
    if (!(expr)) \
    { \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
      ++failures; \
    } \
  } while (0)


int main(void)
{
  const memsize_t page = getpagesize();

  // Page rounding.
  CHECK(trunc_to_page(0) == 0);
  CHECK(trunc_to_page(page-1) == 0);
  CHECK(trunc_to_page(page) == page);
  CHECK(trunc_to_page(page+1) == page);
  CHECK(ext_to_page(0) == 0);
  CHECK(ext_to_page(1) == page);
  CHECK(ext_to_page(page) == page);
  CHECK(ext_to_page(page+1) == 2*page);
  CHECK(ext_to_page(2*page-1) == 2*page);
  CHECK(ext_to_page(2*page) == 2*page);
  CHECK(ext_to_page(2*page+1) == 3*page);

  // At the target, no adjustment is needed.
  CHECK(ideal_swapsize(100000, 30000, 30) == 0);
  CHECK(ideal_swapsize(100000, 50000, 50) == 0);

  // Below the target: a positive adjustment, close to the requested target.
  const memsize_t add = ideal_swapsize(100000, 20000, 30);
  CHECK(add == 14200);
  CHECK(20000+add > 0);
  CHECK((20000+add)*100/(100000+add) >= 29);

  // Above the target: a negative adjustment.
  CHECK(ideal_swapsize(100000, 60000, 30) < 0);

  // Targets that are not the default work the same way.
  CHECK(ideal_swapsize(100000, 40000, 50) == 20000);

  // PSI line parsing.
  int psi = -1;
  CHECK(parse_psi_line(
      "some avg10=1.23 avg60=0.00 avg300=0.00 total=0", &psi));
  CHECK(psi == 1);
  CHECK(parse_psi_line(
      "some avg10=12.99 avg60=0.00 avg300=0.00 total=0", &psi));
  CHECK(psi == 12);
  CHECK(!parse_psi_line(
      "full avg10=99.00 avg60=0.00 avg300=0.00 total=0", &psi));
  CHECK(!parse_psi_line("garbage", &psi));
  CHECK(!parse_psi_line("some avg10=-1.0", &psi));
  CHECK(parse_psi_line("some avg10=150.0", &psi));
  CHECK(psi == 100);

  // cgroup limit parsing.
  memsize_t cg = 0;
  CHECK(!parse_cgroup_limit("max\n", &cg));
  CHECK(parse_cgroup_limit("123456\n", &cg) && cg == 123456);
  CHECK(parse_cgroup_limit("  4096  \n", &cg) && cg == 4096);
  CHECK(!parse_cgroup_limit("-1\n", &cg));
  CHECK(!parse_cgroup_limit("12x\n", &cg));
  CHECK(!parse_cgroup_limit("", &cg));
  // cgroup v1's "unlimited" sentinel is still a valid number; memory.c filters
  // values near LLONG_MAX.
  CHECK(parse_cgroup_limit("9223372036854771712\n", &cg));

  // ---- Percentage mode: percent_swap_request(total, free, freepct, lower, upper,
  // target, pressure) with lower=20, upper=60, target=30 --------------------
  // Inside the band: nothing to do.
  CHECK(percent_swap_request(100000, 40000, 40, 20, 60, 30, false) == 0);
  CHECK(percent_swap_request(100000, 20000, 20, 20, 60, 30, false) == 0);
  CHECK(percent_swap_request(100000, 60000, 60, 20, 60, 30, false) == 0);
  // Below the lower limit: add swap towards the target.
  CHECK(percent_swap_request(100000, 10000, 10, 20, 60, 30, false) > 0);
  CHECK(percent_swap_request(100000, 19999, 19, 20, 60, 30, false) > 0);
  // Above the upper limit: report excess.
  CHECK(percent_swap_request(100000, 80000, 80, 20, 60, 30, false) < 0);
  CHECK(percent_swap_request(100000, 61000, 61, 20, 60, 30, false) < 0);
  // Pressure inside the band and at or below the target: add swap early.
  CHECK(percent_swap_request(100000, 25000, 25, 20, 60, 30, true) > 0);
  CHECK(percent_swap_request(100000, 30000, 30, 20, 60, 30, true) == 0);
  // Pressure above the target but inside the band: leave it alone.
  CHECK(percent_swap_request(100000, 45000, 45, 20, 60, 30, true) == 0);
  // Pressure above the upper limit: never release swap.
  CHECK(percent_swap_request(100000, 80000, 80, 20, 60, 30, true) == 0);
  // Pressure below the lower limit still adds.
  CHECK(percent_swap_request(100000, 10000, 10, 20, 60, 30, true) > 0);
  // The amount agrees with ideal_swapsize().
  CHECK(percent_swap_request(100000, 10000, 10, 20, 60, 30, false) ==
        ideal_swapsize(100000, 10000, 30));

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
