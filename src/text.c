/* text.c -- system-font text for the Java shell (CFont / drawText)
 *
 * The game's own UI strings never come through here: it ships font.fnt and
 * rasterises those glyphs itself. This module only covers the handful of
 * Android-side text calls (text fields, dialogs), which we render with
 * FreeType into a small texture cache and blit with GLES1.
 */

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <GLES/gl.h>

#include "text.h"
#include "config.h"
#include "util.h"

#define CACHE_SLOTS 96
#define ATLAS_MAX   2048

typedef struct {
  unsigned hash;
  GLuint tex;
  int w, h;
  char *key;
  unsigned last_used;
} TextCache;

static FT_Library ft;
static FT_Face face;
static int have_face;
static const char *g_caller = "?";
void text_set_caller(const char *tag) { g_caller = tag ? tag : "?"; }
static int default_px = 24;
static PlFontData shared_font;   /* must outlive the FT_Face */
static int pl_ready;

static TextCache cache[CACHE_SLOTS];
static unsigned tick;

void text_set_default_size(int px) {
  if (px > 4 && px < 512)
    default_px = px;
}

int text_available(void) { return have_face; }

void text_init(const char *font_path) {
  if (FT_Init_FreeType(&ft) != 0) {
    debugPrintf("text: FreeType init failed\n");
    return;
  }

  /* A TTF on the card wins over the console's shared font: the shared font is
   * Nintendo's own cut, which does not look like the Android system font the
   * game was designed around. Drop Noto Sans SC / Source Han Sans at
   * sdmc:/switch/inotia4/font.ttf and it is used instead. */
  {
    const char *candidates[] = {
      font_path,
      "sdmc:/switch/inotia4/font.ttf",
      "sdmc:/switch/inotia4/system_font.ttf",
      "sdmc:/switch/inotia4/font.otf",
      "sdmc:/switch/inotia4/font.ttc",
    };
    for (unsigned i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
      if (!candidates[i])
        continue;
      if (FT_New_Face(ft, candidates[i], 0, &face) == 0) {
        have_face = 1;
        debugPrintf("text: loaded font %s (%ld glyphs)\n", candidates[i],
                    (long)face->num_glyphs);
        return;
      }
    }
    debugPrintf("text: no TTF on the card, using the console's shared font\n");
  }

  if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
    pl_ready = 1;
    /* The game's UI text is UTF-8 Simplified Chinese (the strings reach us as
     * e9 bb 91 ... = "黑曜骑士"), and the console's *default* shared font is
     * the Japanese one -- it has no 简体 glyphs, so every Chinese character came
     * out as a wrong or missing glyph, i.e. 乱码. Ask for the Simplified
     * Chinese face first and keep the standard one only as a fallback. */
    static const struct { PlSharedFontType type; const char *name; } tries[] = {
      { PlSharedFontType_ChineseSimplified, "ChineseSimplified" },
      { PlSharedFontType_Standard,          "Standard" },
    };
    for (unsigned i = 0; i < sizeof(tries) / sizeof(*tries); i++) {
      if (R_FAILED(plGetSharedFontByType(&shared_font, tries[i].type)))
        continue;
      if (!shared_font.address || !shared_font.size)
        continue;
      if (FT_New_Memory_Face(ft, (const FT_Byte *)shared_font.address,
                             (FT_Long)shared_font.size, 0, &face) == 0) {
        have_face = 1;
        debugPrintf("text: using the console's %s shared font (%u KB)\n",
                    tries[i].name, (unsigned)(shared_font.size / 1024));
        return;
      }
    }
    debugPrintf("text: shared font unavailable, looking for a TTF\n");
  }

  debugPrintf("text: no TTF found; system text will be measured, not drawn\n");
}

void text_deinit(void) {
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (cache[i].tex) {
      GLuint t = cache[i].tex;
      glDeleteTextures(1, &t);
    }
    free(cache[i].key);
  }
  if (have_face)
    FT_Done_Face(face);
  if (ft)
    FT_Done_FreeType(ft);
  if (pl_ready)
    plExit();
}

/* ------------------------------------------------------------------------- */

static unsigned fnv1a(const char *s, unsigned h) {
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 16777619u;
  }
  return h;
}

/* ------------------------------------------------------------------------- */
/* inline colour codes                                                       */
/* ------------------------------------------------------------------------- */

/* The engine's text database embeds colour changes as '$' + a one letter id:
 * the first tutorial line is "请触摸屏幕左侧闪烁的$R方向键$B来控制角色移动。",
 * i.e. draw 方向键 in the R colour and switch back to B afterwards. On Android
 * the engine's own layout code consumes those two bytes; here they would end
 * up in the glyph run, so the player reads a literal "$R" in the middle of the
 * sentence. Strip them, and remember each id once so the vocabulary shows up in
 * the log (it is the only way to learn the rest of the table). */
static void note_code(unsigned char id) {
  static unsigned char seen[256];
  if (!seen[id]) {
    seen[id] = 1;
    debugPrintf("text: colour code $%c\n", (char)id);
  }
}

/* copy src[0..len) into dst with every '$<letter>' pair removed */
static int strip_codes(const char *src, int len, char *dst, int cap) {
  int o = 0;
  for (int i = 0; i < len && o < cap - 1; i++) {
    const unsigned char c = (unsigned char)src[i];
    if (c == '$' && i + 1 < len) {
      const unsigned char n = (unsigned char)src[i + 1];
      if ((n >= 'A' && n <= 'Z') || (n >= 'a' && n <= 'z')) {
        note_code(n);
        i++;
        continue;
      }
    }
    dst[o++] = (char)c;
  }
  dst[o] = 0;
  return o;
}

/* strip into `buf` and return the string to use (the original when there is
 * nothing to strip, so the common case costs no copy) */
static const char *clean_str(const char *s, char *buf, int cap) {
  if (!s)
    return NULL;
  if (!strchr(s, '$'))
    return s;
  strip_codes(s, (int)strlen(s), buf, cap);
  return buf;
}

/* Font sizes are read out of the engine's font-group objects. A group the port
 * has not seen before can put anything at that offset -- a pointer, a length, a
 * flag -- and FreeType handed a size like 10^8 spends a very long time inside
 * FT_Set_Pixel_Sizes / the glyph rasteriser before it gives up. That is the
 * "画面卡死" in the in-game HUD: the stack sits at
 * UIPorting_DrawNumber -> GRPX_DrawStringWithFont (our drawer) and no frame
 * comes out for minutes. Clamp to a range no sane UI size leaves. */
static int sane_px(int px) {
  if (px <= 0)
    return default_px;
  if (px < 6)
    return 6;
  if (px > 96) {
    static int logs;
    if (logs < 6) {
      logs++;
      debugPrintf("text: font size %d clamped to 96\n", px);
    }
    return 96;
  }
  return px;
}

int text_measure_at(const char *utf8, int px) {
  if (!utf8 || !*utf8)
    return 0;
  char clean[1024];
  utf8 = clean_str(utf8, clean, (int)sizeof(clean));
  if (!utf8 || !*utf8)
    return 0;
  if (!have_face) {
    /* no font: assume a monospace-ish advance so layouts stay sane */
    const int size = sane_px(px);
    return (int)(strlen(utf8) * size * 0.55f);
  }
  const int size = sane_px(px);
  if (FT_Set_Pixel_Sizes(face, 0, size) != 0)
    return 0;

  int width = 0;
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    unsigned cp = *p++;
    if (cp >= 0xF0) { cp = ((cp & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (cp >= 0xE0) { cp = ((cp & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (cp >= 0xC0) { cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    if (FT_Load_Char(face, cp, FT_LOAD_DEFAULT) == 0)
      width += (int)(face->glyph->advance.x >> 6);
  }
  return width;
}

int text_measure(const char *utf8) { return text_measure_at(utf8, 0); }

/* Height of the ink of this string: the tallest glyph above the baseline plus
 * the deepest one below it. The engine anchors a string image at its centre
 * (its highlight bar around the tutorial hint is a 20 px tall quad centred on
 * the very y it hands over), so a drawer that puts the ink's top at y hangs it
 * half a line too low. */
int text_ink_height(const char *utf8, int px) {
  if (!have_face || !utf8 || !*utf8)
    return 0;
  const int size = sane_px(px);
  if (FT_Set_Pixel_Sizes(face, 0, size) != 0)
    return 0;
  int top = 0, bottom = 0;
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    unsigned cp = *p++;
    if (cp >= 0xF0) { cp = ((cp & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (cp >= 0xE0) { cp = ((cp & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (cp >= 0xC0) { cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
      const int t = (int)face->glyph->bitmap_top;
      const int b = (int)face->glyph->bitmap.rows - t;
      if (t > top) top = t;
      if (b > bottom) bottom = b;
    }
  }
  return top + bottom;
}

/* How many pixels this string rises above its baseline -- the same number
 * render() uses as the texture's top padding. Runs of one line must share a
 * baseline: "...可以获得$S每日奖励$B。" is drawn as three separate quads, each
 * with its own texture height, so the run that holds only "。" was anchored at
 * the line's top instead of on its baseline and the full stop appeared as a
 * raised circle. Same for a run of latin digits next to CJK. */
static int text_ascent_at(const char *utf8, int px) {
  if (!have_face || !utf8 || !*utf8)
    return 0;
  const int size = sane_px(px);
  if (FT_Set_Pixel_Sizes(face, 0, size) != 0)
    return 0;
  int ascent = 0;
  const unsigned char *p = (const unsigned char *)utf8;
  while (*p) {
    unsigned cp = *p++;
    if (cp >= 0xF0) { cp = ((cp & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (cp >= 0xE0) { cp = ((cp & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (cp >= 0xC0) { cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
      const int top = (int)face->glyph->bitmap_top;
      if (top > ascent)
        ascent = top;
    }
  }
  return ascent;
}

/* is this GL texture one of the ones render() made? used by the draw probe to
 * tell "a quad of ours" from "a quad the engine drew". */
int text_is_our_texture(unsigned t) {
  if (!t)
    return 0;
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].tex == t)
      return 1;
  return 0;
}

/* ------------------------------------------------------------------------- */
/* string -> RGBA texture, cached by (text, size, colour)                    */
/* ------------------------------------------------------------------------- */

static TextCache *cache_find(const char *key, unsigned hash) {
  for (int i = 0; i < CACHE_SLOTS; i++)
    if (cache[i].tex && cache[i].hash == hash && cache[i].key &&
        !strcmp(cache[i].key, key))
      return &cache[i];
  return NULL;
}

static TextCache *cache_slot(void) {
  TextCache *oldest = &cache[0];
  for (int i = 0; i < CACHE_SLOTS; i++) {
    if (!cache[i].tex)
      return &cache[i];
    if (cache[i].last_used < oldest->last_used)
      oldest = &cache[i];
  }
  if (oldest->tex) {
    GLuint t = oldest->tex;
    glDeleteTextures(1, &t);
    oldest->tex = 0;
  }
  free(oldest->key);
  oldest->key = NULL;
  return oldest;
}

static void blit(TextCache *c, int x, int y, int sw, int sh) {
  if (!c || !c->tex)
    return;

  const float w = (float)c->w;
  const float h = (float)c->h;

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, c->tex);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  /* Scene-level drawers (TEXTCTRL2: the story text and the tutorial body) run
   * with whatever state the map renderer left behind -- depth test on, depth
   * from the last 3D objects. A text quad at z = 0 then fails the depth test
   * and disappears entirely, while the same call from a popup (depth off) shows
   * up fine. Text is always 2D: turn the test off for the draw and put it back. */
  const GLboolean depth_was_on = glIsEnabled(GL_DEPTH_TEST);
  if (depth_was_on)
    glDisable(GL_DEPTH_TEST);

  /* GLES1 fixed pipeline: an ortho projection in window pixels keeps the call
   * sites independent of the engine's current matrix */
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glLoadIdentity();
  glOrthof(0.0f, (float)sw, (float)sh, 0.0f, -1.0f, 1.0f);
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();
  glLoadIdentity();

  const GLfloat verts[] = { (float)x, (float)y,
                            (float)x + w, (float)y,
                            (float)x, (float)y + h,
                            (float)x + w, (float)y + h };
  const GLfloat uvs[] = { 0, 0, 1, 0, 0, 1, 1, 1 };

  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, verts);
  glTexCoordPointer(2, GL_FLOAT, 0, uvs);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  glDisableClientState(GL_VERTEX_ARRAY);

  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();

  /* restore the state the engine expects to find */
  glDisable(GL_TEXTURE_2D);
  glDisable(GL_BLEND);
  if (depth_was_on)
    glEnable(GL_DEPTH_TEST);
}

static TextCache *render(const char *utf8, int px, unsigned color) {
  if (!have_face || !utf8 || !*utf8)
    return NULL;

  char key[512];
  snprintf(key, sizeof(key), "%d:%08x:%s", px, color, utf8);
  const unsigned h = fnv1a(key, 2166136261u);
  TextCache *c = cache_find(key, h);
  if (c) {
    c->last_used = ++tick;
    return c;
  }

  if (FT_Set_Pixel_Sizes(face, 0, px) != 0)
    return NULL;

  /* measure first so the bitmap is exactly the size of the string */
  int width = 0, ascent = 0, descent = 0;
  const unsigned char *p = (const unsigned char *)utf8;
  unsigned cps[1024];
  int nc = 0;
  while (*p && nc < 1024) {
    unsigned cp = *p++;
    if (cp >= 0xF0) { cp = ((cp & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (cp >= 0xE0) { cp = ((cp & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (cp >= 0xC0) { cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
    cps[nc++] = cp;
    /* Render while measuring: bitmap_top and bitmap.rows only carry the real
     * values after a render. FT_LOAD_DEFAULT leaves both at zero, which
     * collapses ascent/descent, makes `height` 2 px, and clips every glyph out
     * of the atlas -- "the string was submitted" with nothing on screen. */
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) == 0) {
      width += (int)(face->glyph->advance.x >> 6);
      const int top = (int)(face->glyph->bitmap_top);
      const int bottom = (int)(face->glyph->bitmap.rows) - top;
      if (top > ascent) ascent = top;
      if (bottom > descent) descent = bottom;
    }
  }
  if (nc == 0 || width <= 0)
    return NULL;

  /* Line the glyphs up on the font's baseline, not on each string's own ink.
   * The box used to be exactly the ink of this string, and the quad is drawn
   * with its top at the y the engine gives -- so a digits-only string ("0" next
   * to 能力点:) has no descender and a smaller ascent, and it came out sitting
   * higher than the CJK label next to it at the very same y. Padding the box to
   * the face's ascender/descender puts every string of one size on one
   * baseline. */
  {
    const int face_asc = (int)(face->size->metrics.ascender >> 6);
    const int face_desc = (int)(-(face->size->metrics.descender >> 6));
    if (face_asc > ascent)
      ascent = face_asc;
    if (face_desc > descent)
      descent = face_desc;
  }

  const int height = ascent + descent + 2;
  if (width > ATLAS_MAX || height > ATLAS_MAX)
    return NULL;

  uint32_t *bitmap = calloc((size_t)width * height, sizeof(uint32_t));
  if (!bitmap)
    return NULL;

  const uint8_t cr = (color >> 24) & 0xff;
  const uint8_t cg = (color >> 16) & 0xff;
  const uint8_t cb = (color >> 8) & 0xff;
  const uint8_t ca = color ? 0xff : 0xff;

  int pen = 0;
  for (int i = 0; i < nc; i++) {
    if (FT_Load_Char(face, cps[i],
                     FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
      continue;
    const FT_GlyphSlot g = face->glyph;
    const int gx = pen + g->bitmap_left;
    const int gy = ascent - g->bitmap_top + 1;
    for (unsigned row = 0; row < g->bitmap.rows; row++) {
      const int y = gy + (int)row;
      if (y < 0 || y >= height)
        continue;
      for (unsigned col = 0; col < g->bitmap.width; col++) {
        const int x = gx + (int)col;
        if (x < 0 || x >= width)
          continue;
        const uint8_t a = g->bitmap.buffer[row * g->bitmap.pitch + col];
        if (!a)
          continue;
        /* The buffer is a uint32_t uploaded as GL_RGBA/GL_UNSIGNED_BYTE, so on a
         * little-endian CPU the bytes that reach the texture are the reverse of
         * the word: packing (a<<24)|(r<<16)|(g<<8)|b lands as B,G,R,A and every
         * colour whose red and blue differ came out swapped -- the gold names
         * turned blue, the blue emphasis turned red. Pack it the way the texture
         * actually reads it: A,B,G,R. */
        bitmap[(size_t)y * width + x] = ((uint32_t)a << 24) |
                                         ((uint32_t)cb << 16) |
                                         ((uint32_t)cg << 8) | (uint32_t)cr;
        (void)ca;
      }
    }
    pen += (int)(g->advance.x >> 6);
  }

  c = cache_slot();
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, bitmap);
  free(bitmap);

  c->tex = tex;
  c->w = width;
  c->h = height;
  c->hash = h;
  c->key = strdup(key);
  c->last_used = ++tick;
  return c;
}

/* ------------------------------------------------------------------------- */

static int viewport_w = 1280, viewport_h = 720;

/* $<letter> colour codes. The engine's text database uses them to switch the
 * pen colour mid-sentence (see note_code above). We only ever *see* them on the
 * nx_grp_draw_string path; the string-image path hands us an already stripped
 * string, which is why the emphasis inside the hint line was lost. The table
 * maps the letters we have actually observed; an unknown letter switches back
 * to the default colour rather than leaking whatever was current. */
static unsigned colour_for_code(unsigned char id) {
  switch (id) {
  /* B is the BASE colour, not blue: every emphasised span in the database is
   * closed by "$B" ("...可以获得$S每日奖励$B。"), so it is what the sentence
   * returns to. S is the emphasis the original draws in blue -- "每日奖励",
   * the numbers in a skill description, the HP range on a potion. R is the
   * red warning used for a missing skill point.
   *
   * These are plain 0xRRGGBBAA now. They used to be written for the swapped
   * byte order render() produced (see the texel packing there), which is why
   * "S" held 0xFF4080FF: blue on screen, pink in the value. */
  case 'B': return 0xFFFFFFFFu;   /* base / default */
  case 'A': return 0xFF4040FFu;   /* accent: the popup title, same red as R */
  case 'S': return 0x4080FFFFu;   /* emphasis, blue */
  case 'R': return 0xFF4040FFu;   /* red            */
  case 'G': return 0x40FF40FFu;   /* green          */
  case 'Y': return 0xFFFF40FFu;   /* yellow         */
  case 'C': return 0x40FFFFFFu;   /* cyan           */
  case 'M': return 0xFF40FFFFu;   /* magenta        */
  case 'W': return 0xFFFFFFFFu;   /* white          */
  case 'K': return 0x101010FFu;   /* black          */
  default:  return 0xFFFFFFFFu;   /* unknown: back to the default pen */
  }
}

/* Render UTF-8 text straight into a caller-owned RGBA buffer (w x h, rows
 * top-down). Returns the used width in pixels.
 *
 * With color_codes == 0 every glyph is white and the coverage lives in alpha --
 * the shape the string-image atlas needs, since the engine multiplies that
 * quad by its own colour. With color_codes != 0 the '$<letter>' pairs are
 * consumed here instead of being stripped, and each glyph is painted with the
 * colour that was current where it sits. */
int text_render_rgba_ex(const char *utf8, int px, unsigned char *buf, int w,
                        int h, int color_codes) {
  if (!buf || w <= 0 || h <= 0)
    return 0;
  memset(buf, 0, (size_t)w * (size_t)h * 4);
  if (!have_face || !utf8 || !*utf8 || px <= 0)
    return 0;
  if (FT_Set_Pixel_Sizes(face, 0, px) != 0)
    return 0;

  /* One pass over the source builds the glyph list: codepoint, pen position and
   * the colour in force at that point. Colour codes never become glyphs. */
  unsigned cps[1024];
  int offsets[1024];
  unsigned colours[1024];
  int nglyphs = 0;
  int pen_x = 0;
  int ascent = 0, descent = 0;
  unsigned cur = 0xFFFFFFFFu;

  const unsigned char *p = (const unsigned char *)utf8;
  while (*p && nglyphs < 1024) {
    unsigned cp = *p++;
    if (cp >= 0xF0) { cp = ((cp & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
    else if (cp >= 0xE0) { cp = ((cp & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
    else if (cp >= 0xC0) { cp = ((cp & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }

    if (color_codes && cp == '$' && *p) {
      const unsigned char id = *p;
      if ((id >= 'A' && id <= 'Z') || (id >= 'a' && id <= 'z')) {
        p++;
        note_code(id);
        cur = colour_for_code(id);
        continue;
      }
    }
    /* Render on this pass too, not just measure: bitmap_top and bitmap.rows
     * only carry the real values after a render, and the line box below is
     * built from them. Measuring with FT_LOAD_DEFAULT leaves both at zero,
     * which collapses ascent/descent and pushes every glyph above the atlas. */
    if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
      continue;

    const int top = (int)face->glyph->bitmap_top;
    const int bottom = (int)face->glyph->bitmap.rows - top;
    if (top > ascent) ascent = top;
    if (bottom > descent) descent = bottom;

    cps[nglyphs] = cp;
    offsets[nglyphs] = pen_x;
    colours[nglyphs] = cur;
    pen_x += (int)(face->glyph->advance.x >> 6);
    nglyphs++;
  }

  if (nglyphs == 0)
    return 0;

  /* Centre the line box inside the atlas: the engine sizes the image to the
   * line, so anything else clips the ascenders. */
  const int base = ascent + (h - (ascent + descent)) / 2;

  for (int i = 0; i < nglyphs; i++) {
    if (FT_Load_Char(face, cps[i], FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
      continue;
    const FT_GlyphSlot g = face->glyph;
    const int gx = offsets[i];
    const int gy = base - g->bitmap_top;
    const unsigned colour = colours[i];
    const uint8_t cr = (colour >> 24) & 0xff;
    const uint8_t cg = (colour >> 16) & 0xff;
    const uint8_t cb = (colour >> 8) & 0xff;

    for (unsigned row = 0; row < g->bitmap.rows; row++) {
      const int y = gy + (int)row;
      if (y < 0 || y >= h)
        continue;
      for (unsigned col = 0; col < g->bitmap.width; col++) {
        const int x = gx + (int)col;
        if (x < 0 || x >= w)
          continue;
        const unsigned char a = g->bitmap.buffer[row * g->bitmap.pitch + col];
        if (!a)
          continue;
        unsigned char *d = buf + ((size_t)y * (size_t)w + (size_t)x) * 4;
        d[0] = cr;
        d[1] = cg;
        d[2] = cb;
        d[3] = a;
      }
    }
  }

  return pen_x;
}

/* Wrapper for single-colour rendering: the '$<letter>' pairs are dropped here
 * so they never turn into glyphs. */
int text_render_rgba(const char *utf8, int px, unsigned char *buf, int w, int h) {
  if (!utf8)
    return 0;
  char clean[1024];
  const char *s = clean_str(utf8, clean, (int)sizeof(clean));
  return text_render_rgba_ex(s, px, buf, w, h, 0);
}

void text_set_viewport(int w, int h) {
  viewport_w = w;
  viewport_h = h;
}

void text_draw(const char *utf8, int x, int y, unsigned color, int px) {
  TextCache *c = render(utf8, sane_px(px), color);
  blit(c, x, y, viewport_w, viewport_h);
}

void text_draw_rect(const char *utf8, int x, int y, int w, int h,
                    unsigned color, int px) {
  TextCache *c = render(utf8, sane_px(px), color);
  if (!c)
    return;
  /* centre inside the rect, the way Android's Canvas.drawText in a TextView does */
  blit(c, x + (w - c->w) / 2, y + (h - c->h) / 2, viewport_w, viewport_h);
}

/* ------------------------------------------------------------------------- */
/* text in the engine's coordinate space                                     */
/* ------------------------------------------------------------------------- */

/* The engine's own SGL_grpDrawStringWithFont measures with a font object that
 * ends up in Android's Paint/Typeface, which the fake Java shell cannot
 * provide -- on hardware that path is a Data Abort at SGL_RenderFontToAlpha-
 * ImageInRect+0x180, and in the emulator it simply drew nothing. So the
 * engine's call is answered here instead: same position, same colour, glyphs
 * from FreeType.
 *
 * Nothing about the matrices is changed on purpose. x/y arrive in the engine's
 * layout units and the engine's projection is already active, so emitting the
 * quad with the current matrices puts the text exactly where the engine asked
 * for it. */
static void text_draw_engine_line(const char *utf8, int x, int y, int px,
                                  unsigned color);
static void text_draw_rich_line(const char *utf8, int x, int y, int px,
                                unsigned base);

/* width, in the engine's layout units, that a line may occupy before it is
 * broken; 0 disables wrapping */
static int wrap_width;

void text_set_wrap_width(int w) { wrap_width = w > 0 ? w : 0; }


/* byte length of the UTF-8 sequence starting at p (1 for a stray byte) */
static int utf8_len(const char *p) {
  const unsigned char c = (unsigned char)*p;
  if (c < 0x80)
    return 1;
  if ((c & 0xE0) == 0xC0)
    return 2;
  if ((c & 0xF0) == 0xE0)
    return 3;
  if ((c & 0xF8) == 0xF0)
    return 4;
  return 1;
}

/* how many bytes the first n characters of s occupy */
static int bytes_for_chars(const char *s, int n) {
  int i = 0, c = 0;
  while (s[i] && c < n) {
    i += utf8_len(s + i);
    c++;
  }
  return i;
}

/* Wrapping state. The engine hands over one *pre-wrapped* line at a time and
 * draws the next line 30 px lower, so breaking a line into more pieces would
 * collide with the engine's own next line. The fix is to remember how many
 * extra lines the current paragraph has grown by and push every following line
 * of that paragraph down by the same amount. A paragraph ends when the engine
 * starts a new frame (y goes backwards). */
static int wrap_extra;
static int wrap_extra_on = 1;
static int wrap_last_y = -10000;
/* The push-down that keeps the engine's *next line* below the extra lines we
 * added only makes sense for a single block of text. A page that draws several
 * blocks at its own y positions (the skill description draws one per level) must
 * not accumulate it, or the second block lands a whole block lower. */
void text_set_wrap_extra(int on) {
  wrap_extra_on = on;
  if (!on)
    wrap_extra = 0;
}

void text_draw_engine(const char *utf8, int len, int x, int y, int px,
                      unsigned color) {
  text_draw_engine_ex(utf8, len, x, y, px, color, 0);
}

/* Similar to text_draw_engine but preserves $X colour codes if color_codes != 0.
 * This is used for string-image atlases where the engine doesn't consume codes. */
void text_draw_engine_ex(const char *utf8, int len, int x, int y, int px,
                         unsigned color, int color_codes) {
  if (!have_face || !utf8 || !*utf8)
    return;

  char raw[1024];
  int n = len >= 0 ? len : (int)strlen(utf8);
  if (n <= 0)
    return;
  if (n > (int)sizeof(raw) - 1)
    n = (int)sizeof(raw) - 1;
  memcpy(raw, utf8, (size_t)n);
  raw[n] = 0;

  /* If not using colour codes, strip them as before */
  char buf[1024];
  const char *source = raw;
  (void)buf;
  /* the codes are NOT stripped here any more: text_draw_rich_line below turns
   * each "$X" into a colour run, which is what the engine's own layout did */

  /* "&N" is the engine's own line break. Nothing else here understands it, so
   * it used to be drawn literally -- the class-confirm box read
   * "可以在具有不同能力的6个职业中任选一个。&N屏幕下方有对职业的详细介绍" on one
   * line, and the second half also showed up on its own elsewhere. Turn it into
   * a real newline before the line splitter runs. */
  char conv[1024];
  {
    int o = 0;
    for (const char *q = source; q && *q && o < (int)sizeof(conv) - 1; q++) {
      if (q[0] == '&' && (q[1] == 'N' || q[1] == 'n')) {
        conv[o++] = '\n';
        q++;
        continue;
      }
      conv[o++] = *q;
    }
    conv[o] = 0;
    source = conv;
  }

  /* A paragraph is drawn top-down, once per frame, and the push-down from a
   * wrapped line has to survive to the next line of that paragraph -- but not
   * to the next frame. `y < wrap_last_y` was not enough: a paragraph whose
   * lines all share one y (a single-line control redrawn every frame, the
   * save page's message) never went backwards, so every redraw added its own
   * wrap on top of the last and the line crept further down the screen. `<=`
   * resets whenever the draw does not advance, which is exactly "same line
   * again" and "new frame". */
  if (y <= wrap_last_y)
    wrap_extra = 0;
  int draw_y = y + (wrap_extra_on ? wrap_extra : 0);
  wrap_last_y = y;

  const int size = sane_px(px);
  const int line_h = size + size / 4;
  const int max_w = wrap_width > 0 ? wrap_width : 0;


  int line = 0;
  /* A mutable copy: the per-line split and the wrap loop both write into the
   * buffer they are walking. The old implementation did this on `buf`; the
   * colour-code rewrite turned it into a read-only const pointer and broke the
   * loop, which is what blanked every single-line string (all of the text
   * controls: save messages, class names, dialogue, the stat labels). */
  char work[1024];
  snprintf(work, sizeof(work), "%s", source);
  char *p = work;
  char carry = 0;            /* colour code still in force from the last line */
  while (p && *p) {
    char *nl = strchr(p, '\n');
    if (nl)
      *nl = 0;

    if (*p) {
      if (color_codes) {
        /* colour run path: the codes stay in the string and the renderer
         * paints each span; wrapping is not attempted here */
        text_draw_engine_line(p, x, draw_y + line * line_h, size, color);
        line++;
      } else {
        /* break this run so that it fits max_w; measured with the same
         * FreeType face that draws it, so the break lands where the glyphs
         * end. A line longer than max_w therefore becomes several quads. */
        char piece[1024];
        while (*p) {
          int take = (int)strlen(p);
          if (max_w > 0) {
            int w = 0, k = 0;
            take = 0;
            while (p[k]) {
              const int bytes = utf8_len(p + k);
              char save = p[k + bytes];
              p[k + bytes] = 0;
              const int add = text_measure_at(p + k, size);
              p[k + bytes] = save;
              if (w + add > max_w && take > 0)
                break;
              w += add;
              take++;
              k += bytes;
            }
            if (take == 0)
              take = 1;
          }
          const int bytes = bytes_for_chars(p, take);
          if (bytes <= 0)
            break;
          memcpy(piece, p, (size_t)bytes);
          piece[bytes] = 0;
          /* A wrap can land in the middle of a coloured span: "…$R刺杀格里" /
           * "普顿伯爵$B。" -- the second line starts with no code at all and
           * would be drawn in the base colour, while the original keeps it red.
           * Carry the last code seen onto the continuation. */
          char tmp[1040];
          const char *draw_piece = piece;
          const int starts_with_code =
              piece[0] == '$' && piece[1] &&
              ((piece[1] >= 'A' && piece[1] <= 'Z') ||
               (piece[1] >= 'a' && piece[1] <= 'z'));
          if (carry && !starts_with_code) {
            tmp[0] = '$';
            tmp[1] = carry;
            snprintf(tmp + 2, sizeof(tmp) - 2, "%s", piece);
            draw_piece = tmp;
          }
          for (const char *q = piece; *q; q++)
            if (q[0] == '$' && q[1] &&
                ((q[1] >= 'A' && q[1] <= 'Z') ||
                 (q[1] >= 'a' && q[1] <= 'z'))) {
              carry = q[1];
              q++;
            }
          if (strchr(draw_piece, '$'))
            text_draw_rich_line(draw_piece, x, draw_y + line * line_h, size,
                                color);
          else
            text_draw_engine_line(draw_piece, x, draw_y + line * line_h, size,
                                  color);
          line++;
          p += bytes;
        }
      }
    }
    if (!nl)
      break;
    p = nl + 1;
  }

  /* the engine's next line was laid out for the original count */
  if (line > 1 && wrap_extra_on)
    wrap_extra += (line - 1) * line_h;
}

/* The engine lays its whole UI out in its own units (1137x640 on this build)
 * and its quads carry those numbers directly, so this is the size the text
 * coordinates are expressed in -- NOT the framebuffer size. */
static int engine_w = 1137, engine_h = 640;

void text_set_engine_size(int w, int h) {
  if (w > 0)
    engine_w = w;
  if (h > 0)
    engine_h = h;
}

/* Display region for text, in engine units (x1 <= x0 = off). Glyph quads are
 * trimmed at its edges, the same way the original clips the scrolling
 * description, so the text slides under the frame instead of disappearing a
 * line at a time. */
static int clip_on, clip_x0, clip_y0, clip_x1, clip_y1;

/* A plain filled rectangle in engine units -- used for the description page's
 * scroll thumb, which the engine's own ControlScroll draws but never moves here
 * (its position is only fed by the scroll control's event handling, which this
 * page never sees). */
void text_fill_rect(int x, int y, int w, int h, unsigned color) {
  if (w <= 0 || h <= 0)
    return;
  const GLboolean tex_on = glIsEnabled(GL_TEXTURE_2D);
  const GLboolean blend_on = glIsEnabled(GL_BLEND);
  GLfloat prev[4] = { 1, 1, 1, 1 };
  glGetFloatv(GL_CURRENT_COLOR, prev);
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(((color >> 24) & 0xff) / 255.0f, ((color >> 16) & 0xff) / 255.0f,
            ((color >> 8) & 0xff) / 255.0f, (color & 0xff) / 255.0f);
  const GLfloat v[4][2] = { { (float)x, (float)y },
                            { (float)(x + w), (float)y },
                            { (float)x, (float)(y + h) },
                            { (float)(x + w), (float)(y + h) } };
  glEnableClientState(GL_VERTEX_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, v);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableClientState(GL_VERTEX_ARRAY);
  /* `v` is a local: the engine's SGL fills submit through one global array and
   * never re-specify the pointer, so leaving ours behind makes the next fill
   * read this dead stack frame -- which is exactly the smeared, repeated quads
   * that showed up once the scroll thumb started drawing. */
  nx_restore_client_arrays();
  glColor4f(prev[0], prev[1], prev[2], prev[3]);
  if (tex_on)
    glEnable(GL_TEXTURE_2D);
  if (!blend_on)
    glDisable(GL_BLEND);
}

void text_set_clip(int x0, int y0, int x1, int y1) {
  if (x1 <= x0 || y1 <= y0) {
    clip_on = 0;
    return;
  }
  clip_on = 1;
  clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
}

/* One textured quad. y grows downwards in the engine's layout, so the ortho
 * built here flips it to match; `own_ortho` ignores whatever matrices the
 * caller left active, which is the only thing that makes the placement
 * verifiable when the engine's own projection is in doubt. */
static void draw_quad(TextCache *c, int x, int y, int own_ortho) {
  const GLboolean tex_on = glIsEnabled(GL_TEXTURE_2D);
  const GLboolean blend_on = glIsEnabled(GL_BLEND);
  const GLboolean va_on = glIsEnabled(GL_VERTEX_ARRAY);
  const GLboolean ta_on = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  const GLboolean depth_on = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean cull_on = glIsEnabled(GL_CULL_FACE);
  const GLboolean alpha_on = glIsEnabled(GL_ALPHA_TEST);
  const GLboolean fog_on = glIsEnabled(GL_FOG);
  const GLboolean light_on = glIsEnabled(GL_LIGHTING);
  GLint prev_tex = 0;
  GLfloat prev_color[4] = { 1, 1, 1, 1 };
  GLfloat prev_proj[16], prev_mv[16];
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
  glGetFloatv(GL_CURRENT_COLOR, prev_color);
  if (own_ortho) {
    /* Save and restore the matrices by value rather than with
     * glPushMatrix/glPopMatrix. GLES1 guarantees only TWO projection stack
     * entries, and the engine already uses one of them while it draws its 2D
     * pass (pact2DBegin pushes GL_PROJECTION). A push on top of that overflows:
     * the push is dropped, the glLoadIdentity below then overwrites the
     * engine's own projection, and the matching pop pops *its* matrix -- so the
     * rest of that frame is drawn with an identity projection and the picture
     * jumps for one frame, every frame, which reads as the whole screen
     * twitching. */
    glGetFloatv(GL_PROJECTION_MATRIX, prev_proj);
    glGetFloatv(GL_MODELVIEW_MATRIX, prev_mv);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(0.0f, (float)engine_w, (float)engine_h, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
  }

  /* This runs in the middle of the world renderer's draw pass. Every one of
   * these has been seen to make a perfectly good 2D quad vanish: a depth
   * buffer full of terrain, back-face culling on a strip wound the other way,
   * the fog colour bleeding into the texture, a lit texture environment, or an
   * alpha test tuned for the engine's own glyph atlas. Text is always a flat
   * unlit unclipped sprite, so all of it is switched off for the draw. */
  if (depth_on) glDisable(GL_DEPTH_TEST);
  if (cull_on) glDisable(GL_CULL_FACE);
  if (alpha_on) glDisable(GL_ALPHA_TEST);
  if (fog_on) glDisable(GL_FOG);
  if (light_on) glDisable(GL_LIGHTING);

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, c->tex);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

  const GLfloat x0 = (float)x, y0 = (float)y;
  const GLfloat x1 = x0 + (float)c->w, y1 = y0 + (float)c->h;
  /* Pixel clipping at the region edge, the way the original cuts its scrolling
   * text: a line that crosses the edge is trimmed, not dropped. */
  float ax0 = x0, ay0 = y0, ax1 = x1, ay1 = y1;
  float tu0 = 0.0f, tv0 = 0.0f, tu1 = 1.0f, tv1 = 1.0f;
  if (clip_on) {
    if (x1 <= (float)clip_x0 || x0 >= (float)clip_x1 ||
        y1 <= (float)clip_y0 || y0 >= (float)clip_y1)
      return;
    if (ax0 < (float)clip_x0) { tu0 = ((float)clip_x0 - x0) / (x1 - x0); ax0 = (float)clip_x0; }
    if (ax1 > (float)clip_x1) { tu1 = ((float)clip_x1 - x0) / (x1 - x0); ax1 = (float)clip_x1; }
    if (ay0 < (float)clip_y0) { tv0 = ((float)clip_y0 - y0) / (y1 - y0); ay0 = (float)clip_y0; }
    if (ay1 > (float)clip_y1) { tv1 = ((float)clip_y1 - y0) / (y1 - y0); ay1 = (float)clip_y1; }
  }
  const GLfloat uvs[4][2] = { { tu0, tv0 }, { tu1, tv0 }, { tu0, tv1 }, { tu1, tv1 } };
  const GLfloat vtx[4][2] = { { ax0, ay0 }, { ax1, ay0 }, { ax0, ay1 }, { ax1, ay1 } };

  /* GLES1 has no immediate mode, so a client array is the only way to submit
   * the four vertices; the strip is wound both ways by the two triangles of a
   * GL_TRIANGLE_STRIP, and culling is off above, so the winding cannot matter. */
  glEnableClientState(GL_VERTEX_ARRAY);
  glEnableClientState(GL_TEXTURE_COORD_ARRAY);
  glVertexPointer(2, GL_FLOAT, 0, vtx);
  glTexCoordPointer(2, GL_FLOAT, 0, uvs);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  if (!ta_on)
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
  if (!va_on)
    glDisableClientState(GL_VERTEX_ARRAY);
  /* Both arrays above are locals of this frame. The engine's SGL GRP drawers
   * (SGL_grpFillRect, the NPC name plate's backing bar among them) submit
   * through one global array and never re-specify the pointer, so leaving ours
   * behind makes the next such fill read this dead stack frame: a black
   * 4-vertex strip with whatever numbers happen to be there, i.e. a whole-screen
   * flash. Put the engine's own pointers back. */
  nx_restore_client_arrays();

  glColor4f(prev_color[0], prev_color[1], prev_color[2], prev_color[3]);
  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  if (!blend_on)
    glDisable(GL_BLEND);
  if (!tex_on)
    glDisable(GL_TEXTURE_2D);
  if (light_on) glEnable(GL_LIGHTING);
  if (fog_on) glEnable(GL_FOG);
  if (alpha_on) glEnable(GL_ALPHA_TEST);
  if (cull_on) glEnable(GL_CULL_FACE);
  if (depth_on) glEnable(GL_DEPTH_TEST);
  if (own_ortho) {
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(prev_proj);
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(prev_mv);
  }
}

/* Read the framebuffer back where the quad should have landed and count how
 * many sampled pixels differ from the colour just outside it. This separates
 * "the glyphs never became a texture" from "they were drawn somewhere/somehow
 * invisible", which no amount of logging of GL state can tell apart. */
static void probe_quad(TextCache *c, int x, int y, GLenum err) {
  static int budget = 12;
  if (budget <= 0)
    return;
  budget--;

  const int fbw = viewport_w, fbh = viewport_h;
  int sx0 = x * fbw / engine_w;
  int sx1 = (x + c->w) * fbw / engine_w;
  int sy0 = fbh - (y + c->h) * fbh / engine_h;
  int sy1 = fbh - y * fbh / engine_h;
  if (sx1 <= sx0)
    sx1 = sx0 + 1;
  if (sy1 <= sy0)
    sy1 = sy0 + 1;
  if (sx0 < 0) sx0 = 0;
  if (sy0 < 0) sy0 = 0;
  if (sx1 > fbw) sx1 = fbw;
  if (sy1 > fbh) sy1 = fbh;
  if (sx0 >= sx1 || sy0 >= sy1) {
    debugPrintf("text: probe quad %d,%d %dx%d is off-screen (fb %dx%d)\n", x, y,
                c->w, c->h, fbw, fbh);
    return;
  }

  unsigned char bg[4] = { 0, 0, 0, 0 }, t[4], best[4] = { 0, 0, 0, 0 };
  int bx = sx0 - 6;
  if (bx < 0)
    bx = sx1 + 6;
  if (bx >= fbw)
    bx = 0;
  glReadPixels(bx, (sy0 + sy1) / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bg);

  int total = 0, diff = 0;
  for (int yy = sy0; yy < sy1; yy += 2) {
    for (int xx = sx0; xx < sx1; xx += 2) {
      glReadPixels(xx, yy, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, t);
      total++;
      if (abs((int)t[0] - bg[0]) > 8 || abs((int)t[1] - bg[1]) > 8 ||
          abs((int)t[2] - bg[2]) > 8) {
        diff++;
        if (t[0] + t[1] + t[2] > best[0] + best[1] + best[2])
          memcpy(best, t, 4);
      }
    }
  }
  debugPrintf("text: probe tex %u %dx%d quad %d,%d win %d,%d %dx%d bg "
              "%02x%02x%02x%02x brightest %02x%02x%02x%02x diff %d/%d err %#x\n",
              c->tex, c->w, c->h, x, y, sx0, sy0, sx1 - sx0, sy1 - sy0, bg[0],
              bg[1], bg[2], bg[3], best[0], best[1], best[2], best[3], diff,
              total, err);
}

/* Does the currently active projection+modelview map the engine's layout rect
 * (0,0)-(engine_w,engine_h) onto the whole screen?
 *
 * This is the question the story text got wrong. The popup drawers set up a 2D
 * projection and their strings land correctly when the quad is emitted through
 * the current matrices. TEXTCTRL2 -- the story box and the tutorial body --
 * runs right after the map renderer and inherits a projection that does NOT
 * map the layout onto the screen, so the same quad ends up outside the
 * viewport (or in a corner) and the text is invisible while every matrix and
 * coordinate in the log looks sane.
 *
 * So: measure it. If the two corners of the layout rect project to the corners
 * of the viewport, the call site is 2D and we submit through its matrices (it
 * may be inheriting a scale that we would otherwise lose). If not, the
 * geometry is ours to build. */
static int caller_maps_ui(void) {
  GLfloat p[16], m[16], c[16];
  glGetFloatv(GL_PROJECTION_MATRIX, p);
  glGetFloatv(GL_MODELVIEW_MATRIX, m);
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      GLfloat s = 0.0f;
      for (int k = 0; k < 4; k++)
        s += p[k * 4 + row] * m[col * 4 + k];
      c[col * 4 + row] = s;
    }
  }

  float nx[2], ny[2];
  const float xs[2] = { 0.0f, (float)engine_w };
  const float ys[2] = { 0.0f, (float)engine_h };
  for (int i = 0; i < 2; i++) {
    const float x = xs[i], y = ys[i];
    const float w = c[3] * x + c[7] * y + c[15];
    if (w > -0.0001f && w < 0.0001f)
      return 0;
    nx[i] = (c[0] * x + c[4] * y + c[12]) / w;
    ny[i] = (c[1] * x + c[5] * y + c[13]) / w;
  }
  /* engine x grows right, engine y grows down: left-top corner is (-1,+1) */
  const int ok = fabsf(nx[0] + 1.0f) < 0.20f && fabsf(nx[1] - 1.0f) < 0.20f &&
                 fabsf(ny[0] - 1.0f) < 0.20f && fabsf(ny[1] + 1.0f) < 0.20f;
  /* Log each verdict separately: the popup drawers run first and would use up
   * a shared budget before the story text is ever reached. */
  static int ok_logs, bad_logs;
  if (ok ? ok_logs < 3 : bad_logs < 6) {
    if (ok)
      ok_logs++;
    else
      bad_logs++;
    debugPrintf("text: layout -> (%+.2f,%+.2f)..(%+.2f,%+.2f) ndc, %s\n", nx[0],
                ny[0], nx[1], ny[1],
                ok ? "using the caller's matrices"
                   : "caller's matrices are not 2D -- using our own ortho");
  }
  return ok;
}

/* Draw one line, honouring the '$<letter>' colour changes inside it. The
 * engine's strings carry their emphasis that way -- "黑暗骑士团营地" on the save
 * page is the dark-gold one, "每日奖励" in the hint is $S blue -- and stripping
 * the codes (what the port used to do) is what turned every line plain white. */
static void text_draw_rich_line(const char *utf8, int x, int y, int px,
                                unsigned base) {
  if (!utf8 || !*utf8)
    return;
  char run[1024];
  int o = 0;
  int pen = x;
  unsigned cur = base;
  /* Every run of one line is drawn at the same y: render() pads each glyph box
   * to the face's ascender/descender, so runs of one size already share a
   * baseline. Aligning each run by its own ink (what this used to do) pushed a
   * run holding nothing but "。" far down, which is why that full stop kept
   * disappearing. */
  for (const char *p = utf8;; p++) {
    if (*p == '$' && p[1]) {
      const unsigned char id = (unsigned char)p[1];
      if ((id >= 'A' && id <= 'Z') || (id >= 'a' && id <= 'z')) {
        if (o > 0) {
          run[o] = 0;
          text_draw_engine_line(run, pen, y, px, cur);
          pen += text_measure_at(run, px);
          o = 0;
        }
        note_code(id);
        cur = colour_for_code(id);
        if (cur == 0xFFFFFFFFu)
          cur = base;              /* $B returns to the caller's own colour */
        p++;
        continue;
      }
    }
    if (!*p)
      break;
    if (o < (int)sizeof(run) - 1)
      run[o++] = *p;
  }
  if (o > 0) {
    run[o] = 0;
    {
      static int rl_logs;
      if (rl_logs < 16 && strstr(utf8, "伯爵")) {
        rl_logs++;
        debugPrintf("rich: run '%s' at %d col %08x (line y %d)\n", run, pen, cur,
                    y);
      }
    }
    text_draw_engine_line(run, pen, y, px, cur);
  }
}

static void text_draw_engine_line(const char *utf8, int x, int y, int px,
                                  unsigned color) {
  TextCache *c = render(utf8, sane_px(px), color);
  if (!c || !c->tex) {
    static int null_logs;
    if (null_logs < 4) {
      null_logs++;
      debugPrintf("text: render produced nothing for '%d bytes' (%d px)\n",
                  (int)strlen(utf8), px);
    }
    return;
  }

  /* Which drawer paints the quest popup? Its text does not show up in the
   * text-control log, so this names the entry point that actually draws it. */
  {
    static int qn;
    if (qn < 12 && (strstr(utf8, "伯爵") || strstr(utf8, "复命"))) {
      qn++;
      debugPrintf("quest: [%s] at %d,%d px %d len %d '%s'\n", g_caller, x, y,
                  px, (int)strlen(utf8), utf8);
    }
  }

  /* Who draws a short line in the dialogue band? The speaker's name is a short
   * string above the body, and every drawer's own log has a budget that is long
   * spent by the time a dialogue appears, so this one names the drawer. */
  {
    static struct { char text[32]; const char *who; } seen[12];
    static int n;
    const size_t len = strlen(utf8);
    if (y > 420 && y < 580 && len > 0 && len < 16) {
      int known = 0;
      for (int i = 0; i < n; i++)
        if (!strcmp(seen[i].text, utf8) && seen[i].who == g_caller) {
          known = 1;
          break;
        }
      if (!known && n < (int)(sizeof(seen) / sizeof(*seen))) {
        snprintf(seen[n].text, sizeof(seen[n].text), "%s", utf8);
        seen[n].who = g_caller;
        n++;
        debugPrintf("evt: '%s' drawn by '%s' at %d,%d px %d col %08x\n", utf8,
                    g_caller, x, y, px, color);
      }
    }
  }

  if (cfg.text_trace) {
    /* Only the story-text band: the log budget has to survive until the
     * dialogue shows up, and a full UI frame is dozens of strings. */
    static int dl;
    if (dl < 900 && y >= 400 && y <= 620) {
      dl++;
      debugPrintf("text: draw [%s] (%d,%d) px %d col %06x '%s'\n", g_caller, x, y,
                  px > 0 ? px : default_px, color >> 8, utf8);
    }
  }
  {
    const int use_caller = caller_maps_ui();
    if (cfg.text_trace && y >= 400 && y <= 620) {
      /* Only the first sighting of each distinct geometry: the interesting
       * screens are reached late, and a plain line cap gets eaten by the HUD
       * and the stat page long before then. */
      static struct { int x, y, w, h; } seen[24];
      static int nseen;
      int known = 0;
      for (int i = 0; i < nseen; i++)
        if (seen[i].x == x && seen[i].y == y && seen[i].w == c->w &&
            seen[i].h == c->h) { known = 1; break; }
      if (!known && nseen < 24) {
        seen[nseen].x = x;
        seen[nseen].y = y;
        seen[nseen].w = c->w;
        seen[nseen].h = c->h;
        nseen++;
        GLfloat pm[16], mv[16];
        glGetFloatv(GL_PROJECTION_MATRIX, pm);
        glGetFloatv(GL_MODELVIEW_MATRIX, mv);
        debugPrintf("text: geom x=%d y=%d w=%d h=%d caller=%d "
                    "proj %.4f %.4f %.4f %.4f | mv %.4f %.4f %.4f %.4f | "
                    "eng %dx%d vp %dx%d\n",
                    x, y, c->w, c->h, use_caller,
                    pm[0], pm[5], pm[12], pm[13],
                    mv[0], mv[5], mv[12], mv[13],
                    engine_w, engine_h, viewport_w, viewport_h);
      }
    }
    draw_quad(c, x, y, !use_caller);
  }

  /* Verify from the framebuffer that the quad really changed pixels: without
   * this, "the string was submitted" and "the string is on screen" look
   * identical in the log. */
  /* The framebuffer probe that used to run here is gone: one glReadPixels per
   * sampled pixel is thousands of synchronous reads through Mesa's software
   * rasteriser, and on the story screen it pushed a frame from 20 ms to over
   * three seconds. The watchdog then reported "no frame for 3 s" and the frame
   * was never presented -- which is exactly why the story text looked like it
   * was never drawn, even though the readback had proved the quad rasterised. */
}

/* Break a line at the width the engine laid it out for.
 *
 * A text control is given a box width and wraps its own string image to it, but
 * the string it hands this port is unwrapped -- one long line. Drawing that
 * line as it comes is what pushed the quest popup's text out past its frame:
 * the box is 410 px wide and the line ran to 24 characters. CJK breaks between
 * any two characters, so that is what this does; an ASCII space at the break is
 * dropped. Returns the number of lines written. */
int text_wrap_to_box(const char *utf8, int box_w, int px, char *dst, int cap) {
  if (!utf8 || !dst || cap < 2 || box_w <= 0 || px <= 0)
    return 0;
  int o = 0, lines = 1, line_w = 0;
  const unsigned char *p = (const unsigned char *)utf8;
  char ch[8];
  while (*p && o < cap - 2) {
    int n = 1;
    if (*p >= 0xF0)
      n = 4;
    else if (*p >= 0xE0)
      n = 3;
    else if (*p >= 0xC0)
      n = 2;
    if (o + n >= cap - 1)
      break;
    /* "$<letter>" is a colour change, not two glyphs: it has to be carried
     * along with the line it belongs to and must not take up any width, or a
     * rich line (the popup title, the quest body) wraps far too early. */
    if (p[0] == '&' && (p[1] == 'N' || p[1] == 'n')) {
      dst[o++] = '\n';
      lines++;
      line_w = 0;
      p += 2;
      continue;
    }
    if (p[0] == '$' && p[1] &&
        (((p[1] >= 'A' && p[1] <= 'Z') || (p[1] >= 'a' && p[1] <= 'z')))) {
      if (o + 2 >= cap - 1)
        break;
      dst[o++] = '$';
      dst[o++] = (char)p[1];
      p += 2;
      continue;
    }
    memcpy(ch, p, (size_t)n);
    ch[n] = 0;
    if (ch[0] == '\n') {
      dst[o++] = '\n';
      lines++;
      line_w = 0;
      p += n;
      continue;
    }
    const int cw = text_measure_at(ch, px);
    /* a couple of pixels of slack: the engine's box and our measurement do not
     * agree exactly, and without it a trailing "。" gets pushed onto a line of
     * its own */
    if (line_w + cw > box_w + 6 && line_w > 0) {
      /* drop the space we are breaking on, so the next line does not start
       * with one */
      if (ch[0] == ' ') {
        p += n;
        dst[o++] = '\n';
        lines++;
        line_w = 0;
        continue;
      }
      dst[o++] = '\n';
      lines++;
      line_w = 0;
    }
    memcpy(dst + o, ch, (size_t)n);
    o += n;
    line_w += cw;
    p += n;
  }
  dst[o] = 0;
  /* A trailing break (the engine's strings often end with "&N" or a newline)
   * would add an empty line: no glyphs, but the block's height counts it, which
   * showed up as a blank line between the description and the dashed
   * separator. */
  while (o > 0 && (dst[o - 1] == '\n' || dst[o - 1] == '\r')) {
    dst[--o] = 0;
    if (lines > 1)
      lines--;
  }
  return lines;
}

int text_width_engine(const char *utf8, int len, int px) {
  if (!utf8)
    return 0;
  char buf[512];
  int n = len >= 0 ? len : (int)strlen(utf8);
  if (n <= 0)
    return 0;
  if (n > (int)sizeof(buf) - 1)
    n = (int)sizeof(buf) - 1;
  memcpy(buf, utf8, (size_t)n);
  buf[n] = 0;
  return text_measure_at(buf, px);
}
