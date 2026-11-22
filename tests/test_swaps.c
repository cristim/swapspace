/* Tests for swaps.c that need no privileges.
 *
 * This includes the daemon's own swaps.c, so the real allocation, retirement
 * and startup code runs, against the in-memory fake system of
 * tests/fake_system.h (no loop devices, no cryptsetup, no real swapon) and a
 * fake /proc/swaps, all inside a temporary directory.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Compiled-in default for the swap directory; the test overrides it. */
#ifndef VARPREFIX
#define VARPREFIX "/var"
#endif

#include "../src/calc.c"
#include "../src/crypt.c"
#include "../src/support.c"
#include "../src/cryptdev.c"
#include "fake_system.h"

/* Count the calls that decide how a swapfile is populated. */
static int fallocate_calls = 0;
static int fsync_calls = 0;

static int counting_fallocate(int fd, off_t off, off_t len)
{
  ++fallocate_calls;
  return posix_fallocate(fd, off, len);
}

static int counting_fsync(int fd)
{
  ++fsync_calls;
  return fsync(fd);
}

#define posix_fallocate counting_fallocate
#define fsync counting_fsync

#include "../src/swaps.c"

#undef posix_fallocate
#undef fsync

/* Stand-ins for the globals and functions of the rest of the daemon. */
char localbuf[16384];
time_t runclock = 0;
bool verbose = false;
bool quiet = false;
static int diets = 0;

void request_diet(void) { ++diets; }
void dump_memory(void) { }
void dump_state(void) { }

static char logtext[8192];

void logm(int priority, const char fmt[], ...)
{
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  if (strlen(logtext) + strlen(line) + 2 < sizeof(logtext))
  {
    strcat(logtext, line);
    strcat(logtext, "\n");
  }
  if (getenv("TEST_SWAPS_VERBOSE")) fprintf(stderr, "%s\n", line);
}

void log_perr(int priority, const char msg[], int err)
{
  logm(priority, "%s: errno %d", msg, err);
}

void log_perr_str(int priority, const char msg[], const char arg[], int err)
{
  logm(priority, "%s '%s': errno %d", msg, arg, err);
}

void log_discrep(int priority, const char what[], int seqno, memsize_t a,
    memsize_t b)
{
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

#define CHECK_LOG(expected) \
  do \
  { \
    if (strcmp(fake.log, (expected)) != 0) \
    { \
      fprintf(stderr, "FAIL %s:%d: log is '%s', expected '%s'\n", __FILE__, \
	  __LINE__, fake.log, (expected)); \
      ++failures; \
    } \
  } while (0)


/* --paranoid does not exist in the unconfigurable build. */
#ifndef NO_CONFIG
#define SET_PARANOID(value) (paranoid = (value))
#else
#define SET_PARANOID(value) ((void)0)
#endif

static char sysroot[1024];
static char procpath[1024];

static bool exists(const char name[])
{
  struct stat st;
  return lstat(name, &st) == 0;
}

static void remove_numbered_files(void)
{
  DIR *d = opendir(".");
  if (!d) return;
  for (struct dirent *e = readdir(d); e; e = readdir(d))
    if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
      unlink(e->d_name);
  closedir(d);
}

/// Back to a known state, in the swap directory of the scratch area
static void reset(bool crypt)
{
  remove_numbered_files();
  memset(swapfiles, 0, sizeof(swapfiles));
  sequence_number = MAX_SWAPFILES - 1;	// so that the next slot used is 0
  pfalloc_ok = true;
  proc_swaps_read_ok = false;
  diets = 0;
  fallocate_calls = fsync_calls = 0;
  logtext[0] = '\0';
  fake_reset();
  swapops = &fake_ops;
  crypt_enabled = crypt;
  SET_PARANOID(false);
  proc_swaps_path = procpath;
  fake_write("Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n", "proc_swaps");
}

static void set_proc_swaps(const char body[])
{
  char text[2048];
  snprintf(text, sizeof(text),
      "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n%s", body);
  fake_write(text, "proc_swaps");
}

static void make_file(const char name[], size_t bytes, char fill)
{
  FILE *fp = fopen(name, "w");
  if (!fp) { perror(name); exit(2); }
  for (size_t i = 0; i < bytes; ++i) fputc(fill, fp);
  if (fclose(fp) != 0) { perror(name); exit(2); }
}

static memsize_t page(void) { return getpagesize(); }

static bool all_bytes(const char name[], char value)
{
  FILE *fp = fopen(name, "r");
  if (!fp) return false;
  int c;
  bool ok = true;
  while ((c = fgetc(fp)) != EOF) if (c != (unsigned char)value) ok = false;
  fclose(fp);
  return ok;
}


/// Retirement of plain swapfiles, which shares remove_swapfile() with startup
static void test_retire_plain(void)
{
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  fake.log[0] = '\0';
  CHECK(retire_all());
  CHECK_LOG("plain-swapoff");
  CHECK(!exists("0") && swapfiles[0].size == 0 && fake.plain_swapped == 0);

  // swapoff failing keeps the file and the slot.
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  fake.fail_swapoff = true;
  CHECK(!retire_all());
  CHECK(exists("0") && swapfiles[0].size != 0);

#ifndef NO_CONFIG
  // Paranoid retirement overwrites the file before it is gone.
  reset(false);
  paranoid = true;
  CHECK(alloc_swapfile(8 * page()));
  CHECK(link("0", "keep") == 0);
  {
    FILE *fp = fopen("0", "r+");
    CHECK(fp != NULL);
    if (fp) { for (int i = 0; i < 4096; ++i) fputc('S', fp); fclose(fp); }
  }
  CHECK(!all_bytes("keep", '\0'));	// control: the data is there
  CHECK(retire_all());
  CHECK(!exists("0") && exists("keep"));
  CHECK(all_bytes("keep", '\0'));

  /* A file that cannot be unlinked (the kernel refuses that for active swap)
   * holds live data, so it must not be wiped either.  Unlinking is made to fail
   * by taking write permission off the directory, which root ignores.
   */
  if (geteuid() != 0)
  {
    reset(false);
    paranoid = true;
    make_file("3", 4 * page(), 'M');
    CHECK(chmod(".", 0555) == 0);
    remove_swapfile("3", MEMSIZE_ERROR);
    CHECK(chmod(".", 0755) == 0);
    CHECK(exists("3") && all_bytes("3", 'M'));
  }
#endif
}


/// The retry after a plain swapon() failing with EINVAL (no posix_fallocate)
static void test_plain_einval(void)
{
  const long long wanted = trunc_to_page(8 * page()) + 2 * page();

  // Exactly one retry, asking for the same size again: the request is reduced
  // by the overhead that alloc_swapfile() adds back.
  reset(false);
  fake.fail_swapon = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("plain-mkswap;plain-swapon;plain-mkswap;plain-swapon");
  CHECK(fake.plain_sizes_n == 2);
  CHECK(fake.plain_sizes[0] == wanted && fake.plain_sizes[1] == wanted);
  CHECK(!pfalloc_ok);
  CHECK(!exists("0") && swapfiles[0].size == 0);

  // Once quick creation is off, there is no further retry.
  reset(false);
  pfalloc_ok = false;
  fake.fail_swapon = true;
  CHECK(!alloc_swapfile(8 * page()));
  CHECK_LOG("plain-mkswap;plain-swapon");

  // Other errors are not retried, and leave quick creation on.
  const int others[] = { ENOSPC, EBUSY, ENOMEM, EPERM };
  for (size_t i = 0; i < sizeof(others) / sizeof(*others); ++i)
  {
    reset(false);
    fake.fail_swapon = true;
    fake.swapon_errno = others[i];
    CHECK(!alloc_swapfile(8 * page()));
    CHECK_LOG("plain-mkswap;plain-swapon");
    CHECK(pfalloc_ok && diets >= 1);
  }
}


/// dump_stats keeps its table unless some slot is encrypted
static void test_dump_stats(void)
{
  reset(false);
  CHECK(alloc_swapfile(8 * page()));
  logtext[0] = '\0';
  dump_stats();
  CHECK(strstr(logtext,
	"file            size            used         created  seen\n") != NULL);
  CHECK(strstr(logtext, "crypt") == NULL);
  // One row, ending in the "seen" flag.
  const char *row = strstr(logtext, "   0");
  CHECK(row != NULL && strstr(row, "  0\n") != NULL);
}


/// Startup with crypt off: old files are reused as before
static void test_startup_plain(void)
{
  const size_t size = 4 * (size_t)page();

  // An old file is reused as plain swap.
  reset(false);
  make_file("3", size, '\0');
  CHECK(activate_old_swaps());
  CHECK_LOG("plain-mkswap;plain-swapon");
  CHECK(exists("3"));

  // A file that cannot be enabled because it is busy (EBUSY) is kept; any
  // other error deletes it.
  reset(false);
  make_file("3", size, '\0');
  fake.fail_swapon = true;
  fake.swapon_errno = EBUSY;
  CHECK(activate_old_swaps());
  CHECK(exists("3") && swapfiles[3].size == 0);

  reset(false);
  make_file("3", size, '\0');
  fake.fail_swapon = true;
  fake.swapon_errno = ENOSPC;
  CHECK(activate_old_swaps());
  CHECK(!exists("3"));

  // Names that are not swapfile numbers are never touched.
  reset(false);
  make_file("notes", size, 'N');
  CHECK(activate_old_swaps());
  CHECK(exists("notes"));
}


int main(void)
{
  fake_dir_create("swapspace-swaps");
  fake_mkdirs("swap");
  snprintf(sysroot, sizeof(sysroot), "%s/sys", fake_dir);
  snprintf(procpath, sizeof(procpath), "%s/proc_swaps", fake_dir);
  crypt_sysfs_root = sysroot;

  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) { perror("getcwd"); return 2; }
  if (chdir(fake_path("swap")) != 0) { perror("chdir"); return 2; }
  snprintf(swappath, sizeof(swappath), "%s", fake_path("swap"));
  swappath_len = strlen(swappath);

  min_swapsize = 4 * page();
  max_swapsize = 256 * page();

  test_retire_plain();
  test_plain_einval();
  test_dump_stats();
  test_startup_plain();

  if (chdir(cwd) != 0) perror("chdir back");
  fake_dir_remove();

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
