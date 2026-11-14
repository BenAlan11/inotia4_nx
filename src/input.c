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
#include <stdlib.h>
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

/* bound in input_init, used by hud_action() further down */
static void *(*p_ctrl_get_proc)(void *obj);
static long (*p_set_sel)(void *obj, void *pos, long flags);
static long (*p_set_rel)(void *obj, void *pos);
/* engine key layer, used by the menu bind further down */
static long (*p_key_setcode)(long code);
/* message box buttons + the engine's "a UI popup page is on top" test; bound in
 * input_bind, used by popup_press() further down */
static int (*p_popup_ok)(void);
static int (*p_popup_cancel)(void);
static int (*p_scene_popup)(void);

/* used by the d-pad section to find the knob rect (see pad_knob) */
typedef struct { long x, y, w, h; } PadRect;
typedef PadRect (*fn_rect)(void *obj);
static fn_rect p_abs_rect2;
static void *(*p_layer_vk)(void *ctrl);
static void *(*p_find_by_id)(void *parent, long id);

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
  p_ctrl_get_proc = (void *(*)(void *))so_try_find_addr_rx(
      mod, "ControlObject_GetControlProc");
  p_set_sel = (long (*)(void *, void *, long))so_try_find_addr_rx(
      mod, "TouchHandle_SetSelectedControl");
  p_set_rel = (long (*)(void *, void *))so_try_find_addr_rx(
      mod, "TouchHandle_SetReleaseEvent");
  p_abs_rect2 = (fn_rect)so_try_find_addr_rx(mod,
                                             "ControlObject_GetAbsoluteRect");
  p_layer_vk = (void *(*)(void *))so_try_find_addr_rx(
      mod, "InputLayer_GetVirtualKey");
  p_find_by_id = (void *(*)(void *, long))so_try_find_addr_rx(
      mod, "ControlObject_FindByID");
  p_key_setcode = (long (*)(long))so_try_find_addr_rx(mod, "KEY_SetCode");
  /* A/B on a message box: the two button callbacks, plus the engine's own
   * "a UI popup page is on top" test */
  p_popup_ok = (int (*)(void))so_try_find_addr_rx(mod,
                                                 "UIPopupMsg_ButtonOKExe");
  p_popup_cancel = (int (*)(void))so_try_find_addr_rx(
      mod, "UIPopupMsg_ButtonCancelExe");
  p_scene_popup = (int (*)(void))so_try_find_addr_rx(mod, "POPUPSTATE_Exist");
  debugPrintf("input: handleCletEvent_MV = %p\n", (void *)p_motion);
  debugPrintf("input: control action %p / select %p release %p\n",
              (void *)p_ctrl_get_proc, (void *)p_set_sel, (void *)p_set_rel);
  debugPrintf("input: pad geometry %p / vkey %p / findbyid %p\n",
              (void *)p_abs_rect2, (void *)p_layer_vk, (void *)p_find_by_id);
  debugPrintf("input: KEY_SetCode = %p\n", (void *)p_key_setcode);
  debugPrintf("input: popup ok %p cancel %p exist %p\n", (void *)p_popup_ok,
              (void *)p_popup_cancel, (void *)p_scene_popup);
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

/* defined with tap_pump below; the HUD state machine needs to know whether a
 * one-shot tap currently owns the virtual finger */
static int tap_frames;

/* ------------------------------------------------------------------------- */
/* in-game HUD -> pad                                                        */
/*                                                                           */
/* The rects below are filled from the engine's OWN control tree (main.c
 * hud_tick), so nothing here is measured off a screenshot. They arrive in
 * engine layout units; handleCletEvent scales the 480x320 input space by
 * SGL_GetScreenWidth()/480, so to land on engine coordinate ex we must feed
 * the screen coordinate ex * scr_w / SGL_W -- emit() then cancels scr_w out.
 * ------------------------------------------------------------------------- */
static volatile long hud_r[HUD_RECT_N][4];
static volatile unsigned char hud_seen[HUD_RECT_N];
static volatile int hud_active;
static int hud_prev_mode;

/* The control objects behind + , - and A. The touch layer keeps ONE selected
 * control and ONE moving control, so sharing the single virtual finger made A
 * cut the d-pad's hold. These three buttons therefore do not use a finger: the
 * engine's own press/release functions are called on the control directly.
 *
 * The two globals are (verified against the disassembly, not guessed):
 *   0x2b2eb0 = moving control   -- TouchHandle_GetMovingControl reads it
 *   0x2b2eb8 = selected control -- TouchHandle_GetSelectedControl reads it
 * The first version had them the other way round, which is why pressing A still
 * killed the stick: it cleared the *selected* control instead of hiding the
 * moving one from TouchHandle_SetSelectedControl.
 *
 * Both are saved before the call and written back after it, because either
 * function can clear them: SetSelectedControl hands the selection to the new
 * control (and tells the previous one it lost the pointer), SetReleaseEvent
 * releases the *moving* control when it is not the object being released and
 * then clears it -- i.e. releasing A would release the d-pad. */
static void *hud_obj[HUD_RECT_N];
static long (*p_set_sel)(void *obj, void *pos, long flags);
static long (*p_set_rel)(void *obj, void *pos);
static int act_pending[HUD_RECT_N];   /* frames until the release is sent */

/* engine globals, in libgame's own layout */
#define TH_MOVING   0x2b2eb0u
#define TH_SELECTED 0x2b2eb8u

void input_set_hud_obj(int which, void *obj) {
  if (which >= 0 && which < HUD_RECT_N)
    hud_obj[which] = obj;
}

/* Engine layout coordinates: the control rects and ControlObject_IncludePoint
 * are both in the engine's 1137x640 space, not the 480x320 input space. */
static const char *hud_name(int which) {
  return which == HUD_BTN_PLUS ? "+" : which == HUD_BTN_MINUS ? "-" : "A";
}

static void hud_point(int which, long *x, long *y) {
  *x = hud_r[which][0] + hud_r[which][2] / 2;
  *y = hud_r[which][1] + hud_r[which][3] / 2;
}

/* Fire one press/release on a control at an explicit engine-layout point.
 * Every caller hides the touch layer's two roles for the duration of the call:
 * with nothing selected the "you lost the pointer" event never fires on another
 * control, and with nothing moving neither function can release one. */
static long hud_event_at(void *obj, long ex, long ey, int press) {
  unsigned char *vb = input_engine_virtbase();
  if (!obj || !vb)
    return -1;
  struct { long x, y, z; } pt;
  pt.x = ex;
  pt.y = ey;
  pt.z = 0;
  void **moving = (void **)(vb + TH_MOVING);
  void **selected = (void **)(vb + TH_SELECTED);
  void *saved_moving = *moving;
  void *saved_selected = *selected;
  *moving = NULL;
  *selected = NULL;
  long rc = 0;
  if (press) {
    if (!p_set_sel)
      rc = -1;
    else
      p_set_sel(obj, &pt, 0x281);
  } else {
    if (!p_set_rel)
      rc = -1;
    else
      p_set_rel(obj, &pt);
  }
  *moving = saved_moving;
  *selected = saved_selected;
  return rc;
}

/* main.c's pad -> quick-slot mapping uses this instead of injecting a touch:
 * the button is pressed straight on its control object, exactly like the
 * stick / attack disc, so the HUD touch block and the control-tree dispatch
 * order cannot interfere with it. */
int input_fire_control(void *obj, long ex, long ey, int press) {
  return (int)hud_event_at(obj, ex, ey, press);
}

/* defined with the d-pad section below; hud_action asks it to re-press the
 * stick on the next frame, because a button press overwrites the direction */
static void pad_remind(void);

/* The three globals the two pad procs share, read for the log only:
 *   0x29e820 -> int   "current key" that KEY_SetCode() writes
 *   0x29e7c8 -> byte  "a pad button is held" (UIPlay_ButtonDirection sets it)
 *   0x29e7d0 -> byte  "the OK disc is held" (UIPlay_ButtonOK sets it)
 * The OK proc ignores a press while 0x29e7d0 is already 1, so a release that
 * never reached it would silently kill every later attack -- worth seeing. */
static void hud_key_state(const char *what) {
  unsigned char *vb = input_engine_virtbase();
  if (!vb)
    return;
  void **pk = (void **)(vb + 0x29e820u);
  void **pd = (void **)(vb + 0x29e7c8u);
  void **po = (void **)(vb + 0x29e7d0u);
  if (!*pk || !*pd || !*po)
    return;
  debugPrintf("input: after %s key %d pad-held %d ok-held %d\n", what,
              *(volatile int *)*pk, *(volatile unsigned char *)*pd,
              *(volatile unsigned char *)*po);
}

static long hud_action(int which, int press) {
  if (which < 0 || which >= HUD_RECT_N || !hud_obj[which])
    return -1;
  long ex = 0, ey = 0;
  hud_point(which, &ex, &ey);
  const long rc = hud_event_at(hud_obj[which], ex, ey, press);
  if (rc < 0)
    return rc;
  /* the direction lives in the game's single "current key" slot and these
   * buttons write the same slot, so the stick must push its key back -- but
   * only after the game has had a chance to read this one (see pad_remind) */
  pad_remind();
  static int logs;
  if (logs < 24) {
    logs++;
    debugPrintf("input: HUD %s %s (proc %p rc %ld)\n", hud_name(which),
                press ? "press" : "release",
                p_ctrl_get_proc ? p_ctrl_get_proc(hud_obj[which]) : NULL, rc);
    if (which == HUD_BTN_ACTION)
      hud_key_state(press ? "A press" : "A release");
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* d-pad: driven straight on its own control, with no virtual finger          */
/*                                                                           */
/* UIPlay_ButtonDirection (0x15a998) is the pad's action proc. On a press it  */
/* sets dx = px - cx, dy = cy - py against the pad centre, picks the axis by  */
/* |dx| > |dy|, and calls KEY_SetCode(table[index]) -- index 0 left, 1 right, */
/* 2 up, 3 down. Before that it demands the press point be inside the control */
/* found as child 0x16 of the virtual-key layer (the knob the finger drags).  */
/* Only the SIGN of dx/dy matters for the direction, so a small offset from   */
/* the centre is enough; the old 0.36-of-the-pad point (90px out) fell outside */
/* the knob, the containment test failed, and the press only cleared the key. */
static int pad_dir = -1;          /* -1 released, else 0 L / 1 R / 2 U / 3 D */
static long pad_px, pad_py;
static int pad_age;

/* The knob rect, when the engine lets us see it. Clipping the press point into
 * this rect is what makes the containment test inside the direction proc pass
 * no matter where the last finger left the knob. */
static int pad_knob(PadRect *out) {
  if (!p_layer_vk || !p_find_by_id || !p_abs_rect2 || !hud_obj[HUD_DPAD])
    return 0;
  void *vk = p_layer_vk(hud_obj[HUD_DPAD]);
  if (!vk)
    return 0;
  void *nub = p_find_by_id(vk, 0x16);
  if (!nub)
    return 0;
  const PadRect r = p_abs_rect2(nub);
  if (r.w < 8 || r.h < 8)
    return 0;
  *out = r;
  return 1;
}

/* Re-send the press every few frames: the direction lives in the game's single
 * "current key" slot, and the A disc writes that same slot, so the stick has to
 * push its direction back if a button was pressed in between. */
#define PAD_REASSERT_FRAMES 6

static void pad_press_at(int dir) {
  long cx = 0, cy = 0;
  hud_point(HUD_DPAD, &cx, &cy);
  long off = hud_r[HUD_DPAD][2] / 18;      /* 252 -> 14 engine px */
  if (off < 10) off = 10;
  if (off > 24) off = 24;
  long ex = cx, ey = cy;
  if (dir == 0)      ex = cx - off;
  else if (dir == 1) ex = cx + off;
  else if (dir == 2) ey = cy - off;        /* engine Y grows downwards */
  else               ey = cy + off;

  /* UIPlay_ButtonDirection adds TouchHandle_GetControlOffset() (0x2b2ef0) to
   * the point it was given, and that global holds the delta of the last finger
   * move -- a drag across the map leaves tens of pixels in it, which would push
   * the press out of the knob and back into the "not inside" branch. */
  unsigned char *vb = input_engine_virtbase();
  if (vb) {
    volatile long *delta = (volatile long *)(vb + 0x2b2ef0u);
    static int dlogs;
    if ((delta[0] || delta[1]) && dlogs < 6) {
      dlogs++;
      debugPrintf("input: pad cleared drag delta %ld,%ld\n", delta[0], delta[1]);
    }
    delta[0] = 0;
    delta[1] = 0;
  }

  PadRect k;
  const int knob = pad_knob(&k);
  if (knob) {
    if (ex < k.x + 3) ex = k.x + 3;
    if (ex > k.x + k.w - 3) ex = k.x + k.w - 3;
    if (ey < k.y + 3) ey = k.y + 3;
    if (ey > k.y + k.h - 3) ey = k.y + k.h - 3;
  }
  hud_event_at(hud_obj[HUD_DPAD], ex, ey, 1);
  pad_px = ex;
  pad_py = ey;
  pad_dir = dir;
  static int logs;
  if (logs < 10) {
    logs++;
    if (knob)
      debugPrintf("input: pad dir %d press (%ld,%ld) knob %ld,%ld %ldx%ld\n",
                  dir, ex, ey, k.x, k.y, k.w, k.h);
    else
      debugPrintf("input: pad dir %d press (%ld,%ld), no knob rect\n", dir, ex,
                  ey);
  }
}

static void pad_drive(int dir) {
  if (dir == pad_dir) {
    if (++pad_age >= PAD_REASSERT_FRAMES) {
      pad_age = 0;
      hud_event_at(hud_obj[HUD_DPAD], pad_px, pad_py, 1);
    }
    return;
  }
  if (pad_dir >= 0)
    hud_event_at(hud_obj[HUD_DPAD], pad_px, pad_py, 0);
  pad_press_at(dir);
  pad_age = 0;
}

static void pad_release(void) {
  if (pad_dir < 0)
    return;
  hud_event_at(hud_obj[HUD_DPAD], pad_px, pad_py, 0);
  pad_dir = -1;
  pad_age = 0;
  static int logs;
  if (logs < 10) {
    logs++;
    debugPrintf("input: pad release (%ld,%ld)\n", pad_px, pad_py);
  }
}

/* Asked for by hud_action after any button press.
 *
 * The game keeps ONE "current key" (the int behind 0x29e820): the pad's proc
 * writes the direction into it, UIPlay_ButtonOK writes the OK key into it.
 * Re-pressing the stick on the very frame the button fired therefore replaced
 * the OK key before the play state got to read it -- that is why A stopped
 * attacking while the stick was held, and why it attacked again as soon as the
 * stick was released. Giving the button two frames of "current key" is enough
 * for the game to see it, and two frames of missing direction is invisible in
 * movement (the pad's held flags stay set the whole time). */
#define PAD_REASSERT_DELAY 2
static void pad_remind(void) {
  if (pad_dir < 0)
    return;
  pad_age = PAD_REASSERT_FRAMES - PAD_REASSERT_DELAY;
}

void input_hud_begin(void) {
  for (int i = 0; i < HUD_RECT_N; i++)
    hud_seen[i] = 0;
}

void input_set_hud_rect(int which, long x, long y, long w, long h) {
  if (which < 0 || which >= HUD_RECT_N)
    return;
  hud_r[which][0] = x; hud_r[which][1] = y;
  hud_r[which][2] = w; hud_r[which][3] = h;
  hud_seen[which] = 1;
}

void input_hud_end(void) {
  /* Only the real HUD has BOTH the action disc (right) and the d-pad (left,
   * lower). Menu pages reuse some of the small ids, so the geometry is part of
   * the test rather than an assumption. */
  int ok = hud_seen[HUD_BTN_ACTION] && hud_seen[HUD_DPAD] &&
           hud_r[HUD_BTN_ACTION][0] > 600 && hud_r[HUD_DPAD][0] < 400 &&
           hud_r[HUD_DPAD][1] > 250;
  if (ok != hud_active) {
    static int first = 1;
    if (first || ok) {
      first = 0;
      debugPrintf("input: HUD %s (sgl %dx%d, action %ld,%ld %ldx%ld, dpad "
                  "%ld,%ld %ldx%ld)\n",
                  ok ? "present" : "absent",
                  input_engine_screen_w(), input_engine_screen_h(),
                  hud_r[HUD_BTN_ACTION][0], hud_r[HUD_BTN_ACTION][1],
                  hud_r[HUD_BTN_ACTION][2], hud_r[HUD_BTN_ACTION][3],
                  hud_r[HUD_DPAD][0], hud_r[HUD_DPAD][1],
                  hud_r[HUD_DPAD][2], hud_r[HUD_DPAD][3]);
    }
  }
  hud_active = ok;
}

int input_hud_present(void) { return hud_active; }

/* 1 while a direction is being driven: that is exactly when the engine lights
 * the pad up, which is what the pad-zone probe in main.c wants to catch. */
int input_pad_held(void) { return pad_dir >= 0; }

/* fx/fy are fractions of the control's rect (0.5 = centre). */
static int hud_to_screen(int which, float fx, float fy, float *sx, float *sy) {
  if (which < 0 || which >= HUD_RECT_N || !hud_seen[which])
    return 0;
  const long w = hud_r[which][2], h = hud_r[which][3];
  if (w <= 0 || h <= 0)
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;   /* engine layout width seen in the control tree */
  if (eh <= 0) eh = 640;
  const float ex = (float)hud_r[which][0] + (float)w * fx;
  const float ey = (float)hud_r[which][1] + (float)h * fy;
  *sx = ex * (float)scr_w / (float)ew;
  *sy = ey * (float)scr_h / (float)eh;
  return 1;
}

/* One virtual finger for the whole HUD, and only the d-pad uses it: +, - and A
 * are fired straight on their controls (hud_action), because the touch layer
 * keeps a single selected control and a second finger would release the pad.
 * That is what made A cut the stick short. If a control has no action proc the
 * old path is kept as a fallback, so A still works everywhere. */
static int a_was_down;
static int a_via_touch;

/* Engine-layout point inside a HUD rect, at fx/fy fractions of its size. */
static int hud_engine_point(int which, float fx, float fy, long *ex, long *ey) {
  if (which < 0 || which >= HUD_RECT_N || !hud_seen[which])
    return 0;
  const long w = hud_r[which][2], h = hud_r[which][3];
  if (w <= 0 || h <= 0)
    return 0;
  *ex = hud_r[which][0] + (long)((float)w * fx);
  *ey = hud_r[which][1] + (long)((float)h * fy);
  return 1;
}

/* ------------------------------------------------------------------------- */
/* Menus and dialogs: A = confirm / yes, B = back / skip / no                 */
/*                                                                           */
/* The per-scene key handlers (Scene_Event_KeyPress_POPUP_SC_*, e.g. 0x1e6b10)*/
/* take no arguments and read three globals:                                  */
/*   0x29dc50 -> byte  "a key was just pressed" (they only act when it is 1)  */
/*   0x29dc48 -> byte  "a key is held"          (cleared at the end of a call) */
/*   0x29e820 -> int   the key code                                           */
/* then compare that code with the key table (0x29dba0):                      */
/*   tbl[0x10] = cancel / back, tbl[4] = OK.                                  */
/* handleCletEvent (0x169854) does those three stores and then calls the       */
/* handler registered at 0x29eb90 -- but 0x169880 shows that global is a       */
/* SLOT: ldr x9,[0x29eb90] (slot address); ldr x8,[x9] (function); blr x8.    */
/* That handler is written for the Java key-event pump (the pump sets the      */
/* flags, the handler consumes them, the pump clears them). This port has no   */
/* such pump, so it must never call it: even with the slot resolved correctly  */
/* the call stalls the frame loop. ab_press() below is the whole A/B policy.   */
static int menu_a_was, menu_b_was;
/* which layer A/B were last announced as talking to */
static int ab_last_layer = -1;

/* 1 while the current scene has a key handler registered (0x29eb90). Decided
 * per frame rather than remembered from the last press: entering a screen that
 * registers none has to bring the touch fallback back. */
static int menu_has_handler(void) {
  unsigned char *vb = input_engine_virtbase();
  if (!vb)
    return 0;
  return *(void **)(vb + 0x29eb90u) != NULL;
}

/* The old menu_key_press() lived here. It wrote the key code and padflags and
 * then called *(0x29eb90) as a function -- but that global holds the SLOT
 * ADDRESS, not the handler. On the character page A therefore jumped into the
 * engine's .bss and ran data as instructions: Eden logged "Cannot execute
 * instruction at unmapped address" and the frame loop died (0 FPS / nan ms).
 * Resolving the second dereference and validating the pointer against the
 * ELF's PT_LOAD|PF_X ranges is not enough either: the real handler still
 * hangs without the Java key-event pump that normally drives it.
 *
 * Removed for good. A/B on non-message-box pages go through the engine's
 * public touch path (popup_press / corner_tap) or are ignored; see ab_press().
 * menu_has_handler() stays -- input_update() uses it to decide whether A may
 * fall back to the focus-cursor touch press. */

/* ------------------------------------------------------------------------- */
/* message boxes: the engine's own "确认 / 是 / 否" box (UIPopupMsg)          */
/*                                                                           */
/* A UIPopupMsg is NOT a scene: it lives in its own globals and is dismissed  */
/* by two ControlButtons whose callbacks are UIPopupMsg_ButtonOKExe /        */
/* ...ButtonCancelExe. The engine reaches them from its key path only --     */
/* handleCletEvent() forwards key events to UIPopupMsg_Event(), which reads   */
/* the current key and calls one of the two callbacks -- and this port never  */
/* emits key events, so a box could only be answered by tapping its button.   */
/* That is why "连接出错。请稍后重试。确认" sat there with no way out.        */
/*                                                                           */
/* Calling the callback directly is exactly what the button does on release   */
/* (UIPopupMsg_Event does the same), and the box deletes itself inside it.    */
/*                                                                           */
/* globals, from the disassembly of UIPopupMsg_Get*Control:                  */
/*   0x2b8d78 base control   -- created with the box, cleared when it closes  */
/*   0x2b8d80 main control                                                   */
/*   0x2b8d98 OK button     0x2b8da0 cancel button                           */
#define POPUP_BASE   0x2b8d78u
/* 0x2b8d80 main control -- unused on purpose, see popup_on() */
#define POPUP_OKCTL  0x2b8d98u
#define POPUP_CNCTL  0x2b8da0u

/* 1 while a message box is on screen. The base control is deleted when either
 * button is taken, so a non-NULL pointer is the engine's own "box is up".
 * The main control (0x2b8d80) is deliberately NOT part of the test: nothing
 * clears it, so a stale pointer there would make every later press a "confirm"
 * on a box that no longer exists. */
static int popup_on(void) {
  unsigned char *vb = input_engine_virtbase();
  if (!vb)
    return 0;
  return *(void **)(vb + POPUP_BASE) != NULL;
}

/* 1 while a UI popup page (character info, inventory, store, system menu ...)
 * is pushed on top of whatever is behind it. POPUPSTATE_Push is only called by
 * the UI popup layer (UI_PopupProcessCreate), never by the play scene, so this
 * is false in the field and true on every page that owns the screen. */
static int scene_popup_on(void) {
  return p_scene_popup ? p_scene_popup() != 0 : 0;
}

enum { AB_MAP = 0, AB_POPUP, AB_SCENE, AB_MENU };

static const char *ab_layer_name(int layer) {
  return layer == AB_POPUP ? "message box" : layer == AB_SCENE ? "popup page"
         : layer == AB_MAP ? "play HUD" : "menu";
}

/* Which layer the pad's A/B belong to this frame. A box wins over everything:
 * it is modal, and the scene behind it must not see the press. */
static int ab_layer(void) {
  if (popup_on())
    return AB_POPUP;
  if (scene_popup_on())
    return AB_SCENE;
  if (input_hud_present())
    return AB_MAP;
  return AB_MENU;
}

/* A = 确认 / 是, B = 取消 / 否 / 返回. */
static void popup_press(int ok) {
  unsigned char *vb = input_engine_virtbase();
  if (!vb || !popup_on())
    return;
  void *okc = *(void **)(vb + POPUP_OKCTL);
  void *cnc = *(void **)(vb + POPUP_CNCTL);
  /* UIPopupMsg_Event does the same: with no cancel button the cancel key falls
   * through to the OK callback, so an OK-only box (the screenshot's
   * "连接出错") still closes with B. */
  const int use_ok = ok || !cnc;
  if (use_ok) {
    if (!p_popup_ok)
      return;
    p_popup_ok();
  } else {
    if (!p_popup_cancel)
      return;
    p_popup_cancel();
  }
  debugPrintf("input: message box %s -> %s (ok %p cancel %p)\n",
              use_ok ? "OK" : "cancel", use_ok ? "OK" : "cancel", okc, cnc);
}

/* B on a page with a × in the top-left corner: tap it the way a finger would.
 *
 * The engine's internal back-key path (POPUPSTATE_Event / the scene handler at
 * 0x29eb90) was tried and it terminated the process with SIGSEGV when B was
 * pressed on the character page. Do NOT call those functions directly. A
 * normal touch through handleCletEvent_MV is the engine's own public input
 * path -- the same one real taps use -- and cannot disturb its internal state.
 *
 * The button's rectangle comes from the engine's control tree, converted from
 * engine layout units to screen pixels the same way hud_to_screen does. */
static int corner_tap(void) {
  long cx = 0, cy = 0;
  const int kind = ui_corner_button(&cx, &cy);
  if (!kind)
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  const int sx = (int)((float)cx * (float)scr_w / (float)ew);
  const int sy = (int)((float)cy * (float)scr_h / (float)eh);
  input_tap_at(sx, sy);
  static int logs;
  if (logs < 16) {
    logs++;
    debugPrintf("input: back tap %s engine (%ld,%ld) -> screen (%d,%d)\n",
                kind == 2 ? "SKIP" : "CLOSE", cx, cy, sx, sy);
  }
  return 1;
}

/* A on a UI_PopupProcess info box ("路障" and friends) taps its 确认 button.
 *
 * Those boxes are not UIPopupMsg, so popup_on() is false and their A press
 * used to be dropped by ab_press(). They DO have a real ControlButton in the
 * live tree, though; ui_confirm_button() finds it by proc type + tree shape.
 * Tapping it through the engine's public touch path (the same one corner_tap
 * uses for B on the x button) keeps all of the engine's own state handling:
 * it closes the box, runs its action and pops the popup state. No engine
 * internal or scene key handler is called. */
static int confirm_tap(void) {
  long cx = 0, cy = 0;
  if (!ui_confirm_button(&cx, &cy))
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  const int sx = (int)((float)cx * (float)scr_w / (float)ew);
  const int sy = (int)((float)cy * (float)scr_h / (float)eh);
  input_tap_at(sx, sy);
  static int logs;
  if (logs < 16) {
    logs++;
    debugPrintf("input: confirm tap engine (%ld,%ld) -> screen (%d,%d)\n",
                cx, cy, sx, sy);
  }
  return 1;
}

/* A = 确认 / 是, B = 取消 / 否 / 返回.
 *
 * This is the only place outside the play HUD that turns a pad button into an
 * engine action, and it deliberately uses just two safe paths:
 *   - a UIPopupMsg message box -> its OK / cancel callbacks (popup_press)
 *   - a UI_PopupProcess info box (路障 etc.) -> A taps its real 确认 control,
 *     found in the live control tree (confirm_tap)
 *   - B on a page with a × / skip control -> a real touch through the public
 *     handleCletEvent_MV input path (corner_tap)
 * Everything else is ignored. In particular A on the character page is NOT a
 * key press: ab_layer() reports that page as AB_MENU (POPUPSTATE_Exist() is 0
 * there), and every attempt to drive it through the engine's per-scene key
 * handler froze the frame loop. Nothing on these pages ever answered A through
 * that path successfully, so ignoring it loses no working feature.
 * The 10:20 fix only guarded layer == AB_SCENE, which is why it did not help. */
static void ab_press(int ok, int layer) {
  static int press_logs;
  if (press_logs < 24) {
    press_logs++;
    debugPrintf("input: %s press -> %s\n", ok ? "A" : "B",
                ab_layer_name(layer));
  }
  if (layer == AB_POPUP) {
    popup_press(ok);
    return;
  }
  /* A UI_PopupProcess info box has POPUPSTATE_Exist() set (AB_SCENE) but no
   * UIPopupMsg base control, so it never reaches popup_press(). Tap its own
   * 确认 button instead. Deliberately limited to AB_SCENE: the character page
   * reports AB_MENU and must keep ignoring A, and the play HUD owns A. */
  if (ok && cfg.tap_buttons && layer == AB_SCENE && confirm_tap())
    return;
  /* A previous attempt tapped UIPopupMsg's OK control (0x2b8d98) here; that
   * pointer is NOT cleared when a box closes, so reading its rectangle through
   * ControlObject_GetAbsoluteRect hung the engine's frame loop. Do not bring
   * that back without finding the button in the live control tree instead. */
  if (!ok && cfg.tap_buttons && (layer == AB_SCENE || layer == AB_MENU) &&
      corner_tap())
    return;
  static int ignored_logs;
  if (ignored_logs < 24) {
    ignored_logs++;
    debugPrintf("input: %s on %s ignored (no safe engine key handler)\n",
                ok ? "A" : "B", ab_layer_name(layer));
  }
}

static void hud_update(u64 down, u64 pressed, HidAnalogStickState ls) {
  /* The settings-page navigator owns A/B/L/R/d-pad while its page is up.
   * Skip the ordinary menu pair here so a press cannot be counted twice. */
  if (input_nav_active()) {
    if (pad_dir >= 0) { pad_dir = -1; pad_age = 0; }
    menu_a_was = (down & HidNpadButton_A) != 0;
    menu_b_was = (down & HidNpadButton_B) != 0;
    a_was_down = (down & HidNpadButton_Y) != 0;
    return;
  }
  const int hud = input_hud_present();
  /* 2026-11-09: the in-field ATTACK key moved from A to Y. A stays the menu /
   * dialog confirm key (and is the settings-page activate key), so the two
   * roles need separate edge tracking. ZR (Xbox RT) is a MODIFIER for the four
   * face buttons; while it is held neither key is an attack/confirm. */
  const int rt = (down & HidNpadButton_ZR) != 0;
  const int confirm_now = (down & HidNpadButton_A) != 0 && !rt;
  const int attack_now  = (down & HidNpadButton_Y) != 0 && !rt;
  const int b_now = (down & HidNpadButton_B) != 0 && !rt;

  /* Which layer owns A/B this frame. A message box is modal and wins over the
   * HUD: that is the whole point of the change -- "确认" used to be unreachable
   * while the play HUD was up, because A was wired to the attack disc. */
  const int layer = ab_layer();
  if (layer != ab_last_layer) {
    ab_last_layer = layer;
    debugPrintf("input: A/B now talk to the %s\n", ab_layer_name(layer));
  }

  /* Outside the map the four HUD controls do not exist (the objects we kept
   * are destroyed with the play scene), so nothing here may touch them. The
   * buttons become the ordinary menu pair instead. */
  if (!hud) {
    if (pad_dir >= 0) {
      /* leaving the map: forget the hold, the control is gone */
      pad_dir = -1;
      pad_age = 0;
    }
    if (confirm_now && !menu_a_was)
      ab_press(1, layer);
    if (b_now && !menu_b_was)
      ab_press(0, layer);
    menu_a_was = confirm_now;
    menu_b_was = b_now;
    a_was_down = attack_now;
    return;
  }
  /* A message box or a UI popup page is over the map: it is modal, so the HUD
   * underneath must not act on the stick or on + / -. Only A/B are passed on. */
  if (layer != AB_MAP) {
    if (pad_dir >= 0)
      pad_release();
    if (confirm_now && !menu_a_was)
      ab_press(1, layer);
    if (b_now && !menu_b_was)
      ab_press(0, layer);
    a_was_down = attack_now;
    menu_a_was = confirm_now;
    menu_b_was = b_now;
    return;
  }
  /* A belongs to the story, Y belongs to the field:
   *   Y -> HUD_BTN_ACTION, i.e. attack AND the map's talk/open interaction;
   *   A -> message boxes (popup_press above) and story dialogue.
   * A dialogue bubble has no OK button -- it is a full-screen "tap anywhere
   * to continue" control. It appears only while the dialogue is up (verified
   * against the control-tree dumps: absent on the plain map), so tapping it
   * on A's press edge cannot turn A into a map interaction. */
  if (confirm_now && !menu_a_was) {
    long dx, dy;
    if (ui_dialog_button(&dx, &dy)) {
      int ew = input_engine_screen_w(), eh = input_engine_screen_h();
      if (ew <= 0) ew = 1137;
      if (eh <= 0) eh = 640;
      input_tap_at((int)((float)dx * (float)scr_w / (float)ew),
                   (int)((float)dy * (float)scr_h / (float)eh));
      static int dlogs;
      if (dlogs < 8) {
        dlogs++;
        debugPrintf("input: A advances story dialogue at (%ld,%ld)\n", dx, dy);
      }
      menu_a_was = confirm_now;
      a_was_down = attack_now;
      return;
    }
  }
  menu_a_was = confirm_now;

  /* + / - are one-shot taps. */
  if (pressed & (HidNpadButton_Plus | HidNpadButton_Minus)) {
    const int which = (pressed & HidNpadButton_Plus) ? HUD_BTN_PLUS
                                                     : HUD_BTN_MINUS;
    if (hud_action(which, 1) >= 0) {
      /* the handler wants a matching release; give it two frames so the press
       * state is seen by the next update as well */
      act_pending[which] = 2;
    } else {
      float sx = 0, sy = 0;
      if (hud_to_screen(which, 0.5f, 0.5f, &sx, &sy)) {
        if (cur_down) {
          cur_down = 0;
          input_touch(2, ID_PAD, cur_x, cur_y);
        }
        input_tap_at((int)sx, (int)sy);
        debugPrintf("input: HUD %s tap screen (%.0f,%.0f)\n",
                    which == HUD_BTN_PLUS ? "+" : "-", sx, sy);
      }
    }
  }
  for (int i = 0; i < HUD_RECT_N; i++) {
    if (act_pending[i] > 0) {
      act_pending[i]--;
      if (act_pending[i] == 0)
        hud_action(i, 0);
    }
  }

  /* Y (was A): press on the edge, release on the edge, never touching the
   * pointer. A is no longer the attack key -- see confirm_now above. */
  if (attack_now != a_was_down) {
    if (attack_now)
      a_via_touch = hud_action(HUD_BTN_ACTION, 1) < 0;
    else if (!a_via_touch)
      hud_action(HUD_BTN_ACTION, 0);
    a_was_down = attack_now;
  }

  /* B on the map itself has nothing to cancel, so it keeps its old job: stop
   * walking. That needs the release on every frame the button is held, not just
   * on the edge -- the stick re-asserts the direction every few frames. */
  if (b_now)
    pad_release();
  menu_b_was = b_now;

  /* tap_pump() owns the finger while a one-shot tap is in flight */
  if (tap_frames > 0)
    return;

  /* The stick: no finger, no pointer queue. The point inside the pad that the
   * direction maps to is handed to the pad's own event proc, so the game gets
   * the same press UIPlay_ButtonDirection would have seen from a touch -- and
   * nothing in the touch layer is touched, so no feedback is drawn and no other
   * button can steal the hold. */
  /* The stick: no finger, no pointer queue. The pad's own action proc is
   * pressed with a point just off the pad centre on the chosen side, so the
   * game receives the same press UIPlay_ButtonDirection would have seen from a
   * touch -- and nothing in the touch layer is touched, so no feedback is
   * drawn and no other button can steal the hold.
   * HID stick Y is positive when pushed UP while engine Y grows downwards. */
  int dir = -1;
  if (ls.x * ls.x + ls.y * ls.y > 2000 * 2000) {
    const long ax = ls.x < 0 ? -ls.x : ls.x;
    const long ay = ls.y < 0 ? -ls.y : ls.y;
    if (ax > ay)
      dir = ls.x < 0 ? 0 : 1;
    else
      dir = ls.y > 0 ? 2 : 3;
  }
  if (dir >= 0)
    pad_drive(dir);
  else
    pad_release();

  /* A without an action proc still needs the pointer; that path is the one
   * thing left that owns the virtual finger. */
  if (a_via_touch && attack_now) {
    float tx = 0, ty = 0;
    if (hud_to_screen(HUD_BTN_ACTION, 0.5f, 0.5f, &tx, &ty)) {
      if (!cur_down) {
        cur_down = 1;
        emit(EV_DOWN, ID_PAD, tx, ty, 1);
      } else if (fabsf(tx - cur_x) > 2.0f || fabsf(ty - cur_y) > 2.0f) {
        emit(EV_MOVE, ID_PAD, tx, ty, 1);
      }
      cur_x = tx;
      cur_y = ty;
    }
  } else if (cur_down) {
    cur_down = 0;
    input_touch(2, ID_PAD, cur_x, cur_y);
  }
}

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
    if (n < 120) {
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
/* A tap owns the virtual finger for 8 frames. Rapid L/R tab presses used to
 * overwrite tap_frames, cancelling the previous tap's UP event and leaving
 * the engine with a finger stuck down (the "menu froze" symptom). Pending
 * taps are queued instead, so each one runs its full down/move/up. */
#define TAP_QUEUE_MAX 4
static float tap_qx[TAP_QUEUE_MAX], tap_qy[TAP_QUEUE_MAX];
static int tap_qn;

static void tap_begin(int x, int y) {
  tap_x = (float)x;
  tap_y = (float)y;
  tap_frames = 8;
}

void input_tap_at(int x, int y) {
  if (!iconf.inject)
    return;
  if (tap_frames > 0) {
    if (tap_qn < TAP_QUEUE_MAX) {
      tap_qx[tap_qn] = (float)x;
      tap_qy[tap_qn] = (float)y;
      tap_qn++;
    }
    return;
  }
  tap_begin(x, y);
}

static void tap_pump(void) {
  if (tap_frames <= 0) {
    if (tap_qn <= 0)
      return;
    tap_begin((int)tap_qx[0], (int)tap_qy[0]);
    for (int i = 1; i < tap_qn; i++) {
      tap_qx[i - 1] = tap_qx[i];
      tap_qy[i - 1] = tap_qy[i];
    }
    tap_qn--;
  }
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

/* On the map screen the four HUD controls are no longer touch targets: the pad,
 * A and the two side keys answer to the controller only. A finger that lands
 * inside one of their rects is swallowed whole -- press, moves and lift -- so
 * the engine never paints its touch feedback there and the area stays free for
 * whatever goes into it later. Story popups and the option pages have no HUD
 * rects, so touch keeps working normally on those. */
static int hud_screen_rect(int which, float *x, float *y, float *w, float *h) {
  if (which < 0 || which >= HUD_RECT_N || !hud_seen[which])
    return 0;
  if (hud_r[which][2] <= 0 || hud_r[which][3] <= 0)
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  *x = (float)hud_r[which][0] * scr_w / (float)ew;
  *y = (float)hud_r[which][1] * scr_h / (float)eh;
  *w = (float)hud_r[which][2] * scr_w / (float)ew;
  *h = (float)hud_r[which][3] * scr_h / (float)eh;
  return 1;
}

/* The six quick buttons can sit INSIDE the d-pad / action-disc rects. Those
 * rects are blocked from real screen touches (the stick and A are driven by
 * the pad directly, on their control objects), but a tap on a quick button
 * still has to reach the engine or a shortcut can never be assigned to it.
 * main.c publishes the six button rects; only those small rectangles are
 * exempt. Everything else in the HUD rects stays blocked as before. */
static volatile long g_quick_r[6][4];
static volatile int g_quick_on;

void input_set_quick_rects(const long r[6][4], int enabled) {
  g_quick_on = enabled;
  if (!enabled)
    return;
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 4; j++)
      g_quick_r[i][j] = r[i][j];
}

static int hud_quick_hit(float sx, float sy) {
  if (!g_quick_on)
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  for (int i = 0; i < 6; i++) {
    const float x = (float)g_quick_r[i][0] * scr_w / (float)ew;
    const float y = (float)g_quick_r[i][1] * scr_h / (float)eh;
    const float w = (float)g_quick_r[i][2] * scr_w / (float)ew;
    const float h = (float)g_quick_r[i][3] * scr_h / (float)eh;
    if (w > 0 && h > 0 && sx >= x && sx < x + w && sy >= y && sy < y + h)
      return 1;
  }
  return 0;
}

/* The page-swap button is driven by R, never by a finger. This check runs
 * BEFORE the hud_active test so the shortcut-assign page's copy of the button
 * is covered too. */
static volatile long g_swap_r[4];
static volatile int g_swap_on;

void input_set_swap_rect(long x, long y, long w, long h, int enabled) {
  g_swap_on = enabled;
  g_swap_r[0] = x;
  g_swap_r[1] = y;
  g_swap_r[2] = w;
  g_swap_r[3] = h;
}

static int hud_swap_hit(float sx, float sy) {
  if (!g_swap_on)
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  const float x = (float)g_swap_r[0] * scr_w / (float)ew;
  const float y = (float)g_swap_r[1] * scr_h / (float)eh;
  const float w = (float)g_swap_r[2] * scr_w / (float)ew;
  const float h = (float)g_swap_r[3] * scr_h / (float)eh;
  return w > 0 && h > 0 && sx >= x - 4 && sx <= x + w + 4 &&
         sy >= y - 4 && sy <= y + h + 4;
}

/* The cash-shop basket (HUD control id 2) is retired: no icon, no daily-reward
 * hint, no online store. Its rect is published by hud_scan_node() from the
 * engine's own control tree; swallow every real finger touch on it BEFORE the
 * page-swap / quick-slot / nav whitelists get a chance to pass it through. */
static volatile long g_shop_r[4];
static volatile int g_shop_on;

void input_set_shop_rect(long x, long y, long w, long h, int enabled) {
  g_shop_on = enabled;
  g_shop_r[0] = x;
  g_shop_r[1] = y;
  g_shop_r[2] = w;
  g_shop_r[3] = h;
}

static int hud_shop_hit(float sx, float sy) {
  if (!g_shop_on)
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  const float x = (float)g_shop_r[0] * scr_w / (float)ew;
  const float y = (float)g_shop_r[1] * scr_h / (float)eh;
  const float w = (float)g_shop_r[2] * scr_w / (float)ew;
  const float h = (float)g_shop_r[3] * scr_h / (float)eh;
  return w > 0 && h > 0 && sx >= x - 4 && sx <= x + w + 4 &&
         sy >= y - 4 && sy <= y + h + 4;
}

/* forward: defined with the navigation tables below */
static int hud_nav_hit(float sx, float sy);

static int hud_touch_blocked(float sx, float sy) {
  if (hud_shop_hit(sx, sy)) {
    static int logs;
    if (logs < 8) {
      logs++;
      debugPrintf("input: cash-shop touch swallowed (%.0f,%.0f)\n", sx, sy);
    }
    return 1;
  }
  if (hud_swap_hit(sx, sy)) {
    static int logs;
    if (logs < 8) {
      logs++;
      debugPrintf("input: page-swap touch swallowed (%.0f,%.0f)\n", sx, sy);
    }
    return 1;
  }
  /* A settings page covers the whole screen but leaves the HUD controls in
   * the tree, so hud_active stays 1 and the HUD veto would swallow taps on
   * page controls that sit over the d-pad / attack disc. Only the rectangles
   * the navigator itself published are exempt: every other HUD area stays
   * blocked, so the stick and attack disc still never answer a finger. */
  if (input_nav_active() && hud_nav_hit(sx, sy))
    return 0;
  if (!hud_active)
    return 0;
  if (hud_quick_hit(sx, sy)) {
    static int logs;
    if (logs < 8) {
      logs++;
      debugPrintf("input: touch on quick slot passed (%.0f,%.0f)\n", sx, sy);
    }
    return 0;
  }
  for (int i = 0; i < HUD_RECT_N; i++) {
    float x, y, w, h;
    if (!hud_screen_rect(i, &x, &y, &w, &h))
      continue;
    if (sx >= x - 4 && sx <= x + w + 4 && sy >= y - 4 && sy <= y + h + 4) {
      static int logs;
      if (logs < 8) {
        logs++;
        debugPrintf("input: touch in HUD rect %d swallowed at (%.0f,%.0f)\n", i,
                    sx, sy);
      }
      return 1;
    }
  }
  return 0;
}

/* the real panel reports 1280x720 coordinates whatever the render target is */
static void poll_touchscreen(void) {
  HidTouchScreenState ts = { 0 };
  if (!hidGetTouchScreenStates(&ts, 1))
    return;

  static u32 blocked_fingers;
  u32 seen = 0;
  for (u32 i = 0; i < ts.count && i < 8; i++) {
    const HidTouchState *t = &ts.touches[i];
    const int id = 100 + (int)i;
    const float x = (float)t->x * scr_w / 1280.0f;
    const float y = (float)t->y * scr_h / 720.0f;
    const int was = (prev_fingers >> i) & 1;
    const u32 bit = 1u << i;
    if (was && (blocked_fingers & bit)) {   /* this finger is not ours */
      seen |= bit;
      continue;
    }
    if (!was && hud_touch_blocked(x, y)) {
      blocked_fingers |= bit;
      seen |= bit;
      continue;
    }
    input_touch(was ? 1 : 0, id, x, y);
    seen |= bit;
    /* the crosshair follows the real finger, so lifting the hand leaves the
     * pointer where the player last aimed instead of snapping back */
    cur_x = x;
    cur_y = y;
  }
  for (u32 i = 0; i < 8; i++) {
    const u32 bit = 1u << i;
    if (!((prev_fingers >> i) & 1) || (seen & bit))
      continue;
    if (blocked_fingers & bit) {
      blocked_fingers &= ~bit;             /* lifted, nothing was sent */
      continue;
    }
    input_touch(2, 100 + (int)i, cur_x, cur_y);
  }
  prev_fingers = seen;
}

/* ------------------------------------------------------------------------- */
/* Pad navigation over the engine's own control tree (2026-11-13 redesign)   */
/*                                                                           */
/* ONE model for every menu page. The old three-mode state machine (TOP /     */
/* BODY / STAT), the character stat-point mode, the skill and inventory       */
/* special cases and the shaped-cursor rules are all gone. Every control on   */
/* the topmost visible panel is just a focus target; A taps it exactly like   */
/* a finger would. main.c publishes only real clickable controls and removes  */
/* anything covered by a panel, so focus is trapped on the visible layer.     */
/*                                                                           */
/*   D-pad / left stick : spatial move to the nearest target; HOLD repeats.   */
/*                        A 2-D grid stops at its edges (Xbox XAG 112).      */
/*   A                  : activate = tap the focused control                  */
/*   B                  : one level back; focus returns to the control that   */
/*                        opened the panel (restore-to-opener)               */
/*   L / R / right stick: previous / next top-level tab, applied at once      */
/*   X / Y              : stay free for gameplay; never used here.            */
/*                                                                           */
/* Reference models: Zelda BOTW (left stick picks a grid cell, right stick    */
/* flips tabs, A confirm, B back), Genshin (d-pad picks items, LB/RB switch   */
/* modules), Xbox Accessibility XAG 112 (2-D grids stop at edges, every       */
/* screen needs a persistent way back), and the general rule that focus is    */
/* trapped in the top screen and restored to its opener.                       */
/* ------------------------------------------------------------------------- */
static NavItem nav_tabs[NAV_MAX_TABS], nav_items[NAV_MAX_ITEMS], nav_back;
static int nav_present, nav_ntabs, nav_nitems, nav_page_kind;
static int nav_tab_x_valid;     /* tab cursor is a screen coordinate, not an index */
static long nav_tab_x;
static int nav_idx;             /* focused control in nav_items[] */
static int nav_dirty;           /* the tree was rebuilt after a tap */
static int nav_repeat_dir, nav_repeat_wait;
static int nav_grace;           /* frames the pad stays owned during a rebuild */
/* A tab tap can be swallowed by the engine (the page does not rebuild), while
 * our own cursor has already moved on. That is what made "left once does
 * nothing, left again wraps to the settings tab". main.c now passes the live
 * page layer's control id (rebuilt pages get a NEW id), so we can confirm the
 * switch really happened before keeping the cursor. */
static int nav_page_serial;
static int nav_tab_pending, nav_tab_wait, nav_tab_retry;
static int nav_tab_serial_before;
static long nav_tab_prev_x, nav_tab_target_x;
static unsigned long nav_tab_fp_before;
static void nav_tab_confirm(void);

/* Content fingerprint: two different tabs can publish the same number of
 * controls, so the id alone is not enough -- and the engine may reuse a page
 * object. Combine the count with the shape/position of the first and last
 * focus item. Scrolling only changes y on the skill page, and a tab switch is
 * never issued mid-scroll, so this stays stable during the confirmation. */
static unsigned long nav_content_fp(void) {
  unsigned long h = (unsigned long)nav_nitems;
  if (nav_nitems > 0) {
    const NavItem *f = &nav_items[0];
    h = h * 31u + (unsigned long)f->kind;
    h = h * 31u + (unsigned long)(f->x + f->y * 3 + f->w + f->h * 7);
    if (nav_nitems > 1) {
      const NavItem *l = &nav_items[nav_nitems - 1];
      h = h * 31u + (unsigned long)(l->x + l->y * 3 + l->w * 5 + l->kind);
    }
  }
  return h;
}
static int nav_panel_closed;    /* last B already closed the top panel */
static int nav_opener_valid;
static long nav_opener_x, nav_opener_y;  /* control that opened the panel */
/* Free cursor for scrollable regions: the skill tree and the description
 * pages are ControlScroll / drawn content, not ordinary controls, so no
 * focus list can reach them. Last-device-wins: right stick -> cursor,
 * d-pad -> focus. RT + right stick injects a real drag so the engine's
 * scroll handlers (and input_take_drag_dy) see it. */
static float nav_cur_x, nav_cur_y;
static int nav_cursor_ready, nav_use_cursor, nav_drag_active;
/* restore-to-opener: remember the control that opened a panel, plus a small
 * fingerprint of the page so we can tell "panel closed" from "panel opened" */
static int nav_saved_valid, nav_saved_n;
static long nav_saved_x, nav_saved_y, nav_saved_first_x, nav_saved_first_y;

/* Highest-value default: on the title / class pages the primary action is one
 * press away; elsewhere the first control of the visible layer wins. */
static int nav_default_index(void) {
  /* On the skill tree the icons are the primary targets, so the first one
   * gets focus instead of the page's surrounding labels. */
  if (nav_page_kind == NAV_PAGE_SETTINGS)
    for (int i = 0; i < nav_nitems; i++)
      if (nav_items[i].kind == NAV_K_SKILL)
        return i;
  if (nav_page_kind == NAV_PAGE_TITLE || nav_page_kind == NAV_PAGE_CLASS)
    for (int i = 0; i < nav_nitems; i++)
      if (nav_items[i].kind == NAV_K_START ||
          nav_items[i].kind == NAV_K_TITLE_START)
        return i;
  return 0;
}

static int nav_nearest(long x, long y) {
  int bi = 0;
  long best = -1;
  for (int i = 0; i < nav_nitems; i++) {
    const long dx = nav_items[i].x + nav_items[i].w / 2 - x;
    const long dy = nav_items[i].y + nav_items[i].h / 2 - y;
    const long d = dx * dx + dy * dy;
    if (best < 0 || d < best) {
      best = d;
      bi = i;
    }
  }
  return bi;
}

void input_set_nav(int present, const NavItem *tabs, int ntabs,
                   const NavItem *items, int nitems, int page_serial,
                   int page_kind, const NavItem *back) {
  nav_page_serial = page_serial;   /* live page-layer control id */
  const int entering = present && !nav_present;
  const int changed = present && page_kind != nav_page_kind;
  nav_present = present;
  nav_page_kind = page_kind;
  nav_ntabs = ntabs > NAV_MAX_TABS ? NAV_MAX_TABS : (ntabs > 0 ? ntabs : 0);
  nav_nitems = nitems > NAV_MAX_ITEMS ? NAV_MAX_ITEMS
                                      : (nitems > 0 ? nitems : 0);
  if (nav_ntabs)
    memcpy(nav_tabs, tabs, (size_t)nav_ntabs * sizeof(*tabs));
  if (nav_nitems)
    memcpy(nav_items, items, (size_t)nav_nitems * sizeof(*items));
  nav_back = back ? *back : (NavItem){ 0 };
  if (!present) {
    if (nav_drag_active) {
      input_touch(2, ID_PAD, nav_cur_x, nav_cur_y);
      nav_drag_active = 0;
    }
    /* A tab switch rebuilds the page and can leave no controls for several
     * frames. Do NOT reset the tab cursor here -- that was why "right then
     * left" jumped to a different tab. Keep owning the pad keys until the
     * tree comes back so R cannot leak into the field's quick-slot swap. */
    nav_idx = nav_dirty = 0;
    nav_repeat_dir = nav_repeat_wait = 0;
    nav_saved_valid = 0;
    return;
  }
  nav_grace = 0;
  nav_tab_confirm();
  if (nav_ntabs > 0 && !nav_tab_x_valid) {
    nav_tab_x = nav_tabs[0].x + nav_tabs[0].w / 2;
    nav_tab_x_valid = 1;
  }
  if (entering || changed) {
    nav_dirty = 0;
    nav_saved_valid = 0;
    nav_panel_closed = 0;
    nav_opener_valid = 0;
    nav_idx = nav_default_index();
    nav_cur_x = (float)scr_w * 0.5f;
    nav_cur_y = (float)scr_h * 0.5f;
    nav_cursor_ready = 1;
    nav_use_cursor = 0;
    debugPrintf("nav: page %s kind %d (%d tabs, %d controls)\n",
                entering ? "up" : "changed", page_kind, nav_ntabs, nav_nitems);
  } else if (nav_dirty) {
    nav_dirty = 0;
    /* If the control list is the one we had before A, the panel that A opened
     * has just closed: put the focus back on the control that opened it.
     * Otherwise a new layer took over and its first control gets focus. */
    const int back_home = nav_saved_valid && nav_nitems > 0 &&
                          nav_nitems == nav_saved_n &&
                          nav_items[0].x + nav_items[0].w / 2 == nav_saved_first_x &&
                          nav_items[0].y + nav_items[0].h / 2 == nav_saved_first_y;
    nav_idx = back_home ? nav_nearest(nav_saved_x, nav_saved_y)
                        : nav_default_index();
    if (back_home)
      nav_saved_valid = 0;
  }
  if (nav_idx >= nav_nitems)
    nav_idx = 0;
}

int input_nav_active(void) {
  /* A tab page can legitimately have no collectable controls (a task detail
   * page with one wide row). The tabs are still there, so keep owning L/R. */
  return (nav_present && (nav_nitems > 0 || nav_ntabs > 0)) ||
         nav_grace > 0;
}

static void nav_grace_tick(void) {
  if (nav_grace > 0)
    nav_grace--;
}

/* Real screen touches on a menu control must still reach the engine. Compare
 * against the navigator's own rects with the same scaling nav_tap() uses. */
static int hud_nav_hit(float sx, float sy) {
  if (!input_nav_active())
    return 0;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  for (int pass = 0; pass < 2; pass++) {
    const NavItem *arr = pass ? nav_items : nav_tabs;
    const int n = pass ? nav_nitems : nav_ntabs;
    for (int i = 0; i < n; i++) {
      const float x = (float)arr[i].x * scr_w / (float)ew;
      const float y = (float)arr[i].y * scr_h / (float)eh;
      const float w = (float)arr[i].w * scr_w / (float)ew;
      const float h = (float)arr[i].h * scr_h / (float)eh;
      if (w > 0 && h > 0 && sx >= x && sx < x + w && sy >= y && sy < y + h)
        return 1;
    }
  }
  return 0;
}

/* One highlight, always on the focused control. The gold shape is a visual
 * cue only -- it has no effect on how the pad drives the page. */
int input_nav_highlight(long *x, long *y, long *w, long *h, int *shape) {
  if (!nav_present || nav_nitems <= 0 || nav_idx >= nav_nitems)
    return 0;
  const NavItem *it = &nav_items[nav_idx];
  int sh = it->shape;
  if (it->w > it->h * 2)
    sh = NAV_SHAPE_PILL;
  *x = it->x; *y = it->y; *w = it->w; *h = it->h;
  if (shape)
    *shape = sh;
  return it->w > 0 && it->h > 0;
}

/* Engine layout units -> screen pixels, then the public one-shot tap that
 * corner_tap()/confirm_tap() use. tap_pump() owns the finger for 8 frames. */
static void nav_tap(const NavItem *it) {
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  /* The class page sits over the stale save rows; main.c picks tx/ty inside
   * the SAME control but clear of those rows so the tap still lands. */
  const long px = it->tx > 0 ? it->tx : it->x + it->w / 2;
  const long py = it->ty > 0 ? it->ty : it->y + it->h / 2;
  const int sx = (int)(px * (float)scr_w / (float)ew);
  const int sy = (int)(py * (float)scr_h / (float)eh);
  input_tap_at(sx, sy);
  static int logs;
  if (logs < 24) {
    logs++;
    debugPrintf("nav: tap (%ld,%ld %ldx%ld) -> screen (%d,%d)\n",
                it->x, it->y, it->w, it->h, sx, sy);
  }
}

/* Tab cursor is a screen coordinate, not an index into a list that the engine
 * rebuilds on every switch. Left/right look for the nearest tab on that side
 * of the last one tapped and wrap to the opposite end. This is symmetric by
 * construction, so "right then left" always comes back to where it started,
 * even if the control tree was rebuilt in between. */
static void nav_tap_tab_at(long x) {
  if (!nav_ntabs)
    return;
  int bi = 0;
  long bd = -1;
  for (int i = 0; i < nav_ntabs; i++) {
    const long cx = nav_tabs[i].x + nav_tabs[i].w / 2;
    const long d = cx > x ? cx - x : x - cx;
    if (bd < 0 || d < bd) { bd = d; bi = i; }
  }
  nav_tap(&nav_tabs[bi]);
}

static void nav_switch_tab(int d) {
  if (!nav_ntabs)
    return;
  if (!nav_tab_x_valid) {
    nav_tab_x = nav_tabs[0].x + nav_tabs[0].w / 2;
    nav_tab_x_valid = 1;
  }
  const long prev_x = nav_tab_x;
  int target = -1;
  long best = 0;
  for (int i = 0; i < nav_ntabs; i++) {
    const long x = nav_tabs[i].x + nav_tabs[i].w / 2;
    if (d > 0 ? (x > nav_tab_x && (target < 0 || x < best))
              : (x < nav_tab_x && (target < 0 || x > best))) {
      target = i;
      best = x;
    }
  }
  if (target < 0) {
    /* wrap around to the far end of the tab row */
    for (int i = 0; i < nav_ntabs; i++) {
      const long x = nav_tabs[i].x + nav_tabs[i].w / 2;
      if (target < 0 || (d > 0 ? x < best : x > best)) {
        target = i;
        best = x;
      }
    }
  }
  if (target < 0)
    target = 0;
  nav_tab_x = nav_tabs[target].x + nav_tabs[target].w / 2;
  nav_tap(&nav_tabs[target]);
  nav_dirty = 1;
  nav_saved_valid = 0;
  nav_panel_closed = 0;
  nav_opener_valid = 0;
  /* The engine rebuilds the page after a tab tap and may publish no controls
   * for a few frames. Keep L/R owned by the navigator during that gap so R
   * cannot fall through to the field's quick-slot swap. Cleared as soon as
   * input_set_nav() sees the rebuilt page. */
  nav_grace = 20;
  nav_tab_prev_x = prev_x;
  nav_tab_target_x = nav_tab_x;
  nav_tab_serial_before = nav_page_serial;
  nav_tab_fp_before = nav_content_fp();
  nav_tab_wait = 18;
  nav_tab_retry = 1;
  nav_tab_pending = 1;
  debugPrintf("nav: tab %s -> x=%ld (%d/%d, page id %d)\n",
              d > 0 ? "right" : "left", nav_tab_x, target + 1, nav_ntabs,
              nav_page_serial);
}

static void nav_tab_confirm(void) {
  if (!nav_tab_pending)
    return;
  const int id_changed = nav_page_serial &&
                         nav_page_serial != nav_tab_serial_before;
  const int content_changed = nav_content_fp() != nav_tab_fp_before;
  if (id_changed || content_changed) {
    debugPrintf("nav: tab confirmed (id %d->%d, content %s)\n",
                nav_tab_serial_before, nav_page_serial,
                content_changed ? "changed" : "same");
    nav_tab_pending = 0;
    return;
  }
  if (--nav_tab_wait > 0)
    return;
  if (nav_tab_retry > 0) {
    nav_tab_retry--;
    nav_tab_wait = 18;
    nav_tap_tab_at(nav_tab_target_x);
    debugPrintf("nav: tab tap ignored, retrying x=%ld\n", nav_tab_target_x);
    return;
  }
  /* Never leave the cursor one page ahead of the engine: put it back so the
   * next L/R repeats the SAME tab instead of wrapping to the far end. */
  nav_tab_x = nav_tab_prev_x;
  nav_tab_pending = 0;
  debugPrintf("nav: tab failed, cursor restored to x=%ld\n", nav_tab_x);
}

static void nav_scroll_request(int dir, long fx, long fy);

/* Spatial focus movement: score the nearest centre in the requested
 * direction, weighting the off-axis distance heavily. No candidate on that
 * side means the edge of the grid -- stop there, don't wrap (XAG 112). */
static void nav_move(int dx, int dy) {
  if (nav_idx >= nav_nitems)
    return;
  const NavItem *c = &nav_items[nav_idx];
  const long cx = c->x + c->w / 2, cy = c->y + c->h / 2;
  int best = -1;
  long best_score = 0;
  for (int i = 0; i < nav_nitems; i++) {
    if (i == nav_idx)
      continue;
    const NavItem *o = &nav_items[i];
    const long ax = o->x + o->w / 2 - cx;
    const long ay = o->y + o->h / 2 - cy;
    long score;
    if (dx) {
      if ((dx > 0 && ax <= 0) || (dx < 0 && ax >= 0))
        continue;
      score = (ax < 0 ? -ax : ax) + 3 * (ay < 0 ? -ay : ay);
    } else {
      if ((dy > 0 && ay <= 0) || (dy < 0 && ay >= 0))
        continue;
      score = (ay < 0 ? -ay : ay) + 3 * (ax < 0 ? -ax : ax);
    }
    if (best < 0 || score < best_score) {
      best = i;
      best_score = score;
    }
  }
  int wrapped = 0;
  if (best < 0) {
    /* No control in that direction: wrap to the far end of the same row /
     * column instead of dead-ending at a grid edge. The user asked for free
     * movement; XAG 112's "stop at the edge" made skill grids feel stuck. */
    wrapped = 1;
    for (int i = 0; i < nav_nitems; i++) {
      if (i == nav_idx)
        continue;
      const NavItem *o = &nav_items[i];
      const long ax = o->x + o->w / 2 - cx;
      const long ay = o->y + o->h / 2 - cy;
      long score;
      if (dx) {
        if ((dx > 0 && ax >= 0) || (dx < 0 && ax <= 0))
          continue;
        score = -(ax < 0 ? -ax : ax) + 3 * (ay < 0 ? -ay : ay);
      } else {
        if ((dy > 0 && ay >= 0) || (dy < 0 && ay <= 0))
          continue;
        score = -(ay < 0 ? -ay : ay) + 3 * (ax < 0 ? -ax : ax);
      }
      if (best < 0 || score < best_score) {
        best = i;
        best_score = score;
      }
    }
  }
  if (best >= 0) {
    nav_idx = best;
    /* Keep the focused skill readable: if it sits outside the nearest scroll
     * viewport, nudge the content until it comes back into view. */
    const NavItem *f = &nav_items[nav_idx];
    const long fx = f->x + f->w / 2, fy = f->y + f->h / 2;
    long vx, vy, vw, vh;
    if (ui_scroll_viewport(fx, fy, &vx, &vy, &vw, &vh)) {
      const long top = vy + f->h / 2 + 8;
      const long bot = vy + vh - f->h / 2 - 8;
      if (fy < top)
        nav_scroll_request(1, fx, fy);
      else if (fy > bot)
        nav_scroll_request(-1, fx, fy);
    }
    static int mlogs;
    if (mlogs < 20) {
      mlogs++;
      debugPrintf("nav: focus %d -> %d (dir %d,%d%s)\n",
                  nav_idx, best, dx, dy, wrapped ? " wrap" : "");
    }
  }
}

static void nav_step(int d) {
  switch (d) {
    case -1: nav_move(-1, 0); break;
    case  1: nav_move(+1, 0); break;
    case -2: nav_move(0, -1); break;
    case  2: nav_move(0, +1); break;
    default: break;
  }
}

/* Right stick: up/down scrolls the nearest scroll viewport, left/right flicks
 * tabs. A scroll is a REAL drag spread over four frames -- the engine samples
 * the pointer once per nativeProcess, so a down/up inside one frame is
 * invisible. input_touch() also accumulates drag_acc, which is what
 * desc_scroll_tick() consumes for the drawn-text description pages. */
static int nav_scroll_frames, nav_scroll_dir;
static float nav_scroll_sx, nav_scroll_sy;

static void nav_scroll_request(int dir, long fx, long fy) {
  long vx, vy, vw, vh;
  if (nav_scroll_frames > 0)
    return;
  if (!ui_scroll_viewport(fx, fy, &vx, &vy, &vw, &vh))
    return;
  int ew = input_engine_screen_w(), eh = input_engine_screen_h();
  if (ew <= 0) ew = 1137;
  if (eh <= 0) eh = 640;
  nav_scroll_sx = (float)(vx + vw / 2) * (float)scr_w / (float)ew;
  nav_scroll_sy = (float)(vy + vh / 2) * (float)scr_h / (float)eh;
  nav_scroll_dir = dir;
  nav_scroll_frames = 4;
}

static void nav_scroll_pump(void) {
  if (nav_scroll_frames <= 0)
    return;
  /* large enough that the engine sees a drag, not a tap */
  const float step = 60.0f * (float)nav_scroll_dir;
  const int k = 4 - nav_scroll_frames;
  if (k == 0)
    input_touch(0, ID_PAD, nav_scroll_sx, nav_scroll_sy);
  else if (k < 3)
    input_touch(1, ID_PAD, nav_scroll_sx,
                nav_scroll_sy + step * (float)k);
  else
    input_touch(2, ID_PAD, nav_scroll_sx, nav_scroll_sy + step * 2.0f);
  nav_scroll_frames--;
}

static void nav_cursor_tick(u64 down) {
  (void)down;
  /* Right stick is SCROLL ONLY. Its x axis used to flick tabs, which was too
   * easy to trigger while scrolling the skill tree. L / R own tab switching
   * exclusively now. */
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  const float ay = (float)rs.y / 32767.0f;
  if (ay > 0.25f || ay < -0.25f) {
    long fx = 568, fy = 320;
    if (nav_nitems > 0 && nav_idx < nav_nitems) {
      fx = nav_items[nav_idx].x + nav_items[nav_idx].w / 2;
      fy = nav_items[nav_idx].y + nav_items[nav_idx].h / 2;
    }
    /* stick up -> look higher (drag down); stick down -> look lower */
    nav_scroll_request(ay > 0 ? 1 : -1, fx, fy);
  }
}

int input_nav_cursor(int *x, int *y) {
  if (!input_nav_active() || !nav_cursor_ready || !nav_use_cursor)
    return 0;
  *x = (int)nav_cur_x;
  *y = (int)nav_cur_y;
  return 1;
}

static void nav_update(u64 down, u64 pressed) {
  if (!input_nav_active())
    return;
  const int settings = nav_page_kind == NAV_PAGE_SETTINGS;
  const int ready = nav_present && nav_nitems > 0;

  /* A UIPopupMsg box is modal on EVERY page -- including all six settings
   * tabs (save-ok / confirm, yes-no, ...). The old guard was limited to
   * front-end pages on the assumption that a settings page never had a box;
   * there the press fell through to "tap the focused control", so the player
   * had to walk the cursor onto the confirm button. While a box is up:
   * A = confirm/yes, B = cancel/no, and every other pad key is frozen so the
   * focus cannot wander behind the box. popup_on() reads the engine's own box
   * base pointer, so it cannot fire on a stale or already closed box. */
  if (popup_on()) {
    if (pressed & (HidNpadButton_A | HidNpadButton_B))
      ab_press((pressed & HidNpadButton_A) != 0, AB_POPUP);
    return;
  }
  /* UI_PopupProcess info boxes have no UIPopupMsg base, so they need the
   * control-tree lookup. Keep that path on the front-end pages where it was
   * verified; settings pages carry scroll+button panels that must not be
   * mistaken for a modal confirm. */
  if (!settings && (pressed & (HidNpadButton_A | HidNpadButton_B))) {
    long bx, by;
    if (ui_confirm_button(&bx, &by)) {
      ab_press((pressed & HidNpadButton_A) != 0, ab_layer());
      return;
    }
  }

  /* L/R switch tabs immediately. The right stick belongs to the free cursor:
   * scrollable regions (skill tree, description pages) are drawn content, not
   * controls, so a focus list can never reach them. */
  if (settings && nav_ntabs > 0 && !nav_tab_pending) {
    if (pressed & HidNpadButton_L) { nav_switch_tab(-1); return; }
    if (pressed & HidNpadButton_R) { nav_switch_tab(+1); return; }
  }

  if (pressed & HidNpadButton_B) {
    /* Back out ONE level. Prefer the panel's own close cross; many scroll
     * detail panels have no such control (their contents are drawn text, not
     * controls), so fall back to tapping the control that opened the panel:/r/n     * the engine treats a second tap on an item cell as "close the detail".
     * Only when neither applies do we fall through to the page's x handler.
     * nav_panel_closed makes the NEXT B leave the menu, so a docked panel
     * cannot trap the key forever. */
    long dx, dy;
    int closed_one = 0;
    if (settings && !nav_panel_closed && ui_panel_close_button(&dx, &dy)) {
      closed_one = 1;
      debugPrintf("nav: B closes top panel at (%ld,%ld)\n", dx, dy);
    } else if (settings && !nav_panel_closed && nav_opener_valid) {
      dx = nav_opener_x;
      dy = nav_opener_y;
      closed_one = 1;
      static int tlog;
      if (tlog < 8) {
        tlog++;
        debugPrintf("nav: B toggles opener at (%ld,%ld)\n", dx, dy);
      }
    }
    if (closed_one) {
      int ew = input_engine_screen_w(), eh = input_engine_screen_h();
      if (ew <= 0) ew = 1137;
      if (eh <= 0) eh = 640;
      input_tap_at((int)((float)dx * (float)scr_w / (float)ew),
                   (int)((float)dy * (float)scr_h / (float)eh));
      nav_panel_closed = 1;
    } else {
      if (settings)
        ab_press(0, ab_layer());   /* the page's own x / back handler */
      else if (nav_back.w > 0)
        nav_tap(&nav_back);        /* front-end pages: their return arrow */
      nav_panel_closed = 0;
      static int mlog;
      if (mlog < 8) {
        mlog++;
        debugPrintf("nav: B backs out of the menu\n");
      }
    }
    nav_dirty = 1;
    return;
  }

  /* During a rebuild only the tab keys and B stay live; A and the d-pad would
   * otherwise tap controls that no longer exist. */
  nav_cursor_tick(down);
  if (!ready) {
    /* Scrollable pages can have zero ordinary controls. The cursor must
     * still be able to click there. */
    if ((pressed & HidNpadButton_A) && nav_use_cursor &&
        nav_cursor_ready && !nav_drag_active) {
      input_tap_at((int)nav_cur_x, (int)nav_cur_y);
      nav_dirty = 1;
    }
    return;
  }

  if (pressed & HidNpadButton_A) {
    if (nav_use_cursor && nav_cursor_ready && !nav_drag_active) {
      input_tap_at((int)nav_cur_x, (int)nav_cur_y);
      nav_dirty = 1;
      nav_panel_closed = 0;
      static int clog;
      if (clog < 8) {
        clog++;
        debugPrintf("nav: A taps cursor at (%d,%d)\n",
                    (int)nav_cur_x, (int)nav_cur_y);
      }
      return;
    }
    const NavItem *it = &nav_items[nav_idx];
    /* remember the opener so B can restore focus when the layer closes */
    nav_saved_valid = 1;
    nav_saved_x = it->x + it->w / 2;
    nav_saved_y = it->y + it->h / 2;
    nav_opener_valid = 1;
    nav_opener_x = nav_saved_x;
    nav_opener_y = nav_saved_y;
    nav_saved_n = nav_nitems;
    if (nav_nitems > 0) {
      nav_saved_first_x = nav_items[0].x + nav_items[0].w / 2;
      nav_saved_first_y = nav_items[0].y + nav_items[0].h / 2;
    }
    nav_tap(it);                 /* A is ALWAYS just a tap, on every page */
    nav_dirty = 1;
    nav_panel_closed = 0;
    return;
  }

  /* D-pad or left stick, with hold-to-repeat. A direction that is newly
   * pressed moves once; holding it repeats after a short delay. */
  int want = 0;
  if (down & HidNpadButton_Left)       want = -1;
  else if (down & HidNpadButton_Right) want =  1;
  else if (down & HidNpadButton_Up)    want = -2;
  else if (down & HidNpadButton_Down)  want =  2;
  if (!want) {
    const HidAnalogStickState ls = padGetStickPos(&pad, 0);
    if (ls.x < -16000)      want = -1;
    else if (ls.x > 16000)  want =  1;
    else if (ls.y > 16000)  want = -2;  /* HID up is positive */
    else if (ls.y < -16000) want =  2;
  }
  if (want) {
    nav_use_cursor = 0;          /* last device wins */
    if (want != nav_repeat_dir) {
      nav_repeat_dir = want;
      nav_repeat_wait = 18;
      nav_step(want);
    } else if (nav_repeat_wait > 0) {
      nav_repeat_wait--;
    } else {
      nav_repeat_wait = 6;
      nav_step(want);
    }
  } else {
    nav_repeat_dir = 0;
  }
}

void input_update(void) {
  nav_grace_tick();
  nav_scroll_pump();
  tap_pump();
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 pressed = down & ~pad_prev;
  const u64 released = ~down & pad_prev;
  const int rt = (down & HidNpadButton_ZR) != 0;   /* Xbox RT / NS ZR */

  /* R shoulder replaces the on-screen page-swap button. Global on purpose:
   * the field HUD and the shortcut-assign page both use it, and neither draws
   * that button's icon any more. The stick and A never read this key. */
  /* R swaps quick-slot pages in the field; on a settings page it changes tab */
  if ((pressed & HidNpadButton_R) && !input_nav_active())
    quick_swap_fire();

  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);

  /* The in-game HUD has its own control tree. While it is live the pad drives
   * those controls directly (+/-/A/stick). Menus, dialogs and the title screen
   * have no HUD rects, so they keep the focus box / free cursor below. */
  const int hud_mode = input_hud_present();
  /* Pad -> quick slots, keyed by SCREEN POSITION, not by shortcut data: R may
   * exchange the four right-hand slots between pages, but the six buttons and
   * therefore these keys stay where they are. RT (ZR) + X/Y/A/B = up/left/
   * right/down of the right-hand cross; d-pad left/right = the lower-left
   * pair. Every press/release goes straight onto the control objects. */
  {
    static int key_down[6];
    /* Slot order is up / left / right / down. The 14:23 build tried to
     * out-think Eden's qt-config.ini (Xbox A -> NS B, Xbox B -> NS A) and
     * used X, Y, B, A; play testing showed the physical A/B were swapped.
     * Use the plain NS order the user asked for: X, Y, A, B. */
    const u64 face[4] = { HidNpadButton_X, HidNpadButton_Y,
                          HidNpadButton_A, HidNpadButton_B };
    const u64 dpad[2] = { HidNpadButton_Left, HidNpadButton_Right };
    /* A settings page leaves the in-field HUD controls in the tree, so
     * hud_mode stays 1 there; without this guard X/Y/A/B and d-pad L/R would
     * fire quick slots while the navigator is trying to use the same keys. */
    const int active = hud_mode && cfg.quick_layout && !input_nav_active();
    for (int s = 0; s < 6; s++) {
      const u64 bit = s < 4 ? face[s] : dpad[s - 4];
      const int now = active && (down & bit) != 0 && (s >= 4 || rt);
      if (now != key_down[s]) {
        key_down[s] = now;
        quick_slot_fire(s, now);
      }
    }
  }
  if (hud_mode != hud_prev_mode && cur_down) {
    /* the finger belonged to the other mode -- let it go before the new owner
     * presses it, or the engine sees a press with no matching release */
    cur_down = 0;
    input_touch(2, ID_PAD, cur_x, cur_y);
  }
  /* hud_update() runs in BOTH modes: with the HUD gone its only job is the
   * menu pair (A = confirm, B = back), which used to sit behind the hud_mode
   * branch below and therefore never ran at all. */
  hud_update(down, pressed, ls);

  /* The settings-page navigator owns the pad while its page is up. It MUST
   * run before -- and independently of -- the hud_mode branch below: opening
   * a settings page does not remove the in-field HUD controls from the tree
   * (ids 20/21 are still there), so hud_active stays 1 and `!hud_mode` is
   * false. Nesting nav_update() inside that branch meant it never ran at all
   * and every key looked dead. This was the whole "no key responds" bug. */
  if (input_nav_active())
    nav_update(down, pressed);
  else if (!hud_mode) {
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
    /* R is no longer a focus step: it belongs to the quick-slot page swap.
     * Right/Up/StickR still cycle the focus list. */
    if (pressed & (HidNpadButton_Right | HidNpadButton_Up |
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
  /* Once a scene has a key handler registered, A already reaches the game as a
   * real key press; injecting a touch on top of it would double-fire (and the
   * touch would land wherever the focus box happens to sit). The touch path is
   * only the fallback for screens that register no handler at all. */
  const int want_down = !rt && !menu_has_handler() &&
                        (down & HidNpadButton_A) != 0;
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
  }  /* !hud_mode && !nav */

  (void)released;

  /* a real finger on the panel wins over the pad */
  poll_touchscreen();

  hud_prev_mode = hud_mode;
  pad_prev = down;
}

void input_shutdown(void) {
  if (cur_down) {
    cur_down = 0;
    emit(EV_UP, ID_PAD, cur_x, cur_y, 0);
  }
  last_touch_id = -1;
}
