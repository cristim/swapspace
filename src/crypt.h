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
#ifndef SWAPSPACE_CRYPT_H
#define SWAPSPACE_CRYPT_H

#include <stdbool.h>
#include <stddef.h>

/* Optional per-file encryption ("crypt" mode).
 *
 * This header declares the configuration and the pure helpers: validation and
 * name/command-line formatting that touch no system state, so they can be unit
 * tested without privileges.  The parts that talk to the kernel and to
 * cryptsetup live in cryptdev.c.
 */

/// Size of the cipher string buffer, including the terminating NUL
#define CRYPT_CIPHER_MAX 64

#define CRYPT_DEFAULT_CIPHER "aes-xts-plain64"
#define CRYPT_DEFAULT_KEYSIZE 512
#define CRYPT_KEYSIZE_MIN 128
#define CRYPT_KEYSIZE_MAX 1024

/// Is crypt mode switched on?  Always false in the unconfigurable build.
extern bool crypt_enabled;
/// dm-crypt cipher specification, e.g. "aes-xts-plain64"
extern char crypt_cipher[CRYPT_CIPHER_MAX];
/// Key size in bits
extern int crypt_keysize;

#ifndef NO_CONFIG
char *set_crypt(long long dummy);
char *set_crypt_cipher(long long dummy);
char *set_crypt_keysize(long long bits);

/// Verify the crypt configuration; prints a message to stderr on failure
bool crypt_check_config(void);
#endif

/// Is this an acceptable cipher string?
/** The string ends up on a shell command line, so it is restricted to a
 * whitelist: it must start with a letter or digit (so it cannot look like an
 * option), and may contain only letters, digits and any of ":_.-".  It must be
 * non-empty and shorter than CRYPT_CIPHER_MAX.  Ciphers that give no
 * confidentiality are refused: anything with "null" in its name (cipher_null
 * stores plaintext) and the ECB chain mode (repeated blocks stay visible), in
 * any letter case.
 */
bool crypt_cipher_valid(const char cipher[]);

/// Is this key size (in bits) usable with the given cipher?
/** The size must lie in [CRYPT_KEYSIZE_MIN, CRYPT_KEYSIZE_MAX] and be a
 * multiple of 8.  XTS modes take two keys, so their size must be 256 or 512;
 * "aes" and the other "aes-" modes take 128, 192 or 256.  Any other cipher
 * (serpent, twofish, ...) is only range-checked here: cryptsetup or the kernel
 * refuses an unsuitable size when the mapping is created, and crypt mode then
 * fails closed with a logged error instead of rejecting the configuration up
 * front.
 */
bool crypt_keysize_valid(const char cipher[], long long bits);

/// Number of key bytes for a key size in bits, or 0 if the size is unusable
int crypt_keysize_bytes(long long bits);

#endif
