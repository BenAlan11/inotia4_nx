#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define umin(a, b) ((a) < (b) ? (a) : (b))
#define umax(a, b) ((a) > (b) ? (a) : (b))

/* debug log: goes to sdmc:/switch/inotia4/debug.log and the nxlink console */
void debugSetDir(const char *dir);
void debugInit(void);
void debugPrintf(const char *fmt, ...);
void cpu_boost(int on);

/* toupper/lower for the config parser */
int strcasecmp_compat(const char *a, const char *b);

#endif
