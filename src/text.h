#ifndef TEXT_H
#define TEXT_H

/* Text on screen comes from two very different paths:
 *
 *   1. the game's own UI text, drawn by the engine from assets/common/game_res/
 *      font.fnt (a proprietary bitmap atlas it parses itself) -- nothing to do
 *      for us beyond keeping the file at its original path;
 *   2. Android system text the shell asks for through CFont / drawText, which
 *      we rasterise with FreeType here and blit as a GLES1 textured quad.
 *
 * Only path 2 needs this module. */

void text_init(const char *font_path);
void text_deinit(void);
int  text_available(void);

/* width in pixels of a UTF-8 string at the given pixel height (0 = default) */
int  text_measure(const char *utf8);
int  text_ink_height(const char *utf8, int px);
int  text_measure_at(const char *utf8, int px);

void text_draw(const char *utf8, int x, int y, unsigned color, int px);
/* white glyphs, coverage in alpha, into a caller-owned RGBA buffer (top-down).
 * Returns the used width; 0 when there is no font or nothing to draw. */
int  text_render_rgba(const char *utf8, int px, unsigned char *buf, int w, int h);
/* same as above but if color_codes != 0, parse $X colour codes and render each
 * run with its specified colour (e.g., blue emphasis for "每日奖励"). */
int  text_render_rgba_ex(const char *utf8, int px, unsigned char *buf, int w, int h, int color_codes);
void text_draw_rect(const char *utf8, int x, int y, int w, int h,
                    unsigned color, int px);

/* Draw text in the CALLER's current GL coordinate space, using the caller's
 * current colour. The engine lays its UI out in its own units (1137x640) and
 * has already set up the matching projection, so the quad is emitted with the
 * matrices untouched -- that is what makes this a drop-in replacement for the
 * engine's own SGL_grpDrawStringWithFont. `len` < 0 means strlen. */
void text_draw_engine(const char *utf8, int len, int x, int y, int px,
                      unsigned color);
/* Same as above but if color_codes != 0, preserves $X colour codes for multi-colour rendering. */
void text_draw_engine_ex(const char *utf8, int len, int x, int y, int px,
                         unsigned color, int color_codes);
int  text_width_engine(const char *utf8, int len, int px);
/* width, in engine layout units, a single line may occupy (0 = no wrapping) */
void text_set_wrap_width(int w);

/* bring-up tracing: which engine entry point asked for the current draw */
void text_set_caller(const char *tag);
/* 1 when the GL texture id was created by this module's render() */
int  text_is_our_texture(unsigned t);
void text_set_default_size(int px);
/* the blit needs the current view size to build its ortho projection */
void text_set_viewport(int w, int h);
/* the engine's layout size (1137x640 here): text coordinates are in these
 * units, not framebuffer pixels */
void text_set_engine_size(int w, int h);
/* main.c: re-specify the client arrays the engine last set, after this module
 * drew from its own (see nx_restore_client_arrays) */
void nx_restore_client_arrays(void);
/* 0 = do not push the engine's following lines down by the extra wrapped lines
 * (a page that positions its own blocks); 1 = the default */
void text_set_wrap_extra(int on);
/* filled rectangle in engine units (for the description page's scroll thumb) */
void text_fill_rect(int x, int y, int w, int h, unsigned color);
/* clip every glyph quad to this box (engine units); x1 <= x0 turns it off */
void text_set_clip(int x0, int y0, int x1, int y1);
/* wrap `utf8` at `box_w` px, inserting '\n'; returns the line count */
int  text_wrap_to_box(const char *utf8, int box_w, int px, char *dst, int cap);

#endif
