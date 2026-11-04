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

/* what the engine believes its screen size is (bring-up diagnostics) */
int input_engine_screen_w(void);
unsigned char *input_engine_virtbase(void);
int input_engine_screen_h(void);
/* the engine's live touch count (0 == no finger down) */
int input_engine_touch_count(void);

#endif
