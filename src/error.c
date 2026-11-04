/* error.c -- last-resort message screen.
 *
 * A loader that dies silently is undebuggable on hardware, so every fatal path
 * funnels through here: the message goes to debug.log, to fatal.txt on the SD
 * card and to the text console, and the process then exits so Atmosphère's
 * crash reporter picks it up. */

#include <switch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "util.h"

extern void NX_NORETURN __libnx_exit(int rc);

void NX_NORETURN fatal_error(const char *fmt, ...) {
  char msg[2048];
  va_list va;
  va_start(va, fmt);
  vsnprintf(msg, sizeof(msg), fmt, va);
  va_end(va);

  debugPrintf("\n*** FATAL ***\n%s\n", msg);

  FILE *f = fopen("sdmc:/switch/inotia4/fatal.txt", "w");
  if (f) {
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
  }

  consoleInit(NULL);
  printf("\x1b[2;2HInotia 4 could not start.\n\n%s\n\nPress + to exit.", msg);
  consoleUpdate(NULL);

  PadState pad;
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
      break;
    consoleUpdate(NULL);
  }
  consoleExit(NULL);
  __libnx_exit(1);
}
