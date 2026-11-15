#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define ALIGN_MEM(x, align) (((x) + ((align) - 1)) & ~((align) - 1))
#define umin(a, b) ((a) < (b) ? (a) : (b))
#define umax(a, b) ((a) > (b) ? (a) : (b))

/* debug log: goes to sdmc:/switch/inotia4/debug.log and the nxlink console.
 * The format attribute is not cosmetic: a mismatched %s in a rarely-taken
 * debug probe once turned into strlen() on a colour float and killed the game
 * on hardware (Atmosphere 2168-0002, 2026-11-14). Let the compiler audit it. */
void debugSetDir(const char *dir);
void debugInit(void);
void debugPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* debugPrintf buffers its file writes; the frame loop calls this once per
 * frame so a crash loses at most one frame of log instead of costing a
 * synchronous SD-card write per line. */
void debugFlush(void);
void cpu_boost(int on);

/* toupper/lower for the config parser */
int strcasecmp_compat(const char *a, const char *b);

#endif
