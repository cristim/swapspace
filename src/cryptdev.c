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

#include <sys/swap.h>

#include "cryptdev.h"
#include "support.h"


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
  .mkswap = real_mkswap,
  .swapon = real_swapon,
  .swapoff = real_swapoff,
};

const struct swap_ops *swapops = &real_ops;
