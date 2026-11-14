#ifndef INPUT_H
#define INPUT_H

/* Inotia 4 is a touch-only game: every control is drawn on screen and the
 * engine reads pointers. The Switch pad is therefore mapped onto synthetic
 * touch points, and the real touchscreen is forwarded unchanged. */

typedef struct {
  int inject;      /* mirror touches into the engine queue (see input.c) */
  int touch_x_pct; /* virtual dpad origin, percent of the view */
  int touch_y_pct;
  int button_pct;  /* action area radius, percent of the view */
} InputConfig;

struct so_module;

void input_init(int screen_w, int screen_h, struct so_module *mod);
/* resolve the engine's input queue once libgame.so is loaded */
void input_bind(struct so_module *mod);
/* must be called after so_finalize(): only then is that memory readable */
void input_set_base(void *virtbase);
void input_configure(const InputConfig *cfg);
void input_update(void);
void input_shutdown(void);

/* raw pointer feed, also used by the pad mapping */
void input_touch(int action, int id, float x, float y);

/* synthetic tap: press and release at once (screen coordinates) */
void input_tap_at(int x, int y);

/* where the pad's virtual finger is, so it can be drawn */
void input_cursor(int *x, int *y);

/* 1 while the pad is cycling menu targets instead of aiming freely */
int input_focus_mode(void);

/* top-left corner and size of the highlight box, in view pixels from the top */
void input_focus_box(int *x, int *y, int *w, int *h);

int  input_injection_active(void);

/* Vertical travel of the finger since the last call (screen pixels, positive =
 * moved down), drained on every call. Pages whose own scroll control never sees
 * the input (the skill description) use this to drive their scroll themselves. */
int input_take_drag_dy(void);

/* 1 while a finger is down, with the point it went down at (screen pixels).
 * A page only scrolls for a drag that started inside it, so that dragging the
 * skill tree does not also move the description next to it. */
int input_drag_start(float *x, float *y);

/* what the engine believes its screen size is (bring-up diagnostics) */
int input_engine_screen_w(void);
unsigned char *input_engine_virtbase(void);
int input_engine_screen_h(void);
/* the engine's live touch count (0 == no finger down) */
int input_engine_touch_count(void);

/* ------------------------------------------------------------------------- */
/* in-game HUD -> pad mapping                                                */
/*                                                                           */
/* The HUD rects are read from the engine's own control tree every few frames,
 * in engine layout units -- no screenshot measuring, no hard-coded pixels.
 * main.c calls begin/end around one walk of the tree; input.c converts the
 * rects through SGL_GetScreenWidth/Height when it injects a touch.           */
enum {
  HUD_BTN_PLUS = 0,   /* control id 1:  first button in the left column  */
  HUD_BTN_MINUS,      /* control id 3:  the gear                          */
  HUD_BTN_ACTION,     /* control id 20: the big disc, lower right         */
  HUD_DPAD,           /* control id 21: the virtual direction pad          */
  HUD_RECT_N
};
void input_hud_begin(void);
void input_set_hud_rect(int which, long x, long y, long w, long h);
/* the control object itself, so + / - / A can be fired on it directly instead
 * of through the single touch pointer (see hud_action in input.c) */
void input_set_hud_obj(int which, void *obj);
void input_hud_end(void);
int  input_hud_present(void);
/* Six quick-slot button rects in engine units (x,y,w,h). Real screen touches
 * inside them stay live even where they overlap the blocked d-pad / action
 * disc rects, so a shortcut can be assigned to those buttons. enabled = 0
 * turns the exemption off. The pad's stick and A do not use screen touches. */
void input_set_quick_rects(const long r[6][4], int enabled);
/* The page-swap button (engine rect 1059,313 75x75, same spot on the
 * shortcut-assign page): it must never answer a real screen touch. */
void input_set_swap_rect(long x, long y, long w, long h, int enabled);
/* R shoulder replaces that button. Implemented in main.c (data swap). */
void quick_swap_fire(void);
/* Fire a press/release straight on one control object, using the same
 * SetSelectedControl / SetReleaseEvent path as the HUD's + / - / A buttons.
 * ex,ey are engine layout coordinates. Returns -1 if the object is missing. */
int input_fire_control(void *obj, long ex, long ey, int press);
/* Pad -> six quick slots: implemented in main.c, called from input_update().
 * slot 0..3 = right-hand cross (X/Y/A/B with RT), 4/5 = lower-left (dpad L/R). */
void quick_slot_fire(int slot, int press);
/* 1 while a direction is being driven on the pad (used by the pad-zone probe) */
int  input_pad_held(void);

/* The × / back / skip button of the page on top, found in the engine's own
 * control tree. Returns its centre in engine units.
 * Return: 2 = a wide skip bar in the top-right, 1 = a × in the top-left,
 *         0 = nothing found. */
int  ui_corner_button(long *cx, long *cy);

/* The 确认 button of a UI_PopupProcess info box (路障, NPC dialogue, ...),
 * found by control type + tree shape in the live tree. Returns 1 and writes
 * its centre in engine units, or 0 when no such box is up. */
int  ui_confirm_button(long *cx, long *cy);
/* Full-screen "tap anywhere to continue" control of a story dialogue, found
 * by event proc in the live tree. A advances it; Y never does. */
int  ui_dialog_button(long *cx, long *cy);
/* Close cross of the topmost detail panel, so B can back out one level. */
int  ui_panel_close_button(long *cx, long *cy);

/* ---- pad navigation over the engine's own control tree ------------------ */
/* main.c walks the control tree and publishes the tappable controls of the
 * page on top. input.c turns that into a highlight + a pad state machine.
 * All rectangles are the engine's own absolute layout units (1137x640 here)
 * -- never measured off a screenshot. */
#define NAV_MAX_TABS   8
#define NAV_MAX_ITEMS 72

/* Which family of page the live control tree is showing. */
enum {
  NAV_PAGE_NONE = 0,
  NAV_PAGE_SETTINGS,   /* the six-tab in-game menu: L/R + A + B             */
  NAV_PAGE_TITLE,      /* 环境设置 / 开始游戏 / 更多游戏                    */
  NAV_PAGE_SAVE,       /* the three save rows                               */
  NAV_PAGE_CLASS       /* six class badges + this page's own 开始游戏        */
};

enum {
  NAV_K_PLAIN = 0,
  NAV_K_TAB,            /* one of the six top-row page tabs                  */
  NAV_K_TITLE_BTN,      /* 环境设置 / 更多游戏                               */
  NAV_K_TITLE_START,    /* title page's 开始游戏 (initial cursor)            */
  NAV_K_SAVE_ROW,       /* one of the three save rows                        */
  NAV_K_CLASS,          /* one of the six class badges                       */
  NAV_K_START,          /* class page's OWN start-game button            */
  NAV_K_SKILL           /* a skill icon in the scrollable skill tree      */
};
/* Shape of the control under the cursor, so the highlight follows the real
 * widget instead of drawing a square box over everything. */
enum {
  NAV_SHAPE_ROUNDRECT = 0,   /* item cells, +/- buttons, panels */
  NAV_SHAPE_CIRCLE,          /* the six round tab medallions, 能力点 + */
  NAV_SHAPE_PILL,            /* 使用 / 登录快捷键 / 粉碎 and similar bars */
  NAV_SHAPE_ELLIPSE          /* the title page's flat oval art buttons */
};
typedef struct {
  long x, y, w, h;
  long tx, ty;          /* preferred tap point; normally the centre. The
                         * class page moves it out of the stale save rows
                         * that sit under that modal layer. */
  unsigned char kind;
  unsigned char row;    /* stat row / grid row */
  unsigned char col;    /* grid column        */
  unsigned char shape;  /* one of NAV_SHAPE_* */
} NavItem;
/* present = 0 clears the focus. `back` is the page's own return arrow for B
 * (w = 0 when the page has none). page_serial is the live page layer's control
 * id: the engine assigns a fresh id whenever a tab tap rebuilds the page, so
 * the navigator can confirm a tab switch really happened. */
void input_set_nav(int present, const NavItem *tabs, int ntabs,
                   const NavItem *items, int nitems, int page_serial,
                   int page_kind, const NavItem *back);
int  input_nav_active(void);
/* current highlight rectangle in engine units; returns 0 when nothing to draw */
int  input_nav_highlight(long *x, long *y, long *w, long *h, int *shape);
/* Free-cursor position (screen pixels) when the right stick is in use. */
int  input_nav_cursor(int *x, int *y);
/* Nearest scroll viewport to an engine-space point. */
int  ui_scroll_viewport(long rx, long ry, long *x, long *y, long *w, long *h);

#endif
