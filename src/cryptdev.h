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

#include <stdbool.h>

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
  /// Run mkswap on a device or file; returns its exit status
  int (*mkswap)(const char dev[]);
  /// swapon(2) wrapper: 0, or -1 with errno set
  int (*swapon)(const char dev[]);
  /// swapoff(2) wrapper: 0, or -1 with errno set
  int (*swapoff)(const char dev[]);
};

/// The table in use; points at the real implementations unless a test swaps it
extern const struct swap_ops *swapops;

#endif
