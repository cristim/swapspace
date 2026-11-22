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

#include <stdio.h>
#include <string.h>

#include "crypt.h"


bool crypt_enabled = false;
char crypt_cipher[CRYPT_CIPHER_MAX] = CRYPT_DEFAULT_CIPHER;
int crypt_keysize = CRYPT_DEFAULT_KEYSIZE;


#ifndef NO_CONFIG
char *set_crypt(long long dummy)
{
  crypt_enabled = true;
  return NULL;
}

char *set_crypt_cipher(long long dummy)
{
  return crypt_cipher;
}

char *set_crypt_keysize(long long bits)
{
  crypt_keysize = (int)bits;
  return NULL;
}

bool crypt_check_config(void)
{
  if (!crypt_cipher_valid(crypt_cipher))
  {
    fputs("Configuration error: 'crypt_cipher': must start with a letter or "
	"digit, contain only letters, digits and ':_.-', and not name a null or "
	"ECB cipher\n", stderr);
    return false;
  }
  if (!crypt_keysize_valid(crypt_cipher, crypt_keysize))
  {
    fprintf(stderr,
	"Configuration error: 'crypt_keysize': %d bits is not usable with "
	"cipher '%s' (xts needs 256 or 512, other aes modes 128, 192 or 256)\n",
	crypt_keysize,
	crypt_cipher);
    return false;
  }
  return true;
}
#endif


/// Does the dash-separated cipher specification contain the given component?
static bool cipher_has_part(const char cipher[], const char part[])
{
  const size_t len = strlen(part);
  const char *p = cipher;
  for (;;)
  {
    const char *const dash = strchr(p, '-');
    const size_t n = dash ? (size_t)(dash - p) : strlen(p);
    if (n == len && strncmp(p, part, len) == 0) return true;
    if (!dash) return false;
    p = dash + 1;
  }
}


/// Does the cipher specification name a mode that provides no confidentiality?
/** "cipher_null" stores plaintext, and ECB leaves repeated blocks (zero pages,
 * repeated data) recognizable.  Neither is acceptable for swap.  Case is
 * ignored because the kernel's algorithm lookup is case-insensitive.
 */
static bool cipher_weak(const char cipher[])
{
  char low[CRYPT_CIPHER_MAX];
  size_t i;
  for (i = 0; cipher[i] && i < sizeof(low) - 1; ++i)
    low[i] = (cipher[i] >= 'A' && cipher[i] <= 'Z') ? cipher[i] + 32 : cipher[i];
  low[i] = '\0';
  return strstr(low, "null") != NULL || cipher_has_part(low, "ecb");
}


bool crypt_cipher_valid(const char cipher[])
{
  if (!cipher || !*cipher) return false;
  if (strlen(cipher) >= CRYPT_CIPHER_MAX) return false;

  for (const char *p = cipher; *p; ++p)
  {
    const char c = *p;
    const bool alnum =
      (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    if (alnum) continue;
    // Punctuation is fine, but not as the first character.
    if (p != cipher && (c == ':' || c == '_' || c == '.' || c == '-')) continue;
    return false;
  }
  return !cipher_weak(cipher);
}


bool crypt_keysize_valid(const char cipher[], long long bits)
{
  if (bits < CRYPT_KEYSIZE_MIN || bits > CRYPT_KEYSIZE_MAX) return false;
  if (bits % 8 != 0) return false;

  if (cipher_has_part(cipher, "xts")) return bits == 256 || bits == 512;

  // A bare "aes" is accepted by cryptsetup too (it defaults the mode).
  if (strcmp(cipher, "aes") == 0 || strncmp(cipher, "aes-", 4) == 0)
    return bits == 128 || bits == 192 || bits == 256;

  // Any other cipher is range-checked only.
  return true;
}


int crypt_keysize_bytes(long long bits)
{
  if (bits < CRYPT_KEYSIZE_MIN || bits > CRYPT_KEYSIZE_MAX || bits % 8 != 0)
    return 0;
  return (int)(bits / 8);
}

