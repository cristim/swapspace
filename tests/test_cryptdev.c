/* Tests for the sysfs lookups of crypt mode, which need no privileges.
 *
 * A fake sysfs tree in a scratch directory stands in for /sys (the tests point
 * crypt_sysfs_root at it), so finding mappings by name, recognizing the entries
 * of /proc/swaps, and proving that a mapping sits on our file can be checked
 * with every way a mapping can fail to be ours.  The fake system of
 * tests/fake_system.h is used by the tests that follow.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include "../src/crypt.c"
#include "../src/support.c"
#include "../src/cryptdev.c"
#include "fake_system.h"

/* Stand-ins for the globals and logging that main.c and log.c provide. */
char localbuf[16384];
bool verbose = false;
bool quiet = true;

void logm(int priority, const char fmt[], ...)
{
  if (!getenv("TEST_CRYPTDEV_VERBOSE")) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

void log_perr(int priority, const char msg[], int err)
{
  logm(priority, "%s: errno %d", msg, err);
}

void log_perr_str(int priority, const char msg[], const char arg[], int err)
{
  logm(priority, "%s '%s': errno %d", msg, arg, err);
}


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

static const struct swap_ops *real_ops_table;


/// Fake sysfs: finding mappings, the slot of a /proc/swaps entry, and whether
/// the mapping is backed by our file
static void test_sysfs(void)
{
  swapops = real_ops_table;
  static char sys[1024];
  snprintf(sys, sizeof(sys), "%s", fake_path("sys"));
  crypt_sysfs_root = sys;

  fake_mkdirs("swap");
  fake_mkdirs("other");
  fake_write("data", "swap/5");
  fake_write("data", "other/5");
  fake_write("data", "swap/6");
  if (symlink(fake_path("swap"), fake_path("link")) != 0) perror("symlink");

  // dm-3 is swapspace-5 on loop2, which is backed by swap/5
  static const char *const loop2[] = { "loop2", NULL };
  static const char *const loop3[] = { "loop3", NULL };
  static const char *const two[] = { "loop4", "loop5", NULL };
  static const char *const disk[] = { "sda", NULL };
  static const char *const none[] = { NULL };
  fake_sysfs_dm(3, "swapspace-5", loop2);
  fake_sysfs_loop("loop2", fake_path("swap/5"));
  // dm-4 is someone else's
  fake_sysfs_dm(4, "cryptroot", disk);
  // dm-6 .. dm-10: mapping names of our form, each broken in a different way
  fake_sysfs_dm(6, "swapspace-7", loop3);		// backed by another dir
  fake_sysfs_loop("loop3", fake_path("other/5"));
  fake_sysfs_dm(7, "swapspace-8", two);			// two slaves
  fake_sysfs_loop("loop4", fake_path("swap/5"));
  fake_sysfs_loop("loop5", fake_path("swap/5"));
  fake_sysfs_dm(8, "swapspace-9", disk);		// not on a loop device
  fake_sysfs_dm(9, "swapspace-10", none);		// no slaves at all
  static const char *const loop6[] = { "loop6", NULL };
  fake_sysfs_dm(10, "swapspace-11", loop6);		// deleted backing file
  {
    char text[1100];
    snprintf(text, sizeof(text), "%s (deleted)", fake_path("swap/5"));
    fake_sysfs_loop("loop6", text);
  }
  static const char *const loop8[] = { "loop8", NULL };
  fake_sysfs_dm(11, "swapspace-12", loop8);		// backed via a symlink
  fake_sysfs_loop("loop8", fake_path("link/6"));
  static const char *const loop9[] = { "loop9", NULL };
  fake_sysfs_dm(12, "swapspace-13", loop9);		// no backing_file at all
  fake_mkdirs("sys/block/loop9/loop");

  int n = -1, dmno = -1;

  // Finding a dm device by its name.
  CHECK(crypt_find_dm(5, &dmno) && dmno == 3);
  CHECK(crypt_find_dm(7, &dmno) && dmno == 6);
  CHECK(!crypt_find_dm(6, &dmno));
  CHECK(!crypt_find_dm(0, &dmno));

  // A dm device without its /dev/mapper node still counts as existing.
  // (Slot 5 has no node in the test environment.)
  CHECK(crypt_mapping_exists(5));
  CHECK(!crypt_mapping_exists(30));

  // /proc/swaps names as the daemon sees them.
  CHECK(crypt_swaps_slot("/dev/dm-3", 32, &n) && n == 5);
  CHECK(!crypt_swaps_slot("/dev/dm-4", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/dm-99", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/dm-3", 5, &n));	// slot beyond the limit

  // Backing check, "/dev/dm-X" form.
  CHECK(crypt_mapping_backed_by("/dev/dm-3", 5, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-3", 5, "other/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-3", 5, "swap/6"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-3", 5, "swap/missing"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-6", 7, "swap/5"));
  CHECK(crypt_mapping_backed_by("/dev/dm-6", 7, "other/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-7", 8, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-8", 9, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-9", 10, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-10", 11, "swap/5"));
  CHECK(crypt_mapping_backed_by("/dev/dm-11", 12, "swap/6"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-12", 13, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-77", 5, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/sda1", 5, "swap/5"));

  // "/dev/mapper/swapspace-N" form: the dm device is found by name.
  CHECK(crypt_mapping_backed_by("/dev/mapper/swapspace-5", 5, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-5", 5, "other/5"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-6", 6, "swap/6"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-5", 6, "swap/5"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/other-5", 5, "swap/5"));

  // The traced identity, and the cheap re-check against it.
  struct crypt_mapping m;
  CHECK(crypt_mapping_trace("/dev/dm-3", 5, &m) && m.dmno == 3 && m.loopno == 2);
  CHECK(crypt_mapping_is(&m, "swap/5") && !crypt_mapping_is(&m, "other/5"));
  CHECK(!crypt_mapping_is(&m, "swap/missing"));
  CHECK(!crypt_mapping_trace("/dev/dm-3", 6, &m));	// name is not swapspace-6
  CHECK(crypt_mapping_trace("/dev/mapper/swapspace-5", 5, &m) && m.dmno == 3);
  CHECK(crypt_mapping_unchanged(&m, 5));
  CHECK(!crypt_mapping_unchanged(&m, 6));

  /* Only the entry itself counts: a symlink or a second hard link to a file
   * that a mapping sits on, and a non-regular file, are never "ours".
   */
  CHECK(symlink("5", fake_path("swap/alias")) == 0);
  CHECK(link(fake_path("swap/6"), fake_path("swap/6b")) == 0);
  CHECK(mkfifo(fake_path("swap/fifo"), 0600) == 0);
  CHECK(crypt_mapping_is(&m, "swap/5") && !crypt_mapping_is(&m, "swap/alias"));
  CHECK(!crypt_mapping_backed_by("/dev/mapper/swapspace-12", 12, "swap/6") &&
      !crypt_mapping_backed_by("/dev/mapper/swapspace-12", 12, "swap/6b"));
  static const char *const loop10[] = { "loop10", NULL };
  fake_sysfs_dm(13, "swapspace-14", loop10);
  fake_sysfs_loop("loop10", fake_path("swap/fifo"));
  CHECK(!crypt_mapping_backed_by("/dev/dm-13", 14, "swap/fifo"));

  // The loop device now backs another file: no longer the mapping we traced.
  fake_sysfs_loop("loop2", fake_path("other/5"));
  CHECK(!crypt_mapping_unchanged(&m, 5));
  fake_sysfs_loop("loop2", fake_path("swap/5"));
  CHECK(crypt_mapping_unchanged(&m, 5));

  // The dm device was renamed or sits on a different loop device.
  fake_write("swapspace-9\n", "sys/block/dm-3/dm/name");
  CHECK(!crypt_mapping_unchanged(&m, 5));
  fake_write("swapspace-5\n", "sys/block/dm-3/dm/name");
  CHECK(crypt_mapping_unchanged(&m, 5));
  CHECK(rmdir(fake_path("sys/block/dm-3/slaves/loop2")) == 0);
  CHECK(!crypt_mapping_unchanged(&m, 5));
  fake_mkdirs("sys/block/dm-3/slaves/loop2");
  CHECK(crypt_mapping_unchanged(&m, 5));

  crypt_sysfs_root = "/sys";
}


/// Reading mapper names from sysfs-style files
static void test_names(void)
{
  int n = -1;
  CHECK(crypt_swaps_slot("/dev/mapper/swapspace-3", 32, &n) && n == 3);
  CHECK(!crypt_swaps_slot("/dev/mapper/other-3", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/sda2", 32, &n));
  CHECK(!crypt_swaps_slot("/var/lib/swapspace/3", 32, &n));
  CHECK(!crypt_swaps_slot("/dev/dm-4294967295999", 32, &n));

  fake_write("swapspace-12\n", "dmname");
  const char *const file = fake_path("dmname");
  CHECK(crypt_read_mapper_name(file, 32, &n) && n == 12);
  fake_write("swapspace-12", "dmname");
  CHECK(crypt_read_mapper_name(file, 32, &n) && n == 12);
  fake_write("swapspace-12\nx\n", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  fake_write("swapspace-40\n", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  fake_write("cryptroot\n", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  fake_write("", "dmname");
  CHECK(!crypt_read_mapper_name(file, 32, &n));
  CHECK(!crypt_read_mapper_name("/nonexistent/dm/name", 32, &n));
}


int main(void)
{
  fake_dir_create("swapspace-cryptdev");
  real_ops_table = swapops;

  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 2; }
  if (chdir(fake_dir) != 0) { perror("chdir"); return 2; }

  test_sysfs();
  test_names();

  if (chdir(cwd) != 0) perror("chdir back");
  fake_dir_remove();

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
