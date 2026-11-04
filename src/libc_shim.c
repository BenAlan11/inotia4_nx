/* libc_shim.c -- Android/bionic compatibility layer
 *
 * Two jobs:
 *   1. rewrite the absolute paths libgame.so was compiled against
 *      (/sdcard/Android/data/<pkg>/files/...) into the SD card layout;
 *   2. provide the bionic-only symbols listed in the import table.
 *
 * Derived in structure from NaGaa95/swordigo_nx's libc_shim.c (MIT).
 */

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>

#include "libc_shim.h"
#include "so_util.h"
#include "config.h"
#include "util.h"

#define PKG_NAME "com.com2us.inotia4.normal.freefull.google.global.android.common"

static char s_root[256];
static char s_data[256];
static char s_save[256];

void shim_set_dirs(const char *root, const char *data_dir, const char *save_dir) {
  snprintf(s_root, sizeof(s_root), "%s", root);
  snprintf(s_data, sizeof(s_data), "%s", data_dir);
  snprintf(s_save, sizeof(s_save), "%s", save_dir);
}

const char *shim_package_name(void) { return PKG_NAME; }

/* ------------------------------------------------------------------------- */
/* path translation                                                          */
/* ------------------------------------------------------------------------- */

/* Where an absolute Android path lands on the SD card.
 *
 *   /sdcard/Android/data/<pkg>/files/<rest>  -> <data>/<rest>
 *   /sdcard/Android/data/<pkg>/<rest>        -> <data>/<rest>
 *   /sdcard/<rest>                           -> <root>/<rest>
 *   /data/data/<pkg>/<rest>                  -> <root>/<rest>
 *   /data/user/0/<pkg>/<rest>                -> <root>/<rest>
 *   anything else                            -> unchanged
 */
const char *shim_path(const char *path, char *buf, size_t bufsize) {
  if (!path || !*path)
    return path;
  if (path[0] != '/') {
    /* Android gave the engine a writable working directory; Horizon has none, so
     * a bare name like "i_pack.dat" could not be created and the engine's
     * runtime files silently never appeared. Resolve those against the game
     * folder, which is where the data it belongs next to lives. */
    if (!strchr(path, ':') && buf && bufsize > 2) {
      snprintf(buf, bufsize, "%s/%s", s_root, path);
      return buf;
    }
    return path;
  }

  static const char *sdcard_data = "/sdcard/Android/data/";
  static const char *data_data   = "/data/data/";
  static const char *data_user   = "/data/user/0/";

  if (strncmp(path, sdcard_data, strlen(sdcard_data)) == 0) {
    /* skip the package directory */
    const char *p = path + strlen(sdcard_data);
    const char *slash = strchr(p, '/');
    if (!slash)
      return path;
    p = slash + 1;
    /* optional "/files" component: drop it so the tree starts at <data> */
    if (strncmp(p, "files/", 6) == 0)
      p += 6;
    else if (strncmp(p, "files", 5) == 0 && p[5] == '\0')
      p += 5;
    snprintf(buf, bufsize, "%s/%s", s_data, p);
    return buf;
  }
  /* A path that is already the card path but lost its "sdmc:" prefix. The
   * engine builds delete-time names from the Java-side user dir sometimes and
   * ends up with this form; left unmatched it fell through to the "return the
   * input untouched" branch, the caller then read its own uninitialised buffer
   * and every unlink()/remove() of a save file failed with an empty name. */
  if (strncmp(path, "/switch/", 8) == 0) {
    snprintf(buf, bufsize, "sdmc:%s", path);
    return buf;
  }
  if (strncmp(path, "/sdcard", 7) == 0) {
    const char *p = path + 7;
    while (*p == '/') p++;
    snprintf(buf, bufsize, "%s/%s", s_root, p);
    return buf;
  }
  if (strncmp(path, data_data, strlen(data_data)) == 0) {
    const char *p = path + strlen(data_data);
    const char *slash = strchr(p, '/');
    p = slash ? slash + 1 : p + strlen(p);
    snprintf(buf, bufsize, "%s/%s", s_root, p);
    return buf;
  }
  if (strncmp(path, data_user, strlen(data_user)) == 0) {
    const char *p = path + strlen(data_user);
    const char *slash = strchr(p, '/');
    p = slash ? slash + 1 : p + strlen(p);
    snprintf(buf, bufsize, "%s/%s", s_root, p);
    return buf;
  }
  return path;
}

/* The APK keeps its data under resources/common/game_res while the engine may
 * ask for resources/<language>/game_res first. Rewrite the miss instead of
 * duplicating 38 MB of assets per language. */
int shim_path_resolve(const char *path, char *buf, size_t bufsize) {
  const char *p = shim_path(path, buf, bufsize);

  /* shim_path() hands back either its own buffer or the caller's pointer, and
   * every call site below then opens "buf". When the input was already a device
   * path ("sdmc:/..."), buf was left untouched and the open used uninitialised
   * stack -- which showed up as opens of "P5<garbage>" and of the empty string,
   * and silently broke every file the engine writes at run time. Always
   * materialise the result in buf. */
  if (p != buf) {
    if (!p || !*p) {
      if (buf && bufsize)
        buf[0] = '\0';
      return 0;
    }
    snprintf(buf, bufsize, "%s", p);
    p = buf;
  }

  struct stat st;
  if (stat(p, &st) == 0)
    return 1;

  char alt[512];
  const char *res = strstr(p, "/resources/");
  if (res) {
    const size_t head = (size_t)(res - p);
    if (head + 64 < sizeof(alt)) {
      memcpy(alt, p, head);
      snprintf(alt + head, sizeof(alt) - head, "/resources/common%s",
               res + strlen("/resources/") + strcspn(res + strlen("/resources/"), "/"));
      if (stat(alt, &st) == 0) {
        snprintf(buf, bufsize, "%s", alt);
        return 1;
      }
    }
  }
  return 0;
}

static const char *rewrite(const char *path, char *buf, size_t n) {
  return shim_path(path, buf, n);
}

/* ------------------------------------------------------------------------- */
/* file API                                                                  */
/* ------------------------------------------------------------------------- */

/* create every missing component of a directory path (fopen does not) */
static void mkdir_p(const char *path) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 1; *p; p++) {
    if (*p != '/')
      continue;
    *p = '\0';
    mkdir(tmp, 0777);
    *p = '/';
  }
  mkdir(tmp, 0777);
}

/* Android gave the engine a writable per-package directory that already
 * existed; on the card it does not, so the first write into it fails with
 * ENOENT. Creating the parent on demand is what makes the runtime files
 * (i_pack.dat, appinfo.dat, the save slots) appear. */
static void ensure_parent(const char *path) {
  char dir[512];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (!slash)
    return;
  *slash = '\0';
  mkdir_p(dir);
}

FILE *shim_fopen(const char *path, const char *mode) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  FILE *f = fopen(buf, mode);
  if (!f && mode && strpbrk(mode, "wa+")) {
    ensure_parent(buf);
    f = fopen(buf, mode);
  }
  if (cfg.log_level >= 2)
    debugPrintf("fs: fopen(%s) %s -> %p\n", buf, mode, (void *)f);
  return f;
}

int shim_open(const char *path, int flags, ...) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  mode_t mode = 0;
  if (flags & O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = (mode_t)va_arg(va, int);
    va_end(va);
  }
  int fd = open(buf, flags, mode);

  /* On Linux open(dir, O_RDONLY) succeeds, which is how the engine asks "does
   * this directory exist" while it climbs up looking for its data root. Horizon
   * refuses to open a directory, so every probe failed and the engine concluded
   * it had no data -- it then loaded no resources at all and drew a black
   * screen. Hand back a real descriptor on a harmless file so the answer
   * matches what Android would have said. */
  if (fd < 0 && (flags & O_CREAT) && buf[0]) {
    ensure_parent(buf);
    fd = open(buf, flags, mode);
  }

  if (fd < 0 && !(flags & O_CREAT) && buf[0]) {
    struct stat st;
    char probe[300];
    snprintf(probe, sizeof(probe), "%s/..dir.stub", g_root);
    if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) {
      fd = open(probe, O_RDONLY);
      if (fd < 0) {
        fd = open(probe, O_CREAT | O_RDONLY, 0666);
        if (fd >= 0)
          close(fd), fd = open(probe, O_RDONLY);
      }
      if (cfg.log_level >= 2)
        debugPrintf("fs: open(%s) is a directory, faked as fd %d\n", buf, fd);
    }
  }

  if (cfg.log_level >= 2 || !buf[0])
    debugPrintf("fs: open(%s, 0x%x) -> %d%s from %p\n", buf, flags, fd,
                fd < 0 ? " (failed)" : "", __builtin_return_address(0));
  return fd;
}

/* Android's bionic and newlib lay out struct stat differently, and the engine
 * reads st_size at the bionic offset. Handing it newlib's struct made every file
 * report zero bytes, which is why the resource layer refused to load anything
 * and RES_LoadToPool walked into its broken error path. */
static void to_bionic_stat(void *out, const struct stat *nst) {
  uint8_t *p = (uint8_t *)out;
  memset(p, 0, 128);
  *(uint64_t *)(p + 0)   = (uint64_t)nst->st_dev;
  *(uint64_t *)(p + 8)   = (uint64_t)nst->st_ino;
  *(uint32_t *)(p + 16)  = (uint32_t)nst->st_mode;
  *(uint32_t *)(p + 20)  = (uint32_t)nst->st_nlink;
  *(uint32_t *)(p + 24)  = (uint32_t)nst->st_uid;
  *(uint32_t *)(p + 28)  = (uint32_t)nst->st_gid;
  *(uint64_t *)(p + 32)  = (uint64_t)nst->st_rdev;
  *(int64_t  *)(p + 48)  = (int64_t)nst->st_size;
  *(int32_t  *)(p + 56)  = 4096;              /* st_blksize */
  *(int64_t  *)(p + 72)  = (int64_t)nst->st_atime;
  *(int64_t  *)(p + 88)  = (int64_t)nst->st_mtime;
  *(int64_t  *)(p + 104) = (int64_t)nst->st_ctime;
}

int shim_stat(const char *path, void *out) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  struct stat nst;
  const int rc = stat(buf, &nst);
  if (rc != 0)
    return rc;
  to_bionic_stat(out, &nst);
  if (cfg.log_level >= 2)
    debugPrintf("fs: stat(%s) size=%ld mode=0%o\n", buf, (long)nst.st_size,
                (unsigned)nst.st_mode);
  return 0;
}

int shim_fstat(int fd, void *out) {
  struct stat nst;
  const int rc = fstat(fd, &nst);
  if (rc != 0)
    return rc;
  to_bionic_stat(out, &nst);
  return 0;
}

int shim_lstat(const char *path, void *out) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  struct stat nst;
  const int rc = stat(buf, &nst);   /* no symlinks on FAT */
  if (rc != 0)
    return rc;
  to_bionic_stat(out, &nst);
  return 0;
}

int shim_access(const char *path, int mode) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  return access(buf, mode);
}

int shim_mkdir(const char *path, mode_t mode) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  return mkdir(buf, mode);
}

int shim_unlink(const char *path) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  const int rc = unlink(buf);
  if (cfg.log_level >= 2)
    debugPrintf("fs: unlink(%s) -> %d\n", buf, rc);
  return rc;
}

int shim_remove(const char *path) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  const int rc = remove(buf);
  if (cfg.log_level >= 2)
    debugPrintf("fs: remove(%s) -> %d\n", buf, rc);
  return rc;
}

int shim_rmdir(const char *path) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  const int rc = rmdir(buf);
  if (cfg.log_level >= 2)
    debugPrintf("fs: rmdir(%s) -> %d\n", buf, rc);
  return rc;
}

int shim_rename(const char *from, const char *to) {
  char a[512], b[512];
  shim_path_resolve(from, a, sizeof(a));
  shim_path_resolve(to, b, sizeof(b));
  const int rc = rename(a, b);
  if (cfg.log_level >= 2)
    debugPrintf("fs: rename(%s -> %s) -> %d\n", a, b, rc);
  return rc;
}

void *shim_opendir(const char *path) {
  char buf[512];
  shim_path_resolve(path, buf, sizeof(buf));
  return opendir(buf);
}

/* ------------------------------------------------------------------------- */
/* stdio intercept: bionic hands us &__sF[i] where it means stdout/stderr     */
/* ------------------------------------------------------------------------- */

/* bionic's FILE is 0xE8 bytes on arm64; we only ever need the address range */
/* exported: libgame.so relocates &__sF[i] directly */
uint8_t __sF_storage[3][0xE8] __attribute__((aligned(8)));

void *shim_sF_base(void) { return __sF_storage; }
size_t shim_sF_size(void) { return sizeof(__sF_storage); }

static FILE *fix_file(FILE *f) {
  const uintptr_t p = (uintptr_t)f;
  const uintptr_t base = (uintptr_t)__sF_storage;
  if (p >= base && p < base + sizeof(__sF_storage)) {
    const size_t idx = (p - base) / 0xE8;
    return idx == 0 ? stdin : (idx == 1 ? stdout : stderr);
  }
  return f;
}

int shim_fclose(FILE *f) { return fclose(fix_file(f)); }
size_t shim_fread(void *p, size_t sz, size_t n, FILE *f) { return fread(p, sz, n, fix_file(f)); }
size_t shim_fwrite(const void *p, size_t sz, size_t n, FILE *f) { return fwrite(p, sz, n, fix_file(f)); }
int shim_fflush(FILE *f) { return fflush(f ? fix_file(f) : NULL); }
int shim_fputc(int c, FILE *f) { return fputc(c, fix_file(f)); }
int shim_fgetc(FILE *f) { return fgetc(fix_file(f)); }
char *shim_fgets(char *s, int n, FILE *f) { return fgets(s, n, fix_file(f)); }
int shim_feof(FILE *f) { return feof(fix_file(f)); }

int shim_fprintf(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int r = vfprintf(fix_file(f), fmt, va);
  va_end(va);
  return r;
}

int shim_vfprintf(FILE *f, const char *fmt, va_list va) {
  return vfprintf(fix_file(f), fmt, va);
}

/* ------------------------------------------------------------------------- */
/* bionic extras                                                             */
/* ------------------------------------------------------------------------- */

int shim_getpagesize(void) { return 0x1000; }
int shim_getpid(void) { return 1; }
int shim_kill(int pid, int sig) { (void)pid; (void)sig; return 0; }
int shim_dup(int fd) { return fd; }

int shim_fcntl(int fd, int cmd, ...) {
  va_list va;
  va_start(va, cmd);
  int arg = va_arg(va, int);
  va_end(va);
  if (cmd == F_GETFL)
    return O_RDWR;
  (void)arg;
  return 0;
}

/* The engine only mmaps asset blobs read-only; a zeroed aligned allocation is
 * equivalent for that use and avoids needing a real VM. MAP_FAILED on error. */
void *shim_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
  (void)addr; (void)prot;
  const size_t rounded = ALIGN_MEM(len, 0x1000);
  void *p = memalign(0x1000, rounded);
  if (!p) {
    if (cfg.log_level >= 2)
      debugPrintf("fs: mmap(len=%u) failed\n", (unsigned)len);
    return (void *)(intptr_t)-1;
  }
  memset(p, 0, rounded);

  /* This used to be a zeroed buffer, and that silently broke every resource the
   * engine memory-maps: it parsed headers out of all-zero pages, decided the
   * file was not a valid package, and the resource layer then reported failure.
   * A real mmap hands back the file's bytes, so read them in. */
  if (fd >= 0) {
    const long save = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, off, SEEK_SET) != (long)-1) {
      size_t done = 0;
      while (done < len) {
        const ssize_t n = read(fd, (char *)p + done, len - done);
        if (n <= 0)
          break;
        done += (size_t)n;
      }
      if (cfg.log_level >= 2)
        debugPrintf("fs: mmap(len=%u, fd=%d, off=%ld) -> %p, %u bytes read\n",
                    (unsigned)len, fd, off, p, (unsigned)done);
    }
    if (save >= 0)
      lseek(fd, save, SEEK_SET);
  } else if (cfg.log_level >= 2) {
    debugPrintf("fs: mmap(len=%u, anon) -> %p\n", (unsigned)len, p);
  }
  return p;
}

int *__errno_shim(void) { return &errno; }

/* bionic's fortified FD_SET/FD_ISSET wrappers just add a size check */
int shim_fd_set_chk(int fd, void *set, unsigned long size) {
  (void)size;
  if (fd < 0 || fd >= 1024) return -1;
  ((unsigned long *)set)[fd >> 6] |= 1ul << (fd & 63);
  return 0;
}

int shim_fd_isset_chk(int fd, void *set, unsigned long size) {
  (void)size;
  if (fd < 0 || fd >= 1024) return 0;
  return ((((unsigned long *)set)[fd >> 6] >> (fd & 63)) & 1ul) != 0;
}

void shim_stack_chk_fail(void) {
  debugPrintf("*** stack smashing detected in game code ***\n");
  abort();
}

int shim_munmap(void *addr, size_t len) {
  (void)len;
  free(addr);
  return 0;
}

/* the engine probes free space on the "sdcard"; report the real SD numbers */
int shim_statfs(const char *path, void *buf) {
  char tmp[512];
  struct statvfs vfs;
  const char *p = rewrite(path, tmp, sizeof(tmp));
  if (statvfs(p, &vfs) != 0)
    return -1;
  /* bionic's struct statfs and newlib's differ; fill the leading, common part:
   * f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree ... */
  uint32_t *out = (uint32_t *)buf;
  memset(buf, 0, 64);
  out[0] = 0x01021994;             /* TMPFS_MAGIC: anything non-zero works */
  out[1] = (uint32_t)vfs.f_bsize;
  out[2] = (uint32_t)(vfs.f_blocks & 0xffffffff);
  out[3] = (uint32_t)(vfs.f_bfree & 0xffffffff);
  out[4] = (uint32_t)(vfs.f_bavail & 0xffffffff);
  return 0;
}

void *shim_popen(const char *cmd, const char *mode) {
  debugPrintf("shim: popen(%s) -> NULL\n", cmd ? cmd : "(null)");
  (void)mode;
  return NULL;
}

int shim_pclose(void *f) { (void)f; return -1; }

int shim_system(const char *cmd) {
  if (!cmd)
    return -1;

  /* The anti-tamper layer runs `ps > <dir>/temp` and scans the process list for
   * known cheat apps. On a console there is no ps and no shell, so the command
   * failed, the file was never written, and the check reported "cannot verify"
   * -- which is the dialog-and-exit path. Producing the listing ourselves makes
   * that check answer the same way it would on a clean Android device. */
  if (cmd[0] == 'p' && cmd[1] == 's') {
    const char *gt = strchr(cmd, '>');
    if (!gt) {
      debugPrintf("shim: system(%s) -> 0 (no redirect)\n", cmd);
      return 0;
    }
    const char *from = gt + 1;
    while (*from == ' ')
      from++;
    char raw[300];
    snprintf(raw, sizeof(raw), "%s", from);
    for (int i = (int)strlen(raw) - 1; i >= 0 && (raw[i] == ' ' || raw[i] == '\n'); i--)
      raw[i] = '\0';

    /* shim_path returns either its buffer or the input unchanged, so take the
     * return value: a path that is already device-absolute is passed through. */
    char pathbuf[512];
    const char *path = shim_path(raw, pathbuf, sizeof(pathbuf));
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) {
      *slash = '\0';
      mkdir_p(dir);          /* the engine never created its own temp folder */
    }
    FILE *f = fopen(path, "w");
    if (!f) {
      debugPrintf("shim: system(%s) -> could not write %s\n", cmd, path);
      return -1;
    }
    /* only the console's own process: nothing in the list can match a cheat app */
    fprintf(f, "USER     PID  PPID VSZ      RSS   WCHAN    ADDR S NAME\n");
    fprintf(f, "root       1     0 12345    1234  0        0    S init\n");
    fprintf(f, "u0_a64  1201     1 1234567  145678 0       0    S %s\n",
            "com.com2us.inotia4.normal.freefull.google.global.android.common");
    fclose(f);
    debugPrintf("shim: system(%s) -> 0 (emulated process list)\n", cmd);
    return 0;
  }

  debugPrintf("shim: system(%s) -> -1\n", cmd);
  return -1;
}

void shim_sincos(double x, double *s, double *c) { *s = sin(x); *c = cos(x); }
void shim_sincosf(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }

/* AT_HWCAP etc. Report a plain ARMv8-A feature set: the engine only uses this
 * to pick NEON code paths it already assumes are present elsewhere. */
int shim_getauxval(unsigned long type) {
  switch (type) {
    case 16: return 0x00000000;          /* AT_HWCAP  */
    case 26: return 0x00000000;          /* AT_HWCAP2 */
    case 6:  return 0x1000;              /* AT_PAGESZ */
    case 9:  return 0;                   /* AT_ENTRY  */
    default: return 0;
  }
}

/* Enough of ro.product.* for the engine's device detection. Reporting a plain
 * 2013-era flagship keeps it on the affordable-quality code path. */
int shim_system_property_get(const char *name, char *value) {
  struct { const char *k, *v; } props[] = {
    {"ro.product.model",              "Nintendo Switch"},
    {"ro.product.brand",              "Nintendo"},
    {"ro.product.manufacturer",       "Nintendo"},
    {"ro.product.device",             "nx"},
    {"ro.build.version.sdk",          "19"},
    {"ro.build.version.release",      "4.4.2"},
    {"ro.build.version.incremental",  "000000"},
    {"ro.board.platform",             "tegra"},
    {"ro.hardware",                   "nx"},
    {"ro.sf.lcd_density",             "320"},
    {"ro.opengles.version",           "131072"},
    {"ro.kernel.qemu",                "0"},
    {"gsm.version.baseband",          "unknown"},
    {"net.hostname",                  "switch"},
    {"java.vm.version",               "2.1.0"},
    {"ro.debuggable",                 "0"},
  };
  for (unsigned i = 0; i < sizeof(props) / sizeof(*props); i++) {
    if (strcmp(name, props[i].k) == 0) {
      size_t n = strlen(props[i].v);
      if (value) { memcpy(value, props[i].v, n + 1); }
      return (int)n;
    }
  }
  if (value)
    value[0] = '\0';
  return 0;
}

void shim_android_log_print(int prio, const char *tag, const char *fmt, ...) {
  char msg[1024];
  va_list va;
  va_start(va, fmt);
  vsnprintf(msg, sizeof(msg), fmt, va);
  va_end(va);
  (void)prio;
  debugPrintf("[%s] %s\n", tag ? tag : "?", msg);
}

void shim_openlog(const char *ident, int option, int facility) {
  (void)ident; (void)option; (void)facility;
}

void shim_syslog(int prio, const char *fmt, ...) {
  (void)prio;
  char msg[512];
  va_list va;
  va_start(va, fmt);
  vsnprintf(msg, sizeof(msg), fmt, va);
  va_end(va);
  debugPrintf("[syslog] %s\n", msg);
}

void shim_closelog(void) {}

void shim_android_set_abort_message(const char *msg) {
  debugPrintf("android_set_abort_message: %s\n", msg ? msg : "(null)");
}

/* __cxa_atexit: run destructors at exit. Registration only; the port never
 * unloads, so the list is drained by a single call at shutdown if at all. */
#define MAX_ATEXIT 128
static struct { void (*fn)(void *); void *arg; void *dso; } atexit_list[MAX_ATEXIT];
static int atexit_count = 0;

int shim_cxa_atexit(void (*fn)(void *), void *arg, void *dso) {
  if (atexit_count >= MAX_ATEXIT)
    return -1;
  atexit_list[atexit_count].fn = fn;
  atexit_list[atexit_count].arg = arg;
  atexit_list[atexit_count].dso = dso;
  atexit_count++;
  return 0;
}

void shim_cxa_finalize(void *dso) {
  for (int i = atexit_count - 1; i >= 0; i--) {
    if (dso && atexit_list[i].dso != dso)
      continue;
    if (atexit_list[i].fn)
      atexit_list[i].fn(atexit_list[i].arg);
    atexit_list[i].fn = NULL;
  }
}

int shim_dl_iterate_phdr(int (*cb)(void *, size_t, void *), void *data) {
  return so_dl_iterate_phdr(cb, data);
}

uint32_t shim_inet_addr(const char *cp) {
  uint32_t parts[4] = {0};
  int n = 0;
  const char *p = cp;
  while (n < 4 && p && *p) {
    parts[n++] = (uint32_t)strtoul(p, (char **)&p, 10);
    if (*p == '.') p++; else break;
  }
  if (n != 4) return 0xffffffffu;  /* INADDR_NONE */
  return parts[0] | (parts[1] << 8) | (parts[2] << 16) | (parts[3] << 24);
}

char *shim_inet_ntoa(uint32_t in) {
  static char buf[16];
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
           in & 0xff, (in >> 8) & 0xff, (in >> 16) & 0xff, (in >> 24) & 0xff);
  return buf;
}

/* newlib ships memalign/valloc but not the POSIX spelling; harfbuzz and the
   C++ runtime both pull it in. */
int posix_memalign(void **out, size_t align, size_t size) {
  void *p;
  if (align < sizeof(void *) || (align & (align - 1)) != 0)
    return EINVAL;
  p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

/* ---- stall forensics ---------------------------------------------------- */

/* The engine has no pthread imports, so if it ever blocks it is most likely
 * spinning on one of these. Throttled logging turns "it hangs in
 * CWrapperData.nativeInitialize" into "it is calling usleep 200k times". */
static void note_time_call(const char *what, long arg) {
  static u64 last;
  static int calls;
  const u64 now = armGetSystemTick();
  calls++;
  if (now - last < armGetSystemTickFreq())     /* once a second */
    return;
  debugPrintf("time: %s(%ld) x%d\n", what, arg, calls);
  last = now;
  calls = 0;
}

int shim_usleep(unsigned int usec) {
  note_time_call("usleep", (long)usec);
  return usleep(usec);
}

int shim_nanosleep(const struct timespec *req, struct timespec *rem) {
  note_time_call("nanosleep", req ? (long)req->tv_nsec : -1);
  return nanosleep(req, rem);
}

clock_t shim_clock(void) {
  note_time_call("clock", 0);
  return clock();
}

int shim_gettimeofday(struct timeval *tv, void *tz) {
  static int n;
  if (n < 6)
    debugPrintf("time: gettimeofday #%d enter\n", n);
  const int rc = gettimeofday(tv, tz);
  if (n < 6) {
    debugPrintf("time: gettimeofday #%d left rc=%d tv=%ld.%06ld\n", n, rc,
                tv ? (long)tv->tv_sec : -1L, tv ? (long)tv->tv_usec : -1L);
    n++;
  }
  return rc;
}

/* ---- fortify wrappers with a different arity ---------------------------- */

/* __vsprintf_chk(dest, flags, destlen, fmt, ap) and
 * __vsnprintf_chk(dest, flags, destlen, maxlen, fmt, ap) cannot be aliased onto
 * the plain functions: the extra arguments shift fmt/ap by one and two slots.
 * The engine logs through these the moment it starts, and an aliased version
 * hands vsprintf a NULL va_list -- a data abort with no useful trace. */
int shim_vsprintf_chk(char *dest, int flags, size_t destlen, const char *fmt,
                      va_list ap) {
  (void)flags; (void)destlen;
  return vsprintf(dest, fmt, ap);
}

int shim_vsnprintf_chk(char *dest, int flags, size_t destlen, size_t maxlen,
                       const char *fmt, va_list ap) {
  (void)flags; (void)destlen;
  return vsnprintf(dest, maxlen, fmt, ap);
}


/* ---- allocation forensics ----------------------------------------------- */

/* A large block asked for once is normal (textures, the texture store); the same
 * large block asked for repeatedly is a leak or a retry loop. The engine allocates
 * through our import table, so wrapping the four allocators names the caller. */
static int alloc_log_budget = 60;
unsigned long g_alloc_calls, g_alloc_kb;
unsigned long alloc_calls(void) { return g_alloc_calls; }

/* strdup/vasprintf go through libc internally, so the import wrapper on malloc
 * never sees them; wrap the entry points the engine actually uses */
char *shim_strdup(const char *src) {
  char *p = strdup(src);
  if (p) { g_alloc_calls++; g_alloc_kb += (unsigned long)(strlen(src) >> 10); }
  return p;
}
void *shim_memalign(size_t align, size_t size) {
  g_alloc_calls++; g_alloc_kb += (unsigned long)(size >> 10);
  return memalign(align, size);
}
int shim_vasprintf(char **out, const char *fmt, va_list ap) {
  const int rc = vasprintf(out, fmt, ap);
  if (rc > 0) { g_alloc_calls++; g_alloc_kb += (unsigned long)rc >> 10; }
  return rc;
}
unsigned long alloc_kb(void) { return g_alloc_kb; }

void *shim_malloc(size_t size) {
  g_alloc_calls++; g_alloc_kb += (unsigned long)(size >> 10);
  void *p = malloc(size);
  if (size >= (1u << 20) && alloc_log_budget > 0) {
    alloc_log_budget--;
    debugPrintf("alloc: malloc(%u KB) -> %p from %p\n", (unsigned)(size / 1024), p,
                __builtin_return_address(0));
  }
  return p;
}

void *shim_calloc(size_t n, size_t size) {
  g_alloc_calls++; g_alloc_kb += (unsigned long)((n * size) >> 10);
  void *p = calloc(n, size);
  if (n * size >= (1u << 20) && alloc_log_budget > 0) {
    alloc_log_budget--;
    debugPrintf("alloc: calloc(%u x %u) -> %p from %p\n", (unsigned)n,
                (unsigned)size, p, __builtin_return_address(0));
  }
  return p;
}

void *shim_realloc(void *old, size_t size) {
  g_alloc_calls++; g_alloc_kb += (unsigned long)(size >> 10);
  void *p = realloc(old, size);
  if (size >= (1u << 20) && alloc_log_budget > 0) {
    alloc_log_budget--;
    debugPrintf("alloc: realloc(%p, %u KB) -> %p from %p\n", old,
                (unsigned)(size / 1024), p, __builtin_return_address(0));
  }
  return p;
}


/* ---- thread creation ---------------------------------------------------- */

/* The engine's loader runs on its own thread; if pthread_create cannot start one
 * under Horizon the loading bar never advances and the frame loop just idles. */
int shim_pthread_create(pthread_t *th, const pthread_attr_t *attr,
                        void *(*fn)(void *), void *arg) {
  const int rc = pthread_create(th, attr, fn, arg);
  debugPrintf("thread: create(%p) -> %d%s\n", (void *)fn, rc,
              rc ? " (FAILED)" : "");
  return rc;
}

int shim_pthread_join(pthread_t th, void **ret) {
  const int rc = pthread_join(th, ret);
  debugPrintf("thread: join -> %d\n", rc);
  return rc;
}

/* ---- outbound connections -------------------------------------------------

 * On Android, connecting to a server with no route fails immediately
 * (ENETUNREACH), and the engine has a clean "we are offline" branch for that.
 * Horizon's socket layer instead blocks until a long timeout, on the frame
 * thread -- which is the "the page is up but nothing answers, fps drops to 0"
 * hang seen after leaving the save-slot screen (both its buttons start a
 * network check).
 *
 * Only after the menu is up, and only connect(): refusing socket() during boot
 * puts the engine into a retry loop and it never renders a frame. */
extern int g_menu_ready;
static int net_refusals;

int shim_connect(int fd, const struct sockaddr *addr, unsigned len) {
  if (g_menu_ready) {
    if (net_refusals < 8) {
      net_refusals++;
      debugPrintf("net: connect() refused (offline) family %d\n",
                  addr ? (int)addr->sa_family : -1);
    }
    errno = ENETUNREACH;
    return -1;
  }
  return connect(fd, addr, len);
}
