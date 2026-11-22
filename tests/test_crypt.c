/* Unit tests for the pure helpers in src/crypt.c.
 *
 * Like test_calc, this includes the source file so the tests exercise exactly
 * the code that goes into the daemon.
 */
#include <stdio.h>

#include "../src/crypt.c"

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
  // Defaults: off, and the defaults themselves must be acceptable.
  CHECK(!crypt_enabled);
  CHECK(crypt_cipher_valid(crypt_cipher));
  CHECK(crypt_keysize_valid(crypt_cipher, crypt_keysize));

  // Cipher whitelist.
  CHECK(crypt_cipher_valid("aes-xts-plain64"));
  CHECK(crypt_cipher_valid("aes-cbc-essiv:sha256"));
  CHECK(crypt_cipher_valid("serpent-xts-plain64"));
  CHECK(crypt_cipher_valid("a"));
  CHECK(crypt_cipher_valid("Aes_XTS.plain-64:x"));
  CHECK(!crypt_cipher_valid(NULL));
  CHECK(!crypt_cipher_valid(""));
  CHECK(!crypt_cipher_valid("-aes"));       // would look like an option
  CHECK(!crypt_cipher_valid(":aes"));
  CHECK(!crypt_cipher_valid("aes xts"));
  CHECK(!crypt_cipher_valid("aes\txts"));
  CHECK(!crypt_cipher_valid("aes\nxts"));
  CHECK(!crypt_cipher_valid("aes;reboot"));
  CHECK(!crypt_cipher_valid("aes&&id"));
  CHECK(!crypt_cipher_valid("aes|id"));
  CHECK(!crypt_cipher_valid("aes$(id)"));
  CHECK(!crypt_cipher_valid("aes`id`"));
  CHECK(!crypt_cipher_valid("aes'id"));
  CHECK(!crypt_cipher_valid("aes\"id"));
  CHECK(!crypt_cipher_valid("aes\\id"));
  CHECK(!crypt_cipher_valid("aes>x"));
  CHECK(!crypt_cipher_valid("aes<x"));
  CHECK(!crypt_cipher_valid("aes*"));
  CHECK(!crypt_cipher_valid("aes#x"));
  CHECK(!crypt_cipher_valid("capi:xts(aes)-plain64"));
  CHECK(!crypt_cipher_valid("aes/xts"));
  CHECK(!crypt_cipher_valid("aes\x80"));   // non-ASCII byte

  // Ciphers that protect nothing are refused, in any letter case: null ciphers
  // and the ECB mode (which keeps repeated blocks recognizable).
  CHECK(!crypt_cipher_valid("cipher_null-ecb"));
  CHECK(!crypt_cipher_valid("cipher_null"));
  CHECK(!crypt_cipher_valid("cipher_null-cbc-plain64"));
  CHECK(!crypt_cipher_valid("CIPHER_NULL-ecb"));
  CHECK(!crypt_cipher_valid("aes-null-plain64"));
  CHECK(!crypt_cipher_valid("aes-ecb"));
  CHECK(!crypt_cipher_valid("aes-ECB-plain"));
  CHECK(!crypt_cipher_valid("serpent-ecb"));
  CHECK(crypt_cipher_valid("aes-cbc-essiv:sha256"));	// "ecb" must be a whole part
  CHECK(crypt_cipher_valid("aes-xts-plain64"));
  // Length limit: 63 characters fit, 64 do not.
  char longname[CRYPT_CIPHER_MAX + 1];
  memset(longname, 'a', sizeof(longname));
  longname[CRYPT_CIPHER_MAX - 1] = '\0';
  CHECK(crypt_cipher_valid(longname));
  longname[CRYPT_CIPHER_MAX - 1] = 'a';
  longname[CRYPT_CIPHER_MAX] = '\0';
  CHECK(!crypt_cipher_valid(longname));

  // Key sizes: range and multiples of 8.
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 120));
  CHECK(crypt_keysize_valid("serpent-cbc-plain", 128));
  CHECK(crypt_keysize_valid("serpent-cbc-plain", 136));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 129));
  CHECK(crypt_keysize_valid("serpent-cbc-plain", 1024));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 1032));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", 0));
  CHECK(!crypt_keysize_valid("serpent-cbc-plain", -256));

  // XTS takes two keys: 256 or 512 only.
  CHECK(crypt_keysize_valid("aes-xts-plain64", 256));
  CHECK(crypt_keysize_valid("aes-xts-plain64", 512));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 128));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 192));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 384));
  CHECK(!crypt_keysize_valid("aes-xts-plain64", 1024));
  CHECK(crypt_keysize_valid("serpent-xts-plain64", 512));
  CHECK(!crypt_keysize_valid("serpent-xts-plain64", 128));
  // "xts" must be a whole component, not a substring.
  CHECK(crypt_keysize_valid("foo-boxtsx-plain", 128));

  // Other AES modes: 128, 192 or 256.
  CHECK(crypt_keysize_valid("aes-cbc-essiv:sha256", 128));
  CHECK(crypt_keysize_valid("aes-cbc-essiv:sha256", 192));
  CHECK(crypt_keysize_valid("aes-cbc-essiv:sha256", 256));
  CHECK(!crypt_keysize_valid("aes-cbc-essiv:sha256", 512));
  CHECK(!crypt_keysize_valid("aes-cbc-essiv:sha256", 136));

  // A bare "aes" is checked like the other AES modes; other ciphers only by
  // range, which is documented rather than enforced.
  CHECK(crypt_keysize_valid("aes", 128));
  CHECK(crypt_keysize_valid("aes", 256));
  CHECK(!crypt_keysize_valid("aes", 512));
  CHECK(!crypt_keysize_valid("aes", 136));
  CHECK(crypt_keysize_valid("aesx", 512));
  CHECK(crypt_keysize_valid("twofish-cbc-plain", 512));

  // Bits to bytes.
  CHECK(crypt_keysize_bytes(128) == 16);
  CHECK(crypt_keysize_bytes(256) == 32);
  CHECK(crypt_keysize_bytes(512) == 64);
  CHECK(crypt_keysize_bytes(1024) == 128);
  CHECK(crypt_keysize_bytes(127) == 0);
  CHECK(crypt_keysize_bytes(1025) == 0);
  CHECK(crypt_keysize_bytes(0) == 0);
  CHECK(crypt_keysize_bytes(-64) == 0);
  CHECK(crypt_keysize_bytes(130) == 0);

  // Setters, as the option table drives them.
  CHECK(set_crypt(0) == NULL && crypt_enabled);
  CHECK(set_crypt_cipher(0) == crypt_cipher);
  CHECK(set_crypt_keysize(256) == NULL && crypt_keysize == 256);
  CHECK(crypt_check_config());
  strcpy(crypt_cipher, "aes;id");
  CHECK(!crypt_check_config());
  strcpy(crypt_cipher, "aes-xts-plain64");
  set_crypt_keysize(192);
  CHECK(!crypt_check_config());

  if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
  return failures ? 1 : 0;
}
