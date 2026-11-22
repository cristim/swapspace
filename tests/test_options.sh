#!/bin/sh
# Table-driven tests for every entry of the options[] table in src/opts.c.
#
# Nothing here needs root: `swapspace -i` validates the configuration and
# exits, so each case is one invocation checked for its exit status and
# message.  Every case starts from a configuration that passes `-i` and
# changes one thing.
#
# A row of a table reads
#
#     modes|tokens|expect
#
# modes   which ways of passing the settings to run the row through:
#           c  configuration file       "freetarget = 50"
#           l  long option with '='     --freetarget=50
#           s  long option, next arg    --freetarget 50
#           S  short option, next arg   -f 50
#           A  short option, attached   -f50
#         '*' means all five.
# tokens  space-separated settings, "name=value" or a bare "name".  They
#         are applied on top of the base configuration, so a setting that
#         repeats one of the base's overrides it.
# expect  "ok" (exit status 0), "!text" (output must not contain text), or
#         any other text: exit status 1 with that text in the output.
#
# To cover a new option add it to OPTIONS below and give it boundary rows.

BIN="${BIN:-../src/swapspace}"
SRCDIR="${srcdir:-.}"

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit 77; }

test -x "$BIN" || skip "swapspace binary not built (run 'make' first)"

PS=$(getconf PAGESIZE 2>/dev/null) || skip "getconf PAGESIZE is unavailable"
LONG_MAX=$(getconf LONG_MAX 2>/dev/null) || LONG_MAX=0

TMP=$(mktemp -d "${TMPDIR:-/tmp}/swapspace-opts.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

set -f
OUT="$TMP/out"
mkdir -m 700 "$TMP/swap" "$TMP/sp ace" || exit 1

# Thresholds chosen so that minimal_swapfile() is (almost) zero, so the result
# does not depend on the space the build filesystem happens to have.
cat >"$TMP/base.conf" <<EOF
swappath = "$TMP/swap"
lower_freelimit = 50
freetarget = 50
upper_freelimit = 60
psi_threshold = 5
EOF

FAILS=0
CASES=0
bad() { FAILS=$((FAILS+1)); echo "FAIL: $*" >&2; }

# name, short letter, kind (num, str or flag)
OPTIONS="
buffer_elasticity B num
cache_elasticity C num
configfile c str
cooldown a num
daemon d flag
erase e flag
freetarget f num
help h flag
inspect i flag
lower_freelimit l num
max_swapsize M num
min_swapsize m num
paranoid P flag
pidfile p str
psi_threshold x num
quiet q flag
swappath s str
upper_freelimit u num
verbose v flag
version V flag
"

# Make the table's letters available as $L_<name>.
while read -r name letter kind; do
  test -n "$name" && eval "L_$name=$letter"
done <<EOF
$OPTIONS
EOF

# Every option of the daemon's own table must be in OPTIONS above.
sed -n 's/^  { "\([a-z_]*\)",.*/\1/p' "$SRCDIR/../src/opts.c" | sort >"$TMP/src.names"
echo "$OPTIONS" | awk 'NF {print $1}' | sort >"$TMP/test.names"
test -s "$TMP/src.names" || skip "cannot read the options table from src/opts.c"
if ! cmp -s "$TMP/src.names" "$TMP/test.names"; then
  bad "OPTIONS does not match src/opts.c: $(diff "$TMP/src.names" "$TMP/test.names" | tr '\n' ' ')"
fi

# Run the daemon with the arguments given; output and status end up in $OUT
# and $RC.
run() {
  "$BIN" "$@" >"$OUT" 2>&1 </dev/null
  RC=$?
}

# check LABEL EXPECT: compare the last run() against an expectation.
check() {
  CASES=$((CASES+1))
  case $2 in
  ok)
    [ "$RC" -eq 0 ] || bad "$1: expected success, got status $RC: $(head -n 1 "$OUT")"
    ;;
  '!'*)
    if grep -F -q -- "${2#!}" "$OUT"; then
      bad "$1: output unexpectedly contains '${2#!}'"
    fi
    ;;
  *)
    if [ "$RC" -ne 1 ]; then
      bad "$1: expected status 1 ('$2'), got $RC"
    elif ! grep -F -q -- "$2" "$OUT"; then
      bad "$1: expected '$2', got: $(head -n 1 "$OUT")"
    fi
    ;;
  esac
}

# run_mode MODE TOKENS: run `swapspace -i` with the settings passed in MODE.
run_mode() {
  mode=$1
  tokens=$2
  cp "$TMP/base.conf" "$TMP/case.conf"
  set --
  for tok in $tokens; do
    case $tok in
    *=*) key=${tok%%=*}; val=${tok#*=}; hasval=1 ;;
    *)   key=$tok; val=; hasval=0 ;;
    esac
    eval "letter=\${L_$key:-}"
    case $mode in
    c)
      if [ $hasval = 1 ]; then
        printf '%s = %s\n' "$key" "$val"
      else
        printf '%s\n' "$key"
      fi >>"$TMP/case.conf"
      ;;
    l) if [ $hasval = 1 ]; then set -- "$@" "--$key=$val"; else set -- "$@" "--$key"; fi ;;
    s) set -- "$@" "--$key"; [ $hasval = 1 ] && set -- "$@" "$val" ;;
    S) set -- "$@" "-$letter"; [ $hasval = 1 ] && set -- "$@" "$val" ;;
    A) set -- "$@" "-$letter$val" ;;
    esac
  done
  run "$@" -i -c "$TMP/case.conf"
}

# rows < FILE: run every row of FILE through its modes.
rows() {
  while IFS='|' read -r modes tokens expect; do
    case $modes in ''|'#'*) continue ;; esac
    test "$modes" = '*' && modes=clsSA
    n=${#modes}
    while [ "$n" -gt 0 ]; do
      m=$(printf '%s' "$modes" | cut -c "$n")
      n=$((n-1))
      run_mode "$m" "$tokens"
      check "[$m] $tokens" "$expect"
    done
  done
}

# Sanity check of the harness itself: the base configuration passes.
run -i -c "$TMP/base.conf"
check "base configuration" ok

# Rows every option of a kind has to satisfy, generated from OPTIONS.
GEN="$TMP/generated.rows"
: >"$GEN"
while read -r name letter kind; do
  case $kind in
  num)
    cat >>"$GEN" <<EOF
*|$name|requires
c|$name=|syntax error
lsSA|$name=|requires
*|$name=abc|invalid unit letter
*|$name=1x|invalid unit letter
*|$name=1.5|invalid unit letter
*|$name=1kb|invalid numeric argument
*|$name=5k5|invalid numeric argument
*|$name=99999999999999999999|out of range
*|$name=-99999999999999999999|out of range
*|$name=9223372036854775807k|out of range
EOF
    ;;
  esac
done <<EOF
$OPTIONS
EOF
rows <"$GEN"

# Numeric options.  Rows that need other settings to pass the cross-checks
# carry them, so a failure always points at the option under test.

# Percentages: the parser accepts 0..100 and a value above is "too large".
# 0x64 and 0144 are 100 in hexadecimal and octal, one more is too large.
rows <<EOF
*|buffer_elasticity=0|ok
*|buffer_elasticity=50|ok
*|buffer_elasticity=100|ok
*|buffer_elasticity=+100|ok
*|buffer_elasticity=-1|may not be negative
*|buffer_elasticity=101|too large
*|buffer_elasticity=1k|too large
*|buffer_elasticity=0k|ok
*|buffer_elasticity=0x64|ok
*|buffer_elasticity=0x65|too large
*|buffer_elasticity=0144|ok
*|buffer_elasticity=0145|too large
*|buffer_elasticity=08|invalid unit letter
*|cache_elasticity=0|ok
*|cache_elasticity=100|ok
*|cache_elasticity=-1|may not be negative
*|cache_elasticity=101|too large
*|cache_elasticity=1m|too large
*|psi_threshold=0|ok
*|psi_threshold=100|ok
*|psi_threshold=-1|may not be negative
*|psi_threshold=101|too large
*|psi_threshold=1g|too large
*|freetarget=2 lower_freelimit=2|ok
*|freetarget=99 lower_freelimit=99 upper_freelimit=100|ok
*|freetarget=1 lower_freelimit=0|too small
*|freetarget=0|too small
*|freetarget=-1|too small
*|freetarget=100|too large
*|freetarget=1t|too large
*|lower_freelimit=0 freetarget=2|ok
*|lower_freelimit=99 freetarget=99 upper_freelimit=100|ok
*|lower_freelimit=-1|may not be negative
*|lower_freelimit=100|too large
*|lower_freelimit=1k|too large
*|upper_freelimit=100 lower_freelimit=60 freetarget=60|ok
*|upper_freelimit=2 lower_freelimit=2 freetarget=2|ok
*|upper_freelimit=-1|may not be negative
*|upper_freelimit=101|too large
*|upper_freelimit=1g|too large
*|upper_freelimit=1 lower_freelimit=0|freetarget > upper_freelimit
*|upper_freelimit=0 lower_freelimit=0|freetarget > upper_freelimit
EOF

# Seconds: any non-negative value, suffixes multiply.
rows <<EOF
*|cooldown=0|ok
*|cooldown=1|ok
*|cooldown=600|ok
*|cooldown=-1|may not be negative
*|cooldown=1k|ok
*|cooldown=1m|ok
EOF
if [ "$LONG_MAX" = 9223372036854775807 ]; then
  rows <<EOF
*|cooldown=9223372036854775807|ok
*|cooldown=9223372036854775808|out of range
*|cooldown=8388607t|ok
*|cooldown=8388608t|out of range
*|cooldown=9007199254740992k|out of range
EOF
fi

# Sizes in bytes.  The parser only requires 8192, but swaps_check_config()
# also wants min_swapsize >= 10 pages and min_swapsize <= max_swapsize, and
# both values are truncated to whole pages.  The messages tell which of the
# two layers rejected a value.
rows <<EOF
*|min_swapsize=$((10*PS))|ok
*|min_swapsize=$((10*PS+1))|ok
*|min_swapsize=$((10*PS-1))|min_swapsize < 10*getpagesize()
*|min_swapsize=$((9*PS))|min_swapsize < 10*getpagesize()
*|min_swapsize=8192|min_swapsize < 10*getpagesize()
*|min_swapsize=8191|too small
*|min_swapsize=0|too small
*|min_swapsize=1|too small
*|min_swapsize=-1|too small
*|min_swapsize=4m|ok
*|min_swapsize=4194304|ok
*|min_swapsize=0x400000|ok
*|min_swapsize=1g|ok
*|min_swapsize=2t|ok
*|min_swapsize=3t|min_swapsize > max_swapsize
*|min_swapsize=8192t|min_swapsize > max_swapsize
*|min_swapsize=8388607t|min_swapsize > max_swapsize
*|min_swapsize=8388607t max_swapsize=8388607t|ok
*|min_swapsize=8388608t|out of range
*|min_swapsize=9223372036854775807 max_swapsize=9223372036854775807|ok
*|min_swapsize=9223372036854775808|out of range
*|min_swapsize=7k|too small
*|min_swapsize=8k|min_swapsize < 10*getpagesize()
*|max_swapsize=8192|min_swapsize > max_swapsize
*|max_swapsize=8191|too small
*|max_swapsize=0|too small
*|max_swapsize=-1|too small
*|max_swapsize=7k|too small
*|max_swapsize=8k|min_swapsize > max_swapsize
*|max_swapsize=1m min_swapsize=1m|ok
*|max_swapsize=4m|ok
*|max_swapsize=4m min_swapsize=4m|ok
*|max_swapsize=$((4*1024*1024-PS)) min_swapsize=4m|min_swapsize > max_swapsize
*|max_swapsize=$((4*1024*1024+PS-1)) min_swapsize=4m|ok
*|max_swapsize=$((4*1024*1024+PS-1)) min_swapsize=$((4*1024*1024+PS))|min_swapsize > max_swapsize
*|max_swapsize=1t|ok
*|max_swapsize=2t|ok
*|max_swapsize=9223372036854775807|ok
*|max_swapsize=8388607t|ok
*|max_swapsize=8388608t|out of range
*|max_swapsize=9223372036854775808|out of range
*|max_swapsize=$((10*PS)) min_swapsize=$((10*PS))|ok
*|max_swapsize=$((10*PS-1)) min_swapsize=$((10*PS))|min_swapsize > max_swapsize
*|min_swapsize=$((10*PS+1)) max_swapsize=$((10*PS))|ok
*|min_swapsize=$((11*PS-1)) max_swapsize=$((10*PS))|ok
EOF

# Size suffixes are k, m, g and t (either case), each 1024 times the one
# before.  Pairs of settings that are equal only if the suffix is exact.
rows <<EOF
*|min_swapsize=1m max_swapsize=1024k|ok
*|min_swapsize=1m max_swapsize=1023k|min_swapsize > max_swapsize
*|min_swapsize=1M max_swapsize=1024K|ok
*|min_swapsize=1M max_swapsize=1023K|min_swapsize > max_swapsize
*|min_swapsize=1g max_swapsize=1024m|ok
*|min_swapsize=1g max_swapsize=1023m|min_swapsize > max_swapsize
*|min_swapsize=1G max_swapsize=1024M|ok
*|min_swapsize=1G max_swapsize=1023M|min_swapsize > max_swapsize
*|min_swapsize=1t max_swapsize=1024g|ok
*|min_swapsize=1t max_swapsize=1023g|min_swapsize > max_swapsize
*|min_swapsize=1T max_swapsize=1024G|ok
*|min_swapsize=1T max_swapsize=1023G|min_swapsize > max_swapsize
*|min_swapsize=1024k max_swapsize=1048575|min_swapsize > max_swapsize
*|min_swapsize=1048576 max_swapsize=1k|too small
EOF

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS of $CASES test_options cases failed" >&2
  exit 1
fi
echo "test_options: $CASES cases passed"
exit 0
