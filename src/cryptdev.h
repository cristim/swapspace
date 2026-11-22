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
#ifndef SWAPSPACE_CRYPTDEV_H
#define SWAPSPACE_CRYPTDEV_H

#include <sys/types.h>

#include "crypt.h"

/* The parts of swap handling that touch the system go through the swap_ops
 * table below, which is a plain set of function pointers to the real
 * implementations (no allocation).  Tests replace the table to inject failures
 * at every step without root.
 */

/// System operations used for swap activation; replaceable by tests
struct swap_ops
{
  /// Run a complete shell command line; returns its exit status or -1
  int (*run)(const char line[]);
  /// Does a device-mapper device swapspace-<seqno> exist?
  bool (*mapping_exists)(int seqno);
  /// Run mkswap on a device or file; returns its exit status
  int (*mkswap)(const char dev[]);
  /// swapon(2) wrapper: 0, or -1 with errno set
  int (*swapon)(const char dev[]);
  /// swapoff(2) wrapper: 0, or -1 with errno set
  int (*swapoff)(const char dev[]);
};

/// The table in use; points at the real implementations unless a test swaps it
extern const struct swap_ops *swapops;

/// Does a device-mapper device for swapfile seqno exist?
/** True if /dev/mapper/swapspace-<seqno> exists, or if the kernel lists a dm
 * device of that name even though its node is missing.
 */
bool crypt_mapping_exists(int seqno);

/// Find the dm device named swapspace-<seqno> in sysfs; true and *dmno if found
bool crypt_find_dm(int seqno, int *dmno);

/// Which swapfile slot does a device name from /proc/swaps belong to?
/** Recognizes "/dev/mapper/swapspace-<n>" and "/dev/dm-<x>" whose mapper name
 * (read from sysfs) is "swapspace-<n>", for 0 <= n < limit.
 */
bool crypt_swaps_slot(const char devname[], int limit, int *seqno);

/// Where a mapping sits and which file backs it: what makes it ours or not
struct crypt_mapping
{
  /// Number of the dm device (dm-<dmno>)
  int dmno;
  /// Number of the loop device under it (loop<loopno>), its only slave
  int loopno;
  /// Device and inode of the loop device's backing file
  dev_t dev;
  ino_t ino;
};

/// Follow sysfs from a /proc/swaps device name to the backing file's identity
/** The name only says "swapspace-<seqno>"; mapping names are global.  This
 * finds the dm device, requires that it sits on exactly one device and that
 * this is a loop device, and records that device's backing file by device and
 * inode number (the daemon may reach the directory by another path than the
 * kernel reports).  Does not allocate memory beyond opendir().
 * @return false if anything is missing or not as expected
 */
bool crypt_mapping_trace(const char devname[], int seqno,
    struct crypt_mapping *map);

/// Is the traced mapping backed by "file" (relative to the current directory)?
/** Only if "file" is itself a regular file with a single link: a symlink or an
 * extra hard link to someone else's backing file is not ours.
 */
bool crypt_mapping_is(const struct crypt_mapping *map, const char file[]);

/// Is the mapping backed by our file?  A trace followed by a comparison
bool crypt_mapping_backed_by(const char devname[], int seqno, const char file[]);

/// Is the mapping still the one that was traced?  Cheap: no directory scan
/** Re-reads the dm name, checks that the loop device is still its slave, and
 * compares the loop device's current backing file with the recorded identity.
 */
bool crypt_mapping_unchanged(const struct crypt_mapping *map, int seqno);

/// Read a sysfs dm "name" file and recognize "swapspace-<n>" (exposed for tests)
bool crypt_read_mapper_name(const char sysfs_path[], int limit, int *seqno);

#endif
