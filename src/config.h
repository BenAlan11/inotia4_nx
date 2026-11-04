#ifndef CONFIG_H
#define CONFIG_H

/* everything the port reads from config.ini lives here */
typedef struct {
  int  screen_width;      /* render target; 0 = follow the console mode */
  int  screen_height;
  int  dock_1080p;        /* 1 = use 1920x1080 when docked */
  int  language;          /* index passed to CCommonAPI.nativeInitialize */
  int  audio_freq;        /* SDL/audout sample rate, 48000 default */
  int  audio_latency;     /* frames per buffer, 1024 default */
  int  music_volume;      /* 0..100 */
  int  sfx_volume;        /* 0..100 */
  int  text_scale;        /* percent, 100 = Android default metrics */
  int  show_fps;
  int  skip_intro;        /* skip res/H7.mp4 */
  int  log_level;
  int  load_hercules;
  int  hub_callback;      /* pre-answer the Hive login; see main.c */
  int  skip_drm;
  int  tap_test;
  int  tap_test2;         /* a second scripted tap, for reaching deeper menus */
  int  shot_after;
  int  shot_every;
  int  shot_every_frames; /* present this often a screenshot, indexed by render count */
  int  auto_no_max;      /* answer at most this many boot prompts */
  int  skip_boot_prompts;/* never create the Hive / notice prompts at boot */
  int  auto_no;           /* answer every YES/NO prompt with NO */
  int  force_sgl_size;    /* overwrite the engine's own screen size (bring-up) */
  int  draw_log;          /* log this many SGL draw calls (bring-up) */
  int  gl_probe;          /* log GL viewport/scissor state around each present */
  int  draw_log_frame;    /* only start the draw trace at this frame */
  int  dump_tex;          /* dump this many uploaded textures to tex_N.ppm */
  int  tex_replace;       /* force GL_REPLACE texenv (bring-up) */
  int  no_blend;          /* force blending off (bring-up) */
  int  draw_skip;         /* drop the first N quads of every frame (bring-up) */
  int  flat_color;        /* draw every quad with no texture, solid green */
  int  shot_frame_from;   /* framebuffer grab window, by render index */
  int  shot_frame_to;
  int  shot_frame_step;
  int  skip_big_dark;     /* drop oversized opaque-black quads (bring-up) */
  int  skip_thin_tall;    /* drop the 14px-wide full-height stripe (bring-up) */
  int  skip_huge;         /* drop texture parts with an absurd destination size */
  int  touch_cleanup;     /* force-clear engine touch state (bring-up; breaks clicks) */
  int  kill_stale_overlay;/* drop the leftover popup layer above the menu */
  int  draw_hitboxes;     /* outline every control rectangle (bring-up) */
  int  draw_text;         /* draw engine text with FreeType (see main.c) */
  int  text_trace;        /* log the text entry points we do not replace */
  int  string_draw;       /* draw SGL string images (the prologue/tutorial text) */
  int  textctrl_dy;       /* px to nudge text controls; 0 = centre on the anchor */
  int  suppress_dup_push; /* drop repeated UI_SetPopupProcessInfo(1,1) (bring-up only) */
  int  text_px;           /* 0 = take the size from the font group */
  int  text_wrap_w;       /* break engine text at this width (0 = never) */
  int  text_px_scale;     /* percent of the engine's line height used as glyph size */
  int  text_fixed_px;     /* 1 = use text_px for every font index */
  int  calib;             /* draw scissor calibration marks */
  int  redraw_probe;      /* paint magenta after each present to see if the engine redraws */
  int  state_probe;       /* log scene/popup/input-tree state + wrap the menu transition (bring-up) */
  int  patch_http;        /* rewrite BL call sites inside the engine (see main.c) */
  int  stall_abort;       /* seconds without a frame before aborting for a stack (0 = off) */

  /* touch emulation */
  int  inject;            /* mirror pad input into the engine's queue */
  int  touch_x_pct;       /* virtual stick origin, percent of the view */
  int  touch_y_pct;
  int  touch_x_pct2;      /* where the second scripted tap lands */
  int  touch_y_pct2;
  int  button_pct;        /* distance of the action spots from the corner */
  int  overlay;           /* draw the focus box / crosshair (0 = off) */
  /* scripted taps for unattended testing:
   *   tap_script = "1490,562@1200;1490,300@1400"
   * screen pixels, fired once the engine frame counter passes the value */
  char tap_script[256];
} PortConfig;

extern PortConfig cfg;

/* root of the game data on the SD card, e.g. "sdmc:/switch/inotia4" */
extern char g_root[256];
/* where the engine's /sdcard/Android/data/<pkg>/files tree is redirected to */
extern char g_data_dir[256];
/* where saves go */
extern char g_save_dir[256];

void config_defaults(void);
void config_load(const char *path);

#endif
