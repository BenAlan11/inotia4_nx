#ifndef ERROR_H
#define ERROR_H

#include <switch.h>

/* Draws a full-screen message and never returns. Used for conditions the user
 * has to fix (missing data, no syscall access, ...). */
void NX_NORETURN fatal_error(const char *fmt, ...);

#endif
