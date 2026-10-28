/* inotia_input.c -- Switch input turned into the touches the engine polls.
 *
 * Inotia 4 is a touch ARPG: the engine reads its input through
 * CS_knlGetTouchCount and the CUserInput callback, and there is no key path to
 * map onto. So the bridge has two sources:
 *
 *   - the touchscreen, passed through 1:1 (the handheld mode is the natural
 *     way to play this);
 *   - the controller, as a virtual finger: the left stick moves a cursor, A
 *     presses it. That is what makes docked play possible at all.
 *
 * MINUS leaves the port (a bring-up convenience; the game itself has no exit).
 *
 * MIT.
 */
#include <math.h>
#include <string.h>
#include <switch.h>

#include "inotia.h"
#include "rt_applet.h"
#include "rt_pad.h"
#include "rt_settings.h"
#include "util.h"

#define MAX_TOUCH 2
#define CURSOR_SPEED 900.0f /* pixels per second at full stick */

typedef struct {
  int down;
  int x, y;
} Touch;

static Touch g_touch[MAX_TOUCH];
static int g_count;

static PadState g_pad;
static int g_pad_ready;
static float g_cx, g_cy;
static int g_cursor_down;
static u64 g_last_tick;

void inotia_input_init(void) {
  rt_pad_setup(RT_PAD_MAX_PLAYERS, 1);
  rt_pad_slot(&g_pad, 0);
  g_pad_ready = 1;
  hidInitializeTouchScreen();
  g_cx = RT_SCREEN_W / 2.0f;
  g_cy = RT_SCREEN_H / 2.0f;
  g_last_tick = armGetSystemTick();
  debugPrintf("[input] pad + touchscreen ready, cursor at %.0f,%.0f\n", g_cx, g_cy);
}

void inotia_input_frame(void) {
  g_count = 0;

  /* 1. The touchscreen wins when it is being used. */
  HidTouchScreenState ts;
  const size_t n = hidGetTouchScreenStates(&ts, 1);
  if (n > 0 && ts.count > 0) {
    for (size_t i = 0; i < ts.count && (int)i < MAX_TOUCH; i++) {
      g_touch[g_count].down = 1;
      g_touch[g_count].x = (int)((long long)ts.touches[i].x * RT_SCREEN_W / 1280);
      g_touch[g_count].y = (int)((long long)ts.touches[i].y * RT_SCREEN_H / 720);
      g_count++;
    }
    return;
  }

  /* 2. Otherwise the controller drives the virtual finger. */
  if (!g_pad_ready)
    return;
  padUpdate(&g_pad);

  const u64 held = padGetButtons(&g_pad);
  const u64 down = padGetButtonsDown(&g_pad);

  if (down & HidNpadButton_Minus) {
    debugPrintf("[input] MINUS: leaving\n");
    rt_request_exit();
    return;
  }

  const u64 now = armGetSystemTick();
  const float dt = (float)armTicksToNs(now - g_last_tick) / 1e9f;
  g_last_tick = now;

  float sx = 0.0f, sy = 0.0f;
  const HidAnalogStickState l = padGetStickPos(&g_pad, 0);
  sx = (float)l.x / 32767.0f;
  sy = (float)l.y / 32767.0f;
  if (fabsf(sx) < 0.15f)
    sx = 0.0f;
  if (fabsf(sy) < 0.15f)
    sy = 0.0f;
  if (held & HidNpadButton_StickLLeft)
    sx = -1.0f;
  if (held & HidNpadButton_StickLRight)
    sx = 1.0f;
  if (held & HidNpadButton_StickLUp)
    sy = 1.0f;
  if (held & HidNpadButton_StickLDown)
    sy = -1.0f;
  g_cx += sx * CURSOR_SPEED * dt;
  g_cy -= sy * CURSOR_SPEED * dt;
  if (g_cx < 0)
    g_cx = 0;
  if (g_cy < 0)
    g_cy = 0;
  if (g_cx > RT_SCREEN_W - 1)
    g_cx = RT_SCREEN_W - 1;
  if (g_cy > RT_SCREEN_H - 1)
    g_cy = RT_SCREEN_H - 1;

  const int was_down = g_cursor_down;
  g_cursor_down = (held & (HidNpadButton_A | HidNpadButton_ZR)) != 0;
  if (g_cursor_down != was_down) {
    debugPrintf("[input] virtual finger %s at %d,%d\n", g_cursor_down ? "down" : "up",
                (int)g_cx, (int)g_cy);
    log_flush_ring();
  }

  g_touch[0].down = g_cursor_down;
  g_touch[0].x = (int)g_cx;
  g_touch[0].y = (int)g_cy;
  g_count = g_cursor_down ? 1 : 0;
}

int inotia_touch_count(void) { return g_count; }

int inotia_touch_x(int index) {
  return (index >= 0 && index < g_count) ? g_touch[index].x : 0;
}

int inotia_touch_y(int index) {
  return (index >= 0 && index < g_count) ? g_touch[index].y : 0;
}

int inotia_touch_down(int index) {
  return (index >= 0 && index < g_count) ? g_touch[index].down : 0;
}
