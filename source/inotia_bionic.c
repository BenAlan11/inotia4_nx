/* inotia_bionic.c -- the bionic FORTIFY shims libgame.so imports that the
 * android32 runtime does not provide.
 *
 * gen_imports.py reports 13 unresolved imports for libgame.so. Four are plain
 * newlib functions (atof atoll rand srand: listed as PASSTHROUGH in
 * tools/imports.cfg); the nine here are bionic's _FORTIFY_SOURCE wrappers,
 * which take the destination size as an extra argument. The runtime has the
 * mem and str families in bionic_mem.c and __vsnprintf_chk in bionic_extra.c;
 * the rest are this file, written the same way: check the bound, then call the
 * runtime's b_ shim (not newlib directly) so the path stays the same one the
 * rest of the engine takes.
 *
 * MIT.
 */
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include "bionic_io.h"
#include "error.h"
#include "util.h"

/* ---------------------------------------------------------------- strings */

char *b___strcpy_chk(char *dst, const char *src, size_t dst_len) {
  size_t n = strlen(src) + 1;
  if (n > dst_len)
    fatal_error("__strcpy_chk: %u bytes into %u (from %p)", (unsigned)n, (unsigned)dst_len,
                __builtin_return_address(0));
  return strcpy(dst, src);
}

char *b___strcat_chk(char *dst, const char *src, size_t dst_len) {
  size_t have = strlen(dst);
  size_t n = have + strlen(src) + 1;
  if (n > dst_len)
    fatal_error("__strcat_chk: %u bytes into %u (from %p)", (unsigned)n, (unsigned)dst_len,
                __builtin_return_address(0));
  return strcat(dst, src);
}

char *b___strncpy_chk(char *dst, const char *src, size_t n, size_t dst_len) {
  if (n > dst_len)
    fatal_error("__strncpy_chk: %u bytes into %u (from %p)", (unsigned)n, (unsigned)dst_len,
                __builtin_return_address(0));
  return strncpy(dst, src, n);
}

char *b___strncat_chk(char *dst, const char *src, size_t n, size_t dst_len) {
  size_t have = strlen(dst);
  /* strncat appends at most n bytes and always terminates */
  size_t longest = have + n + 1;
  if (longest > dst_len)
    fatal_error("__strncat_chk: up to %u bytes into %u (from %p)", (unsigned)longest,
                (unsigned)dst_len, __builtin_return_address(0));
  return strncat(dst, src, n);
}

/* bionic: the caller passes the known size of `s`; the search may not leave it */
char *b___strchr_chk(const char *s, int c, size_t slen) {
  for (size_t i = 0; i <= slen; i++)
    if (s[i] == (char)c)
      return (char *)s + i;
  return NULL;
}

size_t b___strlen_chk(const char *s, size_t slen) {
  for (size_t i = 0; i < slen; i++)
    if (!s[i])
      return i;
  fatal_error("__strlen_chk: no NUL in %u bytes (from %p)", (unsigned)slen,
              __builtin_return_address(0));
}

/* ---------------------------------------------------------------- printf */

int b___vsprintf_chk(char *dst, int flags, size_t dst_len, const char *fmt, va_list ap) {
  (void)flags;
  /* Unbounded in bionic too; the bound is the caller's promise. The runtime's
   * vsnprintf goes through the NULL-safe core. */
  int n = vsnprintf(dst, dst_len, fmt, ap);
  if (n < 0)
    return n;
  if ((size_t)n >= dst_len)
    fatal_error("__vsprintf_chk: %d bytes into %u (from %p)", n, (unsigned)dst_len,
                __builtin_return_address(0));
  return n;
}

/* ------------------------------------------------------------------- fd */

/* __open_2(path, flags): the two-argument form of open(), no mode */
int b___open_2(const char *path, int flags) { return b_open(path, flags, 0); }

ssize_t b___read_chk(int fd, void *buf, size_t n, size_t buf_len) {
  if (n > buf_len)
    fatal_error("__read_chk: %u bytes into %u (from %p)", (unsigned)n, (unsigned)buf_len,
                __builtin_return_address(0));
  return b_read(fd, buf, n);
}
