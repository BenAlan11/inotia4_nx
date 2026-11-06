/* input.c -- Switch pad + touchscreen -> the engine's pointer queue
 *
 * How a touch reaches the game, recovered from libgame.so (tools/disas.py):
 *
 *   handleCletEvent_MV(action, x, y, id, pointerCount)
 *       w0 = 1 press, 2 move, 3 release
 *       w1 = x, w2 = y   (packed into ((x << 16) | y) inside)
 *       w3 = pointer id
 *       w4 = pointerCount, which it stores at 0x6e456c -- the same global that
 *            jGetCurrentTouchCount() reads back to the game
 *       then tail-calls handleCletEvent(code, ...) which queues 0x17 press,
 *       0x18 release, 0x19 move for a single pointer.
 *
 * That is the engine's own MotionEvent translator, so driving it produces
 * exactly the events Android would have produced. Driving InputQueue_Put
 * directly instead means guessing the queue payload layout, which does not
 * match anything observable.
 *
 * Because this game is touch-only, the pad drives a virtual finger: the stick
 * moves a crosshair, A taps where it is, and a real finger on the panel is
 * forwarded straight through.
 */

#include <switch.h>

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "input.h"
#include "config.h"
#include "util.h"
#include "so_util.h"

/* the engine's action codes */
#define EV_DOWN 1
#define EV_MOVE 2
#define EV_UP   3

/* pointer ids: the pad's virtual finger is 0, real fingers start at 100 */
#define ID_PAD 0

typedef void (*fn_motion)(int action, int x, int y, int id, int count);

static fn_motion p_motion;
static int (*p_sgl_w)(void);
static int (*p_sgl_h)(void);
static unsigned char *g_mod_base;   /* libgame's load_virtbase */
int input_engine_touch_count(void);

/* Focus points: the game is touch-only, so the pad walks a small set of likely
 * targets instead of making the player aim. Left/right cycles, A taps where the
 * box is. The right stick escapes to a free cursor for anything not listed. */
/* Focus targets, as percentages of the view (measured off the title screen).
 * SETTINGS and MORE GAMES are small pills; START is a bigger button sitting
 * lower, so each one carries its own size for the highlight box. */
/* measured off a 1920x1080 screenshot: the two pills are 432x75 at y 864..939,
 * START is 510x81 at y 879..960, i.e. slightly larger and slightly lower */
static struct { float xp, yp, wp, hp; } focus[12] = {
  { 25.0f, 83.5f, 22.0f, 7.0f },    /* SETTINGS   */
  { 50.0f, 85.1f, 27.0f, 8.0f },    /* START      */
  { 73.8f, 83.5f, 20.0f, 7.0f },    /* MORE GAMES */
};
static int focus_count = 3;
static int focus_idx;
static int free_mode;      /* 0 = cycling the box, 1 = free cursor */
static int stick_latch;    /* stick is already counted as one step */

static InputConfig iconf;
static int scr_w = 1280, scr_h = 720;
static PadState pad;
static u64 pad_prev;

/* the virtual finger the pad drives */
static float cur_x, cur_y;
static int cur_down;
static int last_touch_id = -1;

/* held state of the real panel, so a lift is generated exactly once */
static u32 prev_fingers;

/* The engine's touch input space is 480x320, not the screen. handleCletEvent
 * multiplies what it receives by SGL_GetScreenWidth()/480 and
 * SGL_GetScreenHeight()/320 to get its own viewport coordinates, so feeding it
 * screen pixels puts every press far outside the view -- which is why a tap that
 * reached the engine still did nothing. Converting here keeps everything else
 * (focus points, screenshots, the overlay) in plain screen coordinates. */
#define INPUT_SPACE_W 480.0f
#define INPUT_SPACE_H 320.0f

/* CONFIRMED by disassembly of handleCletEvent (0x16958c): it unpacks the
 * packed coordinate, multiplies by SGL_GetScreenWidth/Height and divides by
 * 480/320 (magic-number division), so the injector MUST keep feeding the
 * 480x320 space -- handleCletEvent_MV takes care of nothing. */
static void emit(int action, int id, float x, float y, int count) {
  if (!p_motion)
    return;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x > scr_w - 1) x = scr_w - 1;
  if (y > scr_h - 1) y = scr_h - 1;

  const int ix = (int)(x * INPUT_SPACE_W / (float)scr_w);
  const int iy = (int)(y * INPUT_SPACE_H / (float)scr_h);
  p_motion(action, ix, iy, id, count);
}

/* the engine keeps the live touch count at module offset 0x6e456c; reading it
 * back is the only proof that a synthetic event was accepted */
int input_engine_screen_w(void) { return p_sgl_w ? p_sgl_w() : -1; }
unsigned char *input_engine_virtbase(void) { return g_mod_base; }
int input_engine_screen_h(void) { return p_sgl_h ? p_sgl_h() : -1; }

int input_engine_touch_count(void) {
  if (!g_mod_base)
    return -1;
  return *(volatile int *)(g_mod_base + 0x6e456c);
}

void input_init(int screen_w, int screen_h, so_module *mod) {
  (void)mod;
  scr_w = screen_w;
  scr_h = screen_h;
  cur_x = scr_w * 0.5f;
  cur_y = scr_h * 0.5f;

  iconf.inject = 1;
  iconf.touch_x_pct = 12;
  iconf.touch_y_pct = 78;
  iconf.button_pct = 18;

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  debugPrintf("input: pad ready (%dx%d)\n", scr_w, scr_h);
}

/* called from main once libgame.so is loaded and its symbols are resolvable */
void input_bind(so_module *mod) {
  debugPrintf("input: looking up handleCletEvent_MV\n");
  p_motion = (fn_motion)so_try_find_addr_rx(mod, "handleCletEvent_MV");
  p_sgl_w = (int (*)(void))so_try_find_addr_rx(mod, "SGL_GetScreenWidth");
  p_sgl_h = (int (*)(void))so_try_find_addr_rx(mod, "SGL_GetScreenHeight");
  debugPrintf("input: handleCletEvent_MV = %p\n", (void *)p_motion);
}

/* Must not run before so_finalize(): load_virtbase is only the address the
 * module will be mapped at, and until the mapping exists any read through it is
 * a data abort. On hardware that killed the port right after "looking up
 * handleCletEvent_MV". */
void input_set_base(void *virtbase) {
  g_mod_base = (unsigned char *)virtbase;
  /* logged in two steps on purpose: this is the first read through the freshly
   * mapped module, so if it is going to fault the log must show which line did
   * it rather than dying while building the arguments */
  debugPrintf("input: probing engine globals at %p + 0x6e456c\n", virtbase);
  const int count = input_engine_touch_count();
  debugPrintf("input: engine globals readable, touch count %d\n", count);
  debugPrintf("input: view is %dx%d, engine thinks %dx%d\n", scr_w, scr_h,
              input_engine_screen_w(), input_engine_screen_h());
}

void input_configure(const InputConfig *cfg) {
  if (cfg)
    iconf = *cfg;
  if (!p_motion) {
    debugPrintf("input: no handleCletEvent_MV, injection stays off\n");
    iconf.inject = 0;
  }
}

int input_injection_active(void) { return iconf.inject; }

/* Raw drag tracking for pages whose own scroll control never sees the input in
 * this port (the skill description). The caller consumes the accumulated
 * vertical travel once a frame; the sign is "screen pixels moved downwards". */
static int drag_on;
static float drag_last_y;
static float drag_acc;
static float drag_start_x, drag_start_y;

int input_take_drag_dy(void) {
  const int v = (int)drag_acc;
  drag_acc -= (float)v;
  return v;
}

int input_drag_start(float *x, float *y) {
  if (!drag_on)
    return 0;
  if (x)
    *x = drag_start_x;
  if (y)
    *y = drag_start_y;
  return 1;
}

void input_touch(int action, int id, float x, float y) {
  if (action < 0 || action > 3 || !iconf.inject)
    return;
  {   /* prove real-finger taps reach the injector on hardware: the console log
       * must show down/up pairs with a sane engine touch count, otherwise the
       * screen is being eaten by an overlay control rather than by us */
    static int n;
    if (n < 40) {
      n++;
      debugPrintf("input: touch a%d id%d (%.0f,%.0f) engine count %d\n", action,
                  id, x, y, input_engine_touch_count());
    }
  }

  if (action == 0) {
    drag_on = 1;
    drag_last_y = y;
    drag_start_x = x;
    drag_start_y = y;
    drag_acc = 0;
  } else if (action == 1) {
    if (drag_on)
      drag_acc += y - drag_last_y;
    drag_last_y = y;
  } else {
    drag_on = 0;
  }

  const int ev = (action == 0) ? EV_DOWN : (action == 1) ? EV_MOVE : EV_UP;
  if (ev != EV_UP) {
    emit(ev, id, x, y, 1);
    return;
  }

  /* The release needs pointerCount 1, not 0. handleCletEvent_MV turns
   * (action 3, count 1) into the single-pointer release code 0x18, but
   * (action 3, count 0) into 0x80000006 -- the "secondary pointer went away"
   * form. The scene understands the press and then never sees a matching
   * release, so a tap never completes. Send the 0x18 form, then repeat with
   * count 0 so the engine's live touch count drops back to zero. */
  emit(EV_UP, id, x, y, 1);
  emit(EV_UP, id, x, y, 0);
}

/* A press and a release in the same frame are invisible to the game: the
 * engine reads the pointer state once per nativeProcess, so the finger has to
 * stay down for a couple of frames or it never sees a tap. */
static float tap_x, tap_y;
static int tap_frames;

void input_tap_at(int x, int y) {
  if (!iconf.inject)
    return;
  tap_x = (float)x;
  tap_y = (float)y;
  tap_frames = 8;
}

static void tap_pump(void) {
  if (tap_frames <= 0)
    return;
  if (tap_frames == 8) {
    emit(EV_DOWN, ID_PAD, tap_x, tap_y, 1);
    debugPrintf("input: tap down screen (%d,%d) -> engine (%d,%d), touch count %d\n",
                (int)tap_x, (int)tap_y,
                (int)(tap_x * INPUT_SPACE_W / (float)scr_w),
                (int)(tap_y * INPUT_SPACE_H / (float)scr_h),
                input_engine_touch_count());
  } else if (tap_frames == 4) {
    /* A press with no movement in between leaves the engine's drag state
     * hanging: it waits for a move to release the control it grabbed and the
     * frame loop stalls (fps falls to zero) with the slot stuck red. */
    emit(EV_MOVE, ID_PAD, tap_x, tap_y, 1);
    debugPrintf("input: tap move, touch count %d\n",
                input_engine_touch_count());
  } else if (tap_frames == 1) {
    input_touch(2, ID_PAD, tap_x, tap_y);
    debugPrintf("input: tap up, touch count %d\n",
                input_engine_touch_count());
  }
  tap_frames--;
}

void input_cursor(int *x, int *y) {
  if (!free_mode && focus_count > 0) {
    *x = (int)(focus[focus_idx].xp * scr_w / 100.0f);
    *y = (int)(focus[focus_idx].yp * scr_h / 100.0f);
  } else {
    *x = (int)cur_x;
    *y = (int)cur_y;
  }
}

int input_focus_mode(void) { return !free_mode && focus_count > 0; }

void input_focus_box(int *x, int *y, int *w, int *h) {
  if (free_mode || focus_count <= 0) {
    *x = *y = *w = *h = 0;
    return;
  }
  const int cx = (int)(focus[focus_idx].xp * scr_w / 100.0f);
  const int cy = (int)(focus[focus_idx].yp * scr_h / 100.0f);
  *w = (int)(focus[focus_idx].wp * scr_w / 100.0f);
  *h = (int)(focus[focus_idx].hp * scr_h / 100.0f);
  *x = cx - *w / 2;
  *y = cy - *h / 2;
}

/* the real panel reports 1280x720 coordinates whatever the render target is */
static void poll_touchscreen(void) {
  HidTouchScreenState ts = { 0 };
  if (!hidGetTouchScreenStates(&ts, 1))
    return;

  u32 seen = 0;
  for (u32 i = 0; i < ts.count && i < 8; i++) {
    const HidTouchState *t = &ts.touches[i];
    const int id = 100 + (int)i;
    const float x = (float)t->x * scr_w / 1280.0f;
    const float y = (float)t->y * scr_h / 720.0f;
    const int was = (prev_fingers >> i) & 1;
    input_touch(was ? 1 : 0, id, x, y);
    seen |= 1u << i;
    /* the crosshair follows the real finger, so lifting the hand leaves the
     * pointer where the player last aimed instead of snapping back */
    cur_x = x;
    cur_y = y;
  }
  for (u32 i = 0; i < 8; i++)
    if (((prev_fingers >> i) & 1) && !((seen >> i) & 1))
      input_touch(2, 100 + (int)i, cur_x, cur_y);
  prev_fingers = seen;
}

void input_update(void) {
  tap_pump();
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 pressed = down & ~pad_prev;
  const u64 released = ~down & pad_prev;

  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);

  /* ---- right stick: escape to a free cursor ------------------------------ */
  if (rs.x * rs.x + rs.y * rs.y > 8000 * 8000) {
    if (!free_mode) {
      free_mode = 1;
      int fx, fy;
      input_cursor(&fx, &fy);
      cur_x = (float)fx;
      cur_y = (float)fy;
      debugPrintf("input: free cursor mode\n");
    }
  }
  if (pressed & HidNpadButton_Y) {
    free_mode = 0;
    debugPrintf("input: focus mode\n");
  }

  if (free_mode) {
    /* ---- free cursor: right stick aims, A presses where it is ------------- */
    float vx = 0.0f, vy = 0.0f;
    if (rs.x * rs.x + rs.y * rs.y > 2000 * 2000) {
      vx = (float)rs.x / 32767.0f;
      vy = (float)rs.y / 32767.0f;
    }
    const float speed = (float)scr_h * 1.2f / 60.0f;
    if (vx != 0.0f || vy != 0.0f) {
      cur_x += vx * speed;
      cur_y += vy * speed;
      if (cur_x < 0) cur_x = 0;
      if (cur_y < 0) cur_y = 0;
      if (cur_x > scr_w - 1) cur_x = (float)(scr_w - 1);
      if (cur_y > scr_h - 1) cur_y = (float)(scr_h - 1);
      if (cur_down)
        emit(EV_MOVE, ID_PAD, cur_x, cur_y, 1);
    }
  } else if (focus_count > 0) {
    /* ---- focus box: left/right cycles, A picks --------------------------- */
    /* The stick is a level, not an edge: without latching it, holding left
     * would walk the whole list every frame. One step per push, and it rearms
     * after the stick returns near centre. */
    int step = 0;
    const int stick_dir = (ls.x > 24000) ? 1 : (ls.x < -24000) ? -1 : 0;
    if (stick_dir != 0) {
      if (!stick_latch) {
        stick_latch = 1;
        step = stick_dir;
      }
    } else {
      stick_latch = 0;
    }
    if (pressed & (HidNpadButton_Right | HidNpadButton_Up | HidNpadButton_R |
                   HidNpadButton_StickR))
      step = 1;
    else if (pressed & (HidNpadButton_Left | HidNpadButton_Down |
                        HidNpadButton_L | HidNpadButton_StickL))
      step = -1;
    if (step) {
      focus_idx = (focus_idx + step + focus_count) % focus_count;
      debugPrintf("input: focus %d/%d\n", focus_idx + 1, focus_count);
    }
  }

  /* ---- press / release --------------------------------------------------- */
  int cx, cy;
  input_cursor(&cx, &cy);
  const int want_down = (down & (HidNpadButton_A | HidNpadButton_ZR)) != 0;
  if (want_down && !cur_down) {
    cur_down = 1;
    emit(EV_DOWN, ID_PAD, (float)cx, (float)cy, 1);
    debugPrintf("input: press at (%d,%d), touch count %d\n", cx, cy,
                input_engine_touch_count());
  } else if (!want_down && cur_down) {
    cur_down = 0;
    input_touch(2, ID_PAD, (float)cx, (float)cy);
    debugPrintf("input: release, touch count %d\n",
                input_engine_touch_count());
  }

  /* B doubles as a release so a stuck press can always be cleared */
  if ((down & HidNpadButton_B) && cur_down) {
    cur_down = 0;
    input_touch(2, ID_PAD, (float)cx, (float)cy);
  }

  (void)released;

  /* a real finger on the panel wins over the pad */
  poll_touchscreen();

  pad_prev = down;
}

void input_shutdown(void) {
  if (cur_down) {
    cur_down = 0;
    emit(EV_UP, ID_PAD, cur_x, cur_y, 0);
  }
  last_touch_id = -1;
}
