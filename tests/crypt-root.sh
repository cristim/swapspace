#!/bin/sh
# Destructive integration test for crypt mode (encrypted swapfiles).
#
# RUN ONLY ON A MACHINE WHERE TINY TEMPORARY SWAP AREAS ARE ACCEPTABLE, as root.
# This test activates real swap (a few files of 4 to 8 MB each), creates loop
# devices and dm-crypt mappings named swapspace-<n>, and removes them again.
# It refuses to run unless SWAPSPACE_CRYPT_TEST=1 is set, and refuses to start
# if a mapping named swapspace-* already exists or a swapspace process that may
# use crypt mode is running (either could belong to a real instance).  It only
# ever swaps off, closes or detaches things that it created: mappings whose
# loop device is backed by a file in its own directory, and whose names it
# recorded, plus those loop devices and the plain files there.
#
# Usage, as root from the build tree:
#   SWAPSPACE_CRYPT_TEST=1 ./tests/crypt-root.sh [parent-directory]
#
# From a desktop session with polkit, pkexec starts it as root.  It strips the
# environment: pass variables through env(1) before the script path (after it,
# BIN would be an argument and be ignored) and use absolute paths:
#   pkexec env SWAPSPACE_CRYPT_TEST=1 BIN=/abs/path/to/src/swapspace \
#     /abs/path/to/tests/crypt-root.sh /var/tmp
# The binary is BIN if set, else src/swapspace next to this script, never one
# found through the current directory.  It must be an absolute path to a regular
# executable owned by root or the invoking user (PKEXEC_UID, SUDO_UID), not
# writable by group or others, and its -h must list crypt.  Its path and
# version are printed.
#
# The parent directory (default /var/tmp) must be on a filesystem and device
# where O_DIRECT works with the loop device's block size, which swapspace takes
# from the file's STATX_DIOALIGN (typically the filesystem block size, 4096 on
# 4Kn storage).  If direct I/O is not available, the test reports that crypt
# mode refused to start there.  A throwaway directory is created inside the
# parent, which is resolved to its real path first, and removed at the end,
# including after failures.  Cleanup only touches mappings and loop devices
# whose backing file lies inside that directory.  The parent must not be on
# tmpfs or ramfs (the loop device needs direct I/O) and needs 600 MB free.
#
# Cleanup ignores INT, TERM and HUP so that it always finishes.  After SIGKILL
# of the script, or a reported cleanup problem, check by hand: "dmsetup ls"
# (swapspace-* mappings), "losetup -l" (loops backed by <parent>/swapspace-
# crypt-test.*), "pgrep -x swapspace" (stray daemons, up to 32 x 8 MB), and
# "BIN -c <parent>/swapspace-crypt-test.*/crypt.conf -e" to retire the rest.
#
# Requires: cryptsetup, dmsetup, losetup, mkswap, swapoff, dd, grep, awk, pgrep,
# tr, sed, and kernel support for loop (Linux 5.8 or later) and dm-crypt.
#
# Scenarios:
#  1. allocation: files are created, mapped and enabled; /proc/swaps lists the
#     mapper devices and never the plain files; the cipher is visible in the
#     kernel's state

if [ "${SWAPSPACE_CRYPT_TEST:-0}" != "1" ]; then
  echo "Refusing to run: set SWAPSPACE_CRYPT_TEST=1 (this test activates real" >&2
  echo "swap and creates loop and dm-crypt devices)" >&2
  exit 77
fi

scriptdir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P) || exit 2
BIN=${BIN:-${scriptdir%/*}/src/swapspace}
case $BIN in
  /*) ;;
  *) echo "BIN must be an absolute path: $BIN" >&2; exit 2 ;;
esac
if [ ! -f "$BIN" ] || [ ! -x "$BIN" ]; then
  echo "Not an executable regular file: $BIN (build swapspace or set BIN)" >&2
  exit 2
fi
invoker=${PKEXEC_UID:-${SUDO_UID:-$(id -u)}}
owner=$(stat -L -c %u "$BIN")
case $owner in
  0|"$invoker") ;;
  *) echo "$BIN is owned by uid $owner, not root or uid $invoker" >&2; exit 2 ;;
esac
case $(stat -L -c %A "$BIN") in
  ?????w*|????????w*)
    echo "$BIN is writable by group or others" >&2; exit 2 ;;
esac
if ! "$BIN" -h 2>&1 | grep -q -- '--crypt'; then
  echo "$BIN does not support crypt mode (rebuild without --enable-noconfig)" >&2
  exit 2
fi
echo "binary: $BIN ($("$BIN" -V 2>&1 | sed -n 1p))"

if [ "$(id -u)" -ne 0 ]; then
  echo "This test must run as root" >&2
  exit 77
fi

set -u

for tool in cryptsetup dmsetup losetup mkswap swapoff dd grep awk pgrep tr sed \
    setsid stat df; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing required tool: $tool" >&2
    exit 77
  fi
done

if [ ! -e /dev/loop-control ]; then
  echo "No /dev/loop-control: loop device support is missing" >&2
  exit 77
fi

# A running swapspace that uses crypt mode owns mappings named swapspace-<n> and
# would fight over memory pressure with the daemons started below.  Crypt mode
# can come from the command line or from the configuration file; look at both.
for pid in $(pgrep -x swapspace 2>/dev/null); do
  args=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null)
  case $args in
    *crypt*|*" -E"*|*" -"[A-Za-z]*E*)
      echo "Refusing to run: swapspace (pid $pid) runs with crypt options: $args" >&2
      exit 77 ;;
  esac
  conf=$(echo "$args" | sed -n 's/.*\(-c\|--configfile\)[= ]\([^ ]*\).*/\2/p')
  for f in ${conf:-/etc/swapspace.conf /usr/local/etc/swapspace.conf}; do
    if [ -r "$f" ] && grep -Eq '^[[:space:]]*crypt([[:space:]]|=|$)' "$f"; then
      echo "Refusing to run: swapspace (pid $pid) uses crypt mode from $f" >&2
      exit 77
    fi
  done
done
unset pid args conf f

existing=$(dmsetup ls 2>/dev/null | awk '$1 ~ /^swapspace-[0-9]+$/ {print $1}')
if [ -n "$existing" ]; then
  echo "Refusing to run: mappings named swapspace-* already exist:" >&2
  echo "$existing" >&2
  exit 77
fi

parent=${1:-/var/tmp}
if [ ! -d "$parent" ]; then
  echo "Not a directory: $parent" >&2
  exit 2
fi
# Resolve symlinks and relative components: the checks below compare paths with
# what the kernel reports, and a relative or symlinked parent would make the
# "nothing left" checks pass without looking at anything.
parent=$(CDPATH= cd -- "$parent" && pwd -P) || exit 2
case $parent in
  /*) ;;
  *) echo "Cannot resolve $parent" >&2; exit 2 ;;
esac
case $parent in
  *[\"#]*) echo "A quote or # in $parent cannot go into the configuration" >&2
           exit 2 ;;
esac
case $(stat -f -c %T "$parent") in
  tmpfs|ramfs) echo "$parent is on tmpfs or ramfs; use a disk filesystem" >&2
               exit 2 ;;
esac
avail_kb=$(df -Pk "$parent" | awk 'NR == 2 {print $4}')
if [ "${avail_kb:-0}" -lt 614400 ]; then
  echo "Need 600 MB free in $parent, have ${avail_kb:-0} KB" >&2
  exit 2
fi

base=$(mktemp -d "$parent/swapspace-crypt-test.XXXXXX") || exit 1
swapdir="$base/swap"
mkdir "$swapdir" && chmod 700 "$swapdir" || exit 1
daemon_pid=""
other=""
failures=0

not() { ! "$@"; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*" >&2; failures=$((failures + 1)); }
skip() { echo "SKIP: $*"; }

# check DESCRIPTION COMMAND...
check() {
  desc=$1; shift
  if "$@" >/dev/null 2>&1; then pass "$desc"; else fail "$desc"; fi
}

# Slot numbers of all active encrypted swaps, one per line.
active_slots() {
  while read -r name rest; do
    case $name in
      /dev/mapper/swapspace-[0-9]*)
        echo "${name#/dev/mapper/swapspace-}" ;;
      /dev/dm-[0-9]*)
        node=${name#/dev/}
        if [ -r "/sys/block/$node/dm/name" ]; then
          mname=$(cat "/sys/block/$node/dm/name")
          case $mname in
            swapspace-[0-9]*) echo "${mname#swapspace-}" ;;
          esac
        fi ;;
    esac
  done </proc/swaps
}

count_slots() { active_slots | wc -l; }

mappers() { dmsetup ls 2>/dev/null | awk '$1 ~ /^swapspace-[0-9]+$/ {print $1}'; }

# Loop devices whose backing file lies under the test directory (including
# deleted files, which losetup still lists with their old path).
test_loops() {
  losetup -l -n -O NAME,BACK-FILE 2>/dev/null |
    awk -v p="$base/" 'index($2, p) == 1 {print $1}'
}

old_files() {
  for f in "$swapdir"/* ${other:+"$other"/*}; do
    case ${f##*/} in
      ''|*[!0-9]*) continue ;;
    esac
    [ -e "$f" ] && echo "$f"
  done
}

# Wait up to $1 seconds for the command in the remaining arguments to succeed.
wait_for() {
  limit=$1; shift
  while [ "$limit" -gt 0 ]; do
    if "$@" >/dev/null 2>&1; then return 0; fi
    sleep 1
    limit=$((limit - 1))
  done
  "$@" >/dev/null 2>&1
}

at_least_slots() { [ "$(count_slots)" -ge "$1" ]; }

no_slots() { [ -z "$(active_slots)" ]; }

no_numbered_files() { [ -z "$(old_files)" ]; }

no_plain_swaps() { ! grep -qF "$swapdir/" /proc/swaps; }

files_exist() {
  [ $# -gt 0 ] || return 1
  for s in "$@"; do [ -e "$swapdir/$s" ] || return 1; done
}

nothing_left() {
  [ -z "$(active_slots)" ] &&
  [ -z "$(mappers)" ] &&
  [ -z "$(test_loops)" ] &&
  no_plain_swaps
}

# Wait up to $1 seconds for the daemon to exit.
daemon_exits() {
  waited=0
  while kill -0 "$daemon_pid" 2>/dev/null && [ "$waited" -lt "$1" ]; do
    sleep 1
    waited=$((waited + 1))
  done
  ! kill -0 "$daemon_pid" 2>/dev/null
}

# The daemon runs in its own process group (see start_daemon), so a cryptsetup
# or mkswap it started, which survives SIGKILL of the daemon and can still
# create a mapping, is found by group.  Let those finish, then kill leftovers.
drain_group() {
  waited=0
  while pgrep -g "$daemon_pid" >/dev/null 2>&1 && [ "$waited" -lt 30 ]; do
    sleep 1
    waited=$((waited + 1))
  done
  if pgrep -g "$daemon_pid" >/dev/null 2>&1; then
    kill -KILL -- "-$daemon_pid" 2>/dev/null
    sleep 2
  fi
}

# Stop the daemon: SIGTERM, then SIGKILL after $1 seconds, then drain its group.
stop_daemon() {
  [ -n "$daemon_pid" ] || return 0
  note_mappers
  kill -TERM "$daemon_pid" 2>/dev/null
  if ! daemon_exits "${1:-60}"; then
    fail "daemon did not exit within ${1:-60} seconds of SIGTERM"
    kill -KILL "$daemon_pid" 2>/dev/null
  fi
  wait "$daemon_pid" 2>/dev/null
  daemon_status=$?
  drain_group
  note_mappers
  daemon_pid=""
}

# Names of the swapspace-* mappings whose loop device is backed by a file inside
# the test directory: the ones this test (through the daemon) created.
own_mappers() {
  for m in $(mappers); do
    dev=$(dmsetup deps -o devname "$m" 2>/dev/null |
      sed -n 's/.*(\(loop[0-9][0-9]*\)).*/\1/p')
    [ -n "$dev" ] || continue
    back=$(losetup -l -n -O NAME,BACK-FILE 2>/dev/null |
      awk -v d="/dev/$dev" '$1 == d {print $2}')
    case $back in
      "$base"/*) echo "$m" ;;
    esac
  done
}

# Record the names of the mappings that are ours right now, once per name.
# Cleanup acts on recorded names only.
created="$base/created-mappers"
: >"$created"
note_mappers() {
  for m in $(own_mappers); do
    grep -qx "$m" "$created" || echo "$m" >>"$created"
  done
}

# Remove everything this test may have created.  Runs on every exit path.
cleanup() {
  trap '' INT TERM HUP	# a second Ctrl-C must not stop cleanup half-way
  stop_daemon 10
  note_mappers
  for m in $(own_mappers); do
    # Never touch a mapping that was not recorded as ours.
    grep -qx "$m" "$created" || continue
    swapoff "/dev/mapper/$m" 2>/dev/null
    cryptsetup close "$m" 2>/dev/null
  done
  for l in $(test_loops); do losetup -d "$l" 2>/dev/null; done
  old_files | while IFS= read -r f; do swapoff "$f" 2>/dev/null; done
  case $base in
    "$parent"/swapspace-crypt-test.*) rm -rf "$base" ;;
  esac
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

write_conf() {
  # write_conf FILE [extra lines...]; WCDIR overrides the swap directory
  conf=$1; shift
  {
    cat <<EOF
swappath = "${WCDIR:-$swapdir}"
lower_freelimit = 99
freetarget = 99
upper_freelimit = 100
min_swapsize = 4m
max_swapsize = 8m
cooldown = 5
EOF
    for line in "$@"; do echo "$line"; done
  } >"$conf"
}

start_daemon() {
  # start_daemon CONF LOG
  setsid "$BIN" -c "$1" -v >"$2" 2>&1 &
  daemon_pid=$!
}

echo "kernel: $(uname -r); $(cryptsetup --version)"
echo "swap directory: $swapdir ($(stat -f -c %T "$parent"))"

write_conf "$base/crypt.conf" "crypt" "crypt_cipher = aes-xts-plain64" \
  "crypt_keysize = 512"
write_conf "$base/plain.conf"
write_conf "$base/paranoid.conf" "crypt" "paranoid"
write_conf "$base/badcipher.conf" "crypt" \
  "crypt_cipher = nosuchcipher-xts-plain64"

# === 1. allocation ==========================================================
echo "=== 1. allocation ==="
start_daemon "$base/crypt.conf" "$base/log1"
if wait_for 40 at_least_slots 2; then
  pass "encrypted swap appears in /proc/swaps (two slots)"
else
  fail "no two encrypted swaps after 40 seconds; log follows"
  tail -20 "$base/log1" >&2
fi
stop_daemon
check "daemon exits cleanly on SIGTERM" [ "$daemon_status" -eq 0 ]

slots=$(active_slots)
first=$(echo "$slots" | sed -n 1p)
second=$(echo "$slots" | sed -n 2p)

check "no plain swapfile is listed in /proc/swaps" no_plain_swaps
check "files exist for each slot" files_exist $slots

if [ -n "$first" ]; then
  check "dmsetup table shows the cipher" \
    sh -c "dmsetup table swapspace-$first | grep -q 'crypt aes-xts-plain64 '"
fi

echo
if [ "$failures" -eq 0 ]; then
  echo "ALL PASSED"
  exit 0
fi
echo "$failures check(s) failed; logs are in $base until this script exits" >&2
exit 1
