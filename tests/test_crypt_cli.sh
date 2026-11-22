#!/bin/sh
# Command-line and configuration tests for the crypt options.
#
# These do not require root: crypt mode is only validated here (-i), never
# started.

BIN="${BIN:-../src/swapspace}"

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit 77; }

test -x "$BIN" || skip "swapspace binary not built (run 'make' first)"

TMP=$(mktemp -d "${TMPDIR:-/tmp}/swapspace-crypt-test.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

cat >"$TMP/base.conf" <<EOF2
swappath = "$TMP"
lower_freelimit = 50
freetarget = 50
upper_freelimit = 60
psi_threshold = 5
EOF2

ok() {
  desc=$1; shift
  "$BIN" -i -c "$TMP/base.conf" "$@" >/dev/null 2>&1 || fail "rejected: $desc"
}
bad() {
  desc=$1; shift
  if "$BIN" -i -c "$TMP/base.conf" "$@" >/dev/null 2>&1; then
    fail "accepted: $desc"
  fi
}

# The new options are documented in the usage text.
"$BIN" -h 2>&1 | grep -q -- '--crypt_cipher' || fail "help lacks crypt_cipher"
"$BIN" -h 2>&1 | grep -q -- '-E,' || fail "help lacks -E"

# Defaults and the short and long forms.
ok "no crypt options"
ok "-E" -E
ok "--crypt" --crypt
ok "-E -X cipher -K size" -E -X aes-xts-plain64 -K 512
ok "long forms" --crypt --crypt_cipher=aes-xts-plain64 --crypt_keysize=256
ok "aes-cbc-essiv" -E -X aes-cbc-essiv:sha256 -K 256
ok "serpent 1024" -E -X serpent-cbc-plain64 -K 1024

# Key size range and cipher rules.
bad "keysize 64" -E -K 64
bad "keysize 1032" -E -K 1032
bad "keysize not a number" -E -K abc
bad "xts keysize 128" -E -K 128
bad "xts keysize 384" -E -K 384
bad "aes-cbc keysize 512" -E -X aes-cbc-essiv:sha256 -K 512
ok "bare aes keysize 256" -E -X aes -K 256
bad "bare aes keysize 512" -E -X aes -K 512
bad "keysize not multiple of 8" -E -X serpent-cbc-plain64 -K 130

# Cipher whitelist: nothing that a shell would interpret.
bad "cipher with semicolon" -E "--crypt_cipher=aes-xts-plain64;touch $TMP/pwned"
bad "cipher with space" -E "--crypt_cipher=aes xts"
bad "cipher with substitution" -E '--crypt_cipher=$(id)'
bad "cipher with backtick" -E '--crypt_cipher=`id`'
bad "cipher with quote" -E "--crypt_cipher=aes'"
bad "cipher leading dash" -E --crypt_cipher=-aes-xts-plain64
bad "empty cipher" -E --crypt_cipher=
bad "cipher too long" -E "--crypt_cipher=$(printf 'a%.0s' $(seq 1 64))"
bad "cipher with pipe (validated even when crypt is off)" "--crypt_cipher=a|b"
test ! -e "$TMP/pwned" || fail "cipher text was executed"

# Ciphers that protect nothing.
bad "null cipher" -E --crypt_cipher=cipher_null-ecb -K 128
bad "null cipher, other mode" -E --crypt_cipher=cipher_null-cbc-plain64 -K 128
bad "ecb mode" -E --crypt_cipher=aes-ecb -K 256

# The same checks apply to the configuration file.
printf 'crypt\ncrypt_cipher = "aes-xts-plain64"\ncrypt_keysize = 512\n' >"$TMP/c1.conf"
cat "$TMP/base.conf" "$TMP/c1.conf" >"$TMP/c1full.conf"
"$BIN" -i -c "$TMP/c1full.conf" >/dev/null 2>&1 || fail "valid crypt config rejected"

printf 'crypt_cipher = "aes;id"\n' >"$TMP/c2.conf"
cat "$TMP/base.conf" "$TMP/c2.conf" >"$TMP/c2full.conf"
if "$BIN" -i -c "$TMP/c2full.conf" >/dev/null 2>&1; then
  fail "config file cipher with semicolon accepted"
fi

printf 'crypt = 1\n' >"$TMP/c3.conf"
cat "$TMP/base.conf" "$TMP/c3.conf" >"$TMP/c3full.conf"
if "$BIN" -i -c "$TMP/c3full.conf" >/dev/null 2>&1; then
  fail "crypt flag with an argument accepted"
fi

# Crypt off keeps the erase path working unprivileged on an empty directory.
"$BIN" -e -c "$TMP/base.conf" >/dev/null 2>&1 || fail "-e failed with crypt off"

exit 0
