/*
This file is part of Swapspace.

Swapspace is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Swapspace is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with swapspace; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/
#include "env.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/swap.h>

#include "cryptdev.h"
#include "support.h"

/// Slot limit for reading mapper names; matches MAX_SWAPFILES in swaps.c
#define SLOT_LIMIT 32


/// Read a sysfs "name" file and test it against swapspace-<seqno>
static bool dm_name_is(const char node[], int seqno)
{
  char path[PATH_MAX];
  int found;
  return crypt_sysfs_block_path(path, sizeof(path), node, "dm/name") &&
    crypt_read_mapper_name(path, SLOT_LIMIT, &found) && found == seqno;
}


/* opendir() allocates, as do the fopen() of /proc/swaps and the system() calls
 * elsewhere in the daemon.  This runs when a swapfile is set up, or when a
 * mapping not yet known shows up in /proc/swaps.
 */
bool crypt_find_dm(int seqno, int *dmno)
{
  char dir[PATH_MAX];
  if (unlikely(snprintf(dir, sizeof(dir), "%s/block", crypt_sysfs_root) >=
	(int)sizeof(dir)))
    return false;

  DIR *const d = opendir(dir);
  if (!d) return false;

  bool found = false;
  for (const struct dirent *e = readdir(d); e && !found; e = readdir(d))
  {
    if (strncmp(e->d_name, "dm-", 3) != 0) continue;

    char devnode[PATH_MAX];
    if (snprintf(devnode, sizeof(devnode), "/dev/%s", e->d_name) >=
	(int)sizeof(devnode))
      continue;
    if (crypt_dm_node_number(devnode, dmno) && dm_name_is(e->d_name, seqno))
      found = true;
  }
  closedir(d);
  return found;
}


/// Does a device-mapper device for swapfile seqno exist, with or without a node?
/** A dm device can exist without its /dev/mapper node (udev lagging or
 * failing).  cryptsetup open would then fail on the name forever, so the
 * kernel's own list of dm names is consulted as well.
 */
static bool real_mapping_exists(int seqno)
{
  char path[64];
  int dmno;
  return (crypt_mapper_path(path, sizeof(path), seqno) &&
      access(path, F_OK) == 0) ||
    crypt_find_dm(seqno, &dmno);
}


bool crypt_mapping_exists(int seqno)
{
  return swapops->mapping_exists(seqno);
}


static int real_run(const char line[])
{
  return runcommandline(line);
}

static int real_mkswap(const char dev[])
{
  return runcommand("mkswap", dev);
}

static int real_swapon(const char dev[])
{
  return swapon(dev, 0);
}

static int real_swapoff(const char dev[])
{
  return swapoff(dev);
}

static const struct swap_ops real_ops =
{
  .run = real_run,
  .mapping_exists = real_mapping_exists,
  .mkswap = real_mkswap,
  .swapon = real_swapon,
  .swapoff = real_swapoff,
};

const struct swap_ops *swapops = &real_ops;


bool crypt_read_mapper_name(const char sysfs_path[], int limit, int *seqno)
{
  const int fd = open(sysfs_path, O_RDONLY|O_CLOEXEC);
  if (fd == -1) return false;

  char name[64];
  const ssize_t len = read(fd, name, sizeof(name) - 1);
  close(fd);
  if (len <= 0) return false;

  name[len] = '\0';
  if (name[len-1] == '\n') name[len-1] = '\0';
  return crypt_parse_mapper_name(name, limit, seqno);
}


/// Read the first line of a sysfs file into buf (without the newline)
static bool read_line(const char path[], char buf[], size_t size)
{
  const int fd = open(path, O_RDONLY|O_CLOEXEC);
  if (fd == -1) return false;

  const ssize_t len = read(fd, buf, size - 1);
  close(fd);
  if (len <= 0) return false;

  buf[len] = '\0';
  char *const nl = strchr(buf, '\n');
  if (nl) *nl = '\0';
  return buf[0] != '\0';
}


/// Device and inode of the file behind loop<loopno>, from sysfs
/** A deleted backing file is reported with a suffix that makes stat() fail. */
static bool loop_backing_identity(int loopno, dev_t *dev, ino_t *ino)
{
  char node[32], path[PATH_MAX], backing[PATH_MAX];
  struct stat st;
  if (snprintf(node, sizeof(node), "loop%d", loopno) >= (int)sizeof(node) ||
      !crypt_sysfs_block_path(path, sizeof(path), node, "loop/backing_file") ||
      !read_line(path, backing, sizeof(backing)) ||
      stat(backing, &st) != 0)
    return false;
  *dev = st.st_dev;
  *ino = st.st_ino;
  return true;
}


/// The loop device number if name is "loop<digits>", else -1
static int loop_number(const char name[])
{
  if (strncmp(name, "loop", 4) != 0 || !name[4] || strlen(name) > 12) return -1;
  int n = 0;
  for (const char *p = name + 4; *p; ++p)
  {
    if (*p < '0' || *p > '9') return -1;
    n = n * 10 + (*p - '0');
  }
  return n;
}


bool crypt_mapping_trace(const char devname[], int seqno,
    struct crypt_mapping *map)
{
  int dmno, n;
  if (!crypt_dm_node_number(devname, &dmno))
  {
    // Listed as /dev/mapper/swapspace-<n>: find the dm device by its name.
    if (!crypt_parse_mapper_path(devname, SLOT_LIMIT, &n) || n != seqno ||
	!crypt_find_dm(seqno, &dmno))
      return false;
  }

  char node[32], path[PATH_MAX];
  if (snprintf(node, sizeof(node), "dm-%d", dmno) >= (int)sizeof(node) ||
      !dm_name_is(node, seqno) ||
      !crypt_sysfs_block_path(path, sizeof(path), node, "slaves"))
    return false;

  // The mapping must sit on exactly one device, and that must be a loop device.
  DIR *const d = opendir(path);
  if (!d) return false;
  int loopno = -1, count = 0;
  for (const struct dirent *e = readdir(d); e; e = readdir(d))
  {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    ++count;
    loopno = loop_number(e->d_name);
  }
  closedir(d);
  if (count != 1 || loopno < 0) return false;

  map->dmno = dmno;
  map->loopno = loopno;
  return loop_backing_identity(loopno, &map->dev, &map->ino);
}


bool crypt_mapping_is(const struct crypt_mapping *map, const char file[])
{
  /* The entry itself, not what it points to: a symlink or a second hard link
   * in our directory must not make us claim another instance's backing file.
   */
  struct stat ours;
  return lstat(file, &ours) == 0 && S_ISREG(ours.st_mode) &&
    ours.st_nlink == 1 && map->dev == ours.st_dev && map->ino == ours.st_ino;
}


bool crypt_mapping_backed_by(const char devname[], int seqno, const char file[])
{
  struct crypt_mapping map;
  return crypt_mapping_trace(devname, seqno, &map) &&
    crypt_mapping_is(&map, file);
}


bool crypt_mapping_unchanged(const struct crypt_mapping *map, int seqno)
{
  char node[32], rest[48], path[PATH_MAX];
  dev_t dev;
  ino_t ino;
  return snprintf(node, sizeof(node), "dm-%d", map->dmno) < (int)sizeof(node) &&
    dm_name_is(node, seqno) &&
    snprintf(rest, sizeof(rest), "slaves/loop%d", map->loopno) <
      (int)sizeof(rest) &&
    crypt_sysfs_block_path(path, sizeof(path), node, rest) &&
    access(path, F_OK) == 0 &&
    loop_backing_identity(map->loopno, &dev, &ino) &&
    dev == map->dev && ino == map->ino;
}


bool crypt_swaps_slot(const char devname[], int limit, int *seqno)
{
  if (crypt_parse_mapper_path(devname, limit, seqno)) return true;

  char sysfs[64];
  return crypt_dm_sysfs_path(devname, sysfs, sizeof(sysfs)) &&
    crypt_read_mapper_name(sysfs, limit, seqno);
}
