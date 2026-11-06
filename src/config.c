/* config.c -- config.ini + the SD card path layout the whole port shares */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>

#include "config.h"
#include "util.h"

PortConfig cfg;
char g_root[256]     = "sdmc:/switch/inotia4";
char g_data_dir[256] = "sdmc:/switch/inotia4/data";
char g_save_dir[256] = "sdmc:/switch/inotia4/save";

void config_defaults(void) {
  cfg.screen_width  = 0;      /* auto */
  cfg.screen_height = 0;
  cfg.dock_1080p    = 1;
  cfg.language      = 0;      /* 0 = follow the console language */
  cfg.audio_freq    = 48000;
  cfg.audio_latency = 1024;
  cfg.music_volume  = 80;
  cfg.sfx_volume    = 90;
  cfg.text_scale    = 100;
  cfg.show_fps      = 0;
  cfg.skip_intro    = 0;
  cfg.log_level     = 1;
  cfg.load_hercules = 0;
  cfg.hub_callback  = 0;   /* answering it out of order deadlocks the Hub */
  cfg.skip_drm      = 1;
  cfg.tap_test      = 0;
  cfg.tap_test2     = 0;
  cfg.shot_after    = 0;
  cfg.shot_every    = 0;
  cfg.shot_every_frames = 0;
  cfg.auto_no_max = 8;   /* boot prompts, all answered until the class screen */
  cfg.skip_boot_prompts = 1; /* Hive/notice boxes never render their buttons */
  cfg.auto_no       = 0;   /* off: the player answers every dialog. Auto-
                            * answering left the prompt on screen: the callback
                            * ran but the box itself was never destroyed, so it
                            * stayed there and swallowed clicks. */
  cfg.force_sgl_size = 0;
  cfg.draw_log      = 0;
  cfg.gl_probe      = 0;
  cfg.draw_log_frame = 0;
  cfg.dump_tex      = 0;
  cfg.tex_replace   = 0;
  cfg.no_blend      = 0;
  cfg.draw_skip     = 0;
  cfg.flat_color    = 0;
  cfg.shot_frame_from = 0;
  cfg.shot_frame_to = 0;
  cfg.shot_frame_step = 0;
  cfg.skip_big_dark = 1;  /* the save-list paints a 16-screen-tall black panel */
  cfg.skip_thin_tall = 1; /* and a 16px-wide stripe down the middle of it */
  cfg.skip_huge     = 1;  /* and an oversized sprite over the character select */
  cfg.touch_cleanup = 0;  /* off: the engine's own release handling must run */
  cfg.kill_stale_overlay = 1;  /* the Hive prompt leaves a full-screen layer behind */
  cfg.draw_hitboxes = 0;       /* outline the control rects to calibrate clicks */
  cfg.draw_text     = 1;       /* the engine's own glyph rasteriser needs Android */
  cfg.text_trace    = 0;       /* off: only the screens being chased need it */
  cfg.textctrl_dy   = 0;       /* 0: centre the ink on the anchor the engine gives */
  cfg.bar_h        = 0;        /* 0: draw the engine's highlight bar unchanged */
  cfg.string_draw   = 1;       /* fill the engine's glyph atlas with FreeType
                                  text (prologue / tutorial / NPC name plate) */
  cfg.suppress_dup_push = 0;   /* off: the engine legitimately re-pushes the scene */
  cfg.text_px       = 20;      /* one size for every font: the engine's own
                                * per-index heights are not consistent */
  cfg.text_wrap_w   = 700;     /* engine units: the dialog's inner width, so a
                                * line only breaks when it really does not fit */
  cfg.text_px_scale = 85;      /* line height -> glyph size (26 -> 22) */
  cfg.text_fixed_px = 1;       /* ignore the per-index heights */
  cfg.calib         = 0;
  cfg.redraw_probe  = 0;   /* the check needs a server, i.e. it cannot pass offline */
  cfg.state_probe   = 0;   /* bring-up: log the scene/popup/input state */
  cfg.patch_http    = 0;   /* rewrite BL call sites in the engine; see main.c */
  cfg.stall_abort   = 0;   /* >0: panic-break after N stalled seconds (0 = log only) */

  cfg.inject        = 1;   /* the game is touch-only; see input.c */
  cfg.touch_x_pct   = 12;
  cfg.touch_y_pct   = 78;
  cfg.touch_x_pct2  = 50;
  cfg.touch_y_pct2  = 45;
  cfg.button_pct    = 18;
  cfg.overlay       = 0;   /* the real panel is the primary pointer */
  cfg.tap_script[0] = 0;   /* no scripted taps unless config.ini asks */
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  char *e = s + strlen(s);
  while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
  return s;
}

void config_load(const char *path) {
  config_defaults();

  FILE *f = fopen(path, "r");
  if (!f) {
    debugPrintf("config: %s not found, using defaults\n", path);
    return;
  }

  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *s = trim(line);
    if (!*s || *s == '#' || *s == ';' || *s == '[')
      continue;
    char *eq = strchr(s, '=');
    if (!eq)
      continue;
    *eq = '\0';
    char *key = trim(s);
    char *val = trim(eq + 1);

    /* allow trailing comments */
    char *hash = strchr(val, '#');
    if (hash && hash != val && hash[-1] == ' ')
      *hash = '\0', val = trim(val);

    if      (!strcasecmp_compat(key, "screen_width"))   cfg.screen_width  = atoi(val);
    else if (!strcasecmp_compat(key, "screen_height"))  cfg.screen_height = atoi(val);
    else if (!strcasecmp_compat(key, "dock_1080p"))     cfg.dock_1080p    = atoi(val);
    else if (!strcasecmp_compat(key, "language"))       cfg.language      = atoi(val);
    else if (!strcasecmp_compat(key, "audio_frequency"))cfg.audio_freq    = atoi(val);
    else if (!strcasecmp_compat(key, "audio_latency"))  cfg.audio_latency = atoi(val);
    else if (!strcasecmp_compat(key, "music_volume"))   cfg.music_volume  = atoi(val);
    else if (!strcasecmp_compat(key, "sfx_volume"))     cfg.sfx_volume    = atoi(val);
    else if (!strcasecmp_compat(key, "text_scale"))     cfg.text_scale    = atoi(val);
    else if (!strcasecmp_compat(key, "show_fps"))       cfg.show_fps      = atoi(val);
    else if (!strcasecmp_compat(key, "skip_intro"))     cfg.skip_intro    = atoi(val);
    else if (!strcasecmp_compat(key, "log_level"))      cfg.log_level     = atoi(val);
    else if (!strcasecmp_compat(key, "load_hercules"))  cfg.load_hercules = atoi(val);
    else if (!strcasecmp_compat(key, "hub_callback"))   cfg.hub_callback  = atoi(val);
    else if (!strcasecmp_compat(key, "skip_drm"))       cfg.skip_drm      = atoi(val);
    else if (!strcasecmp_compat(key, "tap_test"))       cfg.tap_test      = atoi(val);
    else if (!strcasecmp_compat(key, "tap_test2"))      cfg.tap_test2     = atoi(val);
    else if (!strcasecmp_compat(key, "shot_after"))     cfg.shot_after    = atoi(val);
    else if (!strcasecmp_compat(key, "shot_every"))     cfg.shot_every    = atoi(val);
    else if (!strcasecmp_compat(key, "shot_every_frames")) cfg.shot_every_frames = atoi(val);
    else if (!strcasecmp_compat(key, "auto_no_max"))   cfg.auto_no_max   = atoi(val);
    else if (!strcasecmp_compat(key, "skip_boot_prompts")) cfg.skip_boot_prompts = atoi(val);
    else if (!strcasecmp_compat(key, "auto_no"))        cfg.auto_no       = atoi(val);
    else if (!strcasecmp_compat(key, "force_sgl_size")) cfg.force_sgl_size = atoi(val);
    else if (!strcasecmp_compat(key, "draw_log"))       cfg.draw_log      = atoi(val);
    else if (!strcasecmp_compat(key, "gl_probe"))       cfg.gl_probe      = atoi(val);
    else if (!strcasecmp_compat(key, "draw_log_frame")) cfg.draw_log_frame = atoi(val);
    else if (!strcasecmp_compat(key, "dump_tex"))       cfg.dump_tex      = atoi(val);
    else if (!strcasecmp_compat(key, "tex_replace"))    cfg.tex_replace   = atoi(val);
    else if (!strcasecmp_compat(key, "no_blend"))       cfg.no_blend      = atoi(val);
    else if (!strcasecmp_compat(key, "draw_skip"))      cfg.draw_skip     = atoi(val);
    else if (!strcasecmp_compat(key, "flat_color"))     cfg.flat_color    = atoi(val);
    else if (!strcasecmp_compat(key, "shot_frame_from")) cfg.shot_frame_from = atoi(val);
    else if (!strcasecmp_compat(key, "shot_frame_to"))   cfg.shot_frame_to   = atoi(val);
    else if (!strcasecmp_compat(key, "shot_frame_step")) cfg.shot_frame_step = atoi(val);
    else if (!strcasecmp_compat(key, "skip_big_dark"))  cfg.skip_big_dark  = atoi(val);
    else if (!strcasecmp_compat(key, "skip_thin_tall")) cfg.skip_thin_tall = atoi(val);
    else if (!strcasecmp_compat(key, "skip_huge"))      cfg.skip_huge      = atoi(val);
    else if (!strcasecmp_compat(key, "touch_cleanup"))  cfg.touch_cleanup  = atoi(val);
    else if (!strcasecmp_compat(key, "kill_stale_overlay")) cfg.kill_stale_overlay = atoi(val);
    else if (!strcasecmp_compat(key, "draw_hitboxes"))  cfg.draw_hitboxes  = atoi(val);
    else if (!strcasecmp_compat(key, "draw_text"))      cfg.draw_text      = atoi(val);
    else if (!strcasecmp_compat(key, "text_trace"))     cfg.text_trace     = atoi(val);
    else if (!strcasecmp_compat(key, "string_draw"))    cfg.string_draw    = atoi(val);
    else if (!strcasecmp_compat(key, "textctrl_dy"))    cfg.textctrl_dy    = atoi(val);
    else if (!strcasecmp_compat(key, "suppress_dup_push")) cfg.suppress_dup_push = atoi(val);
    else if (!strcasecmp_compat(key, "text_px"))        cfg.text_px        = atoi(val);
    else if (!strcasecmp_compat(key, "text_wrap_w"))    cfg.text_wrap_w    = atoi(val);
    else if (!strcasecmp_compat(key, "text_px_scale"))  cfg.text_px_scale  = atoi(val);
    else if (!strcasecmp_compat(key, "bar_h"))          cfg.bar_h          = atoi(val);
    else if (!strcasecmp_compat(key, "text_fixed_px"))  cfg.text_fixed_px  = atoi(val);
    else if (!strcasecmp_compat(key, "calib"))          cfg.calib         = atoi(val);
    else if (!strcasecmp_compat(key, "redraw_probe"))   cfg.redraw_probe  = atoi(val);
    else if (!strcasecmp_compat(key, "state_probe"))    cfg.state_probe   = atoi(val);
    else if (!strcasecmp_compat(key, "patch_http"))     cfg.patch_http    = atoi(val);
    else if (!strcasecmp_compat(key, "stall_abort"))    cfg.stall_abort   = atoi(val);
    else if (!strcasecmp_compat(key, "inject"))         cfg.inject        = atoi(val);
    else if (!strcasecmp_compat(key, "touch_x_pct"))    cfg.touch_x_pct   = atoi(val);
    else if (!strcasecmp_compat(key, "touch_y_pct"))    cfg.touch_y_pct   = atoi(val);
    else if (!strcasecmp_compat(key, "touch_x_pct2"))   cfg.touch_x_pct2  = atoi(val);
    else if (!strcasecmp_compat(key, "touch_y_pct2"))   cfg.touch_y_pct2  = atoi(val);
    else if (!strcasecmp_compat(key, "button_pct"))     cfg.button_pct    = atoi(val);
    else if (!strcasecmp_compat(key, "overlay"))        cfg.overlay       = atoi(val);
    else if (!strcasecmp_compat(key, "tap_script")) {
      /* the readme shows this one quoted ("960,919@600; 959,279@1200") because
       * the semicolon is also the ini comment character in some parsers; this
       * parser keeps the value verbatim, so drop the quotes or sscanf() sees a
       * leading '"' and silently loses the first tap */
      char *v = val;
      const size_t n = strlen(v);
      if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
        v[n - 1] = '\0';
        v++;
      }
      snprintf(cfg.tap_script, sizeof(cfg.tap_script), "%s", v);
    }
    else if (!strcasecmp_compat(key, "data_dir"))       snprintf(g_data_dir, sizeof(g_data_dir), "%s", val);
    else if (!strcasecmp_compat(key, "save_dir"))       snprintf(g_save_dir, sizeof(g_save_dir), "%s", val);
  }
  fclose(f);

  /* 0 means "decide from the console mode" (see main.c), anything else has to
   * be a sane render target */
  if (cfg.screen_width != 0 && (cfg.screen_width < 640 || cfg.screen_width > 1920))
    cfg.screen_width = 0;
  if (cfg.screen_height != 0 && (cfg.screen_height < 360 || cfg.screen_height > 1080))
    cfg.screen_height = 0;
  if ((cfg.screen_width == 0) != (cfg.screen_height == 0)) {
    cfg.screen_width = 0;
    cfg.screen_height = 0;
  }
  if (cfg.touch_x_pct < 0 || cfg.touch_x_pct > 90) cfg.touch_x_pct = 12;
  if (cfg.touch_y_pct < 10 || cfg.touch_y_pct > 98) cfg.touch_y_pct = 78;
  if (cfg.button_pct < 5 || cfg.button_pct > 45) cfg.button_pct = 18;
  if (cfg.audio_freq < 8000 || cfg.audio_freq > 48000) cfg.audio_freq = 48000;
  if (cfg.audio_latency < 256) cfg.audio_latency = 256;
  if (cfg.audio_latency > 4096) cfg.audio_latency = 4096;
  if (cfg.text_scale < 50) cfg.text_scale = 50;
  if (cfg.text_scale > 200) cfg.text_scale = 200;

  /* bring-up: file access is what a stall or a crash in the asset path looks
   * like from the outside, so keep at least the fs trace on */
  if (cfg.log_level < 2)
    cfg.log_level = 2;

  debugPrintf("config: %s%dx%d dock1080=%d lang=%d audio=%dHz/%df inject=%d log=%d\n",
              cfg.screen_width ? "" : "auto ", cfg.screen_width, cfg.screen_height,
              cfg.dock_1080p, cfg.language, cfg.audio_freq, cfg.audio_latency,
              cfg.inject, cfg.log_level);
}
