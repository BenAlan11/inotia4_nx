/* libc_shim.h -- the bionic-shaped bits of libc that newlib does not provide
 *
 * libgame.so was linked against bionic. Everything here exists so the original
 * ARM64 binary can run unmodified: Android path translation, the __sF stdio
 * array, system properties, fortify wrappers, and the handful of libc calls
 * newlib on the Switch does not implement.
 */
#ifndef LIBC_SHIM_H
#define LIBC_SHIM_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>

/* Translates an Android-style path into the SD card layout. `buf` must hold at
 * least 512 bytes; returns buf, or `path` itself when no rewriting is needed. */
const char *shim_path(const char *path, char *buf, size_t bufsize);
/* Same, but for the fallback lookup: tries the language folder first, then
 * .../resources/common/... . Returns 1 when the returned path exists. */
int shim_path_resolve(const char *path, char *buf, size_t bufsize);

/* set by the port at startup */
void shim_set_dirs(const char *root, const char *data_dir, const char *save_dir);
const char *shim_package_name(void);

/* bionic exposes stdio as an array of FILE structs (__sF[0..2]); anything that
 * was compiled against bionic may hand us &__sF[2] where it means stderr. */
void *shim_sF_base(void);
size_t shim_sF_size(void);

/* implemented in libc_shim.c because newlib/libnx lack them */
void *shim_mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int   shim_munmap(void *addr, size_t len);
int   shim_fd_set_chk(int fd, void *set, unsigned long size);
int   shim_fd_isset_chk(int fd, void *set, unsigned long size);
void  shim_stack_chk_fail(void);
extern int *__errno_shim(void);
void *shim_popen(const char *cmd, const char *mode);
int   shim_pclose(void *f);
int   shim_system(const char *cmd);
void  shim_openlog(const char *ident, int option, int facility);
void  shim_syslog(int prio, const char *fmt, ...);
void  shim_closelog(void);
int   shim_statfs(const char *path, void *buf);
int   shim_popen_dummy(const char *cmd, const char *mode);
int   shim_pclose_dummy(void *f);
int   shim_system_dummy(const char *cmd);
void  shim_sincos(double x, double *s, double *c);
void  shim_sincosf(float x, float *s, float *c);
int   shim_getauxval(unsigned long type);
int   shim_system_property_get(const char *name, char *value);
void  shim_android_log_print(int prio, const char *tag, const char *fmt, ...);
void  shim_android_set_abort_message(const char *msg);
int   shim_cxa_atexit(void (*fn)(void *), void *arg, void *dso);
void  shim_cxa_finalize(void *dso);
int   shim_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data);
int   shim_getpagesize(void);
int   shim_getpid(void);
int   shim_kill(int pid, int sig);
int   shim_dup(int fd);
int   shim_fcntl(int fd, int cmd, ...);
uint32_t shim_inet_addr(const char *cp);
char *shim_inet_ntoa(uint32_t in);
int   shim_statfs_stub(const char *path, void *buf);

/* open/fopen/stat family: rewritten paths go through these */
FILE *shim_fopen(const char *path, const char *mode);
int   shim_open(const char *path, int flags, ...);
/* these fill the caller's buffer in Android's (bionic) struct stat layout */
int   shim_stat(const char *path, void *out);
int   shim_lstat(const char *path, void *out);
int   shim_fstat(int fd, void *out);
int   shim_access(const char *path, int mode);
int   shim_mkdir(const char *path, mode_t mode);
int   shim_unlink(const char *path);
int   shim_remove(const char *path);
int   shim_rmdir(const char *path);
int   shim_rename(const char *from, const char *to);
void *shim_opendir(const char *path);
int   shim_fclose(FILE *f);
size_t shim_fread(void *p, size_t sz, size_t n, FILE *f);
size_t shim_fwrite(const void *p, size_t sz, size_t n, FILE *f);
int   shim_fflush(FILE *f);
int   shim_fprintf(FILE *f, const char *fmt, ...);
int   shim_vfprintf(FILE *f, const char *fmt, va_list va);
int   shim_fputc(int c, FILE *f);
int   shim_fgetc(FILE *f);
char *shim_fgets(char *s, int n, FILE *f);
int   shim_feof(FILE *f);

/* connect() that fails fast once the menu is up; see libc_shim.c */
struct sockaddr;
int shim_connect(int fd, const struct sockaddr *addr, unsigned len);

#endif

/* logged wrappers around the calls a stalled engine tends to sit in */
int shim_usleep(unsigned int usec);
int shim_nanosleep(const struct timespec *req, struct timespec *rem);
clock_t shim_clock(void);
int shim_gettimeofday(struct timeval *tv, void *tz);

/* fortify wrappers: their argument lists differ from the plain functions */
int shim_vsprintf_chk(char *dest, int flags, size_t destlen, const char *fmt,
                      va_list ap);
int shim_vsnprintf_chk(char *dest, int flags, size_t destlen, size_t maxlen,
                       const char *fmt, va_list ap);

/* logged allocators, so an out-of-memory names the site that asked for it */
void *shim_malloc(size_t size);
void *shim_calloc(size_t n, size_t size);
void *shim_realloc(void *old, size_t size);

/* cumulative allocation counters (bring-up only) */
unsigned long alloc_calls(void);
unsigned long alloc_kb(void);
char *shim_strdup(const char *src);
void *shim_memalign(size_t align, size_t size);
int shim_vasprintf(char **out, const char *fmt, va_list ap);

/* logged pthread entry points (bring-up diagnostics) */
int shim_pthread_create(pthread_t *th, const pthread_attr_t *attr,
                        void *(*fn)(void *), void *arg);
int shim_pthread_join(pthread_t th, void **ret);
