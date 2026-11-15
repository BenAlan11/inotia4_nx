/* util.c -- logging, clocks and the small odds and ends every module needs */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

#include "util.h"
#include "config.h"

static FILE *log_file = NULL;
static Mutex log_mutex;
static int log_ready = 0;
static char log_dir[280] = "sdmc:/switch/inotia4";

/* the NRO can sit anywhere on the card, so main() tells us where before the
 * first debugPrintf call; the default keeps the fixed layout working */
void debugSetDir(const char *dir) {
  if (!dir || !*dir || log_ready)
    return;
  snprintf(log_dir, sizeof(log_dir), "%s", dir);
}

void debugInit(void) {
  if (log_ready)
    return;
  mutexInit(&log_mutex);
  socketInitializeDefault();
  nxlinkStdio();          /* harmless when nothing is listening */
  char path[300], prev[300];
  snprintf(path, sizeof(path), "%s/debug.log", log_dir);
  snprintf(prev, sizeof(prev), "%s/debug.prev.log", log_dir);
  /* keep the previous run: the usual sequence is "it crashed, run it again to
   * go and fetch the log", which would otherwise overwrite the evidence */
  remove(prev);
  rename(path, prev);
  log_file = fopen(path, "w");
  log_ready = 1;
  if (log_file) {
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    fprintf(log_file, "Inotia 4 (Switch port) -- %04d-%02d-%02d %02d:%02d:%02d\n",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
            tm->tm_hour, tm->tm_min, tm->tm_sec);
    fflush(log_file);
  }
}

void debugPrintf(const char *fmt, ...) {
  if (!log_ready)
    debugInit();

  char buf[1024];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof(buf), fmt, va);
  va_end(va);

  mutexLock(&log_mutex);
  if (log_file)
    fputs(buf, log_file);   /* buffered; the frame loop flushes once per frame */
  /* The nxlink console and the kernel debug string are bring-up facilities.
   * Each line used to cost two flushes plus an SVC even when nobody was
   * listening. On the console's SD card that is a synchronous write per line,
   * and during effects / blinking tips the log gets chatty enough to stutter --
   * which never showed up in Eden (host SSD, cheap SVCs). log_level >= 3 keeps
   * the old behaviour for bring-up. */
  if (cfg.log_level >= 3) {
    fputs(buf, stdout);
    fflush(stdout);
    svcOutputDebugString(buf, strlen(buf));
  }
  mutexUnlock(&log_mutex);
}

void debugFlush(void) {
  if (!log_ready)
    return;
  mutexLock(&log_mutex);
  if (log_file)
    fflush(log_file);       /* a crash now loses at most the current frame */
  mutexUnlock(&log_mutex);
}

void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int strcasecmp_compat(const char *a, const char *b) {
  while (*a && *b) {
    int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
    if (ca != cb)
      return ca - cb;
    a++, b++;
  }
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
