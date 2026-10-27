/* inotia_font.c -- the glyph source the engine asks Java for (stage 1f).
 *
 * libgame.so imports no FreeType and no Skia symbol, and CFont's native entry
 * points take a Java Typeface: the engine does not rasterise its own text, it
 * asks the platform for glyph bitmaps. On a phone that is
 * android/graphics/Paint + Canvas + Typeface.
 *
 * This is the one layer with no precedent in the android32 ports (their games
 * all draw their own fonts), so it is also the piece that decides how much work
 * this port really is. Two ways, in order of preference:
 *
 *   1. hook the engine's own font entry (CS_font*, the symbols around
 *      CFont.nativeFontInitialize) and rasterise with stb_truetype: no Java at
 *      all, and the glyph format is the engine's own, so it is one hook instead
 *      of an android/graphics subset.
 *   2. implement Typeface + Paint.measureText + Canvas.drawText + Bitmap in the
 *      JNI table. Much more code, and every metric has to be plausible or the
 *      text lands in the wrong place.
 *
 * Until one of them is done, this stub answers with an empty bitmap: the engine
 * keeps running and the log says what it asked for, which is what stage 1b
 * needs to see.
 *
 * MIT.
 */
#include <stdlib.h>
#include <string.h>

#include "inotia.h"
#include "jni.h"
#include "util.h"

JObj *inotia_font_bitmap(const char *utf8, int px, int *w, int *h) {
  static int warned;
  if (!warned) {
    debugPrintf("[font] glyph request for \"%.32s\" at %d px: no rasteriser yet (stage 1f)\n",
                utf8 ? utf8 : "", px);
    warned = 1;
  }
  if (w)
    *w = 0;
  if (h)
    *h = 0;
  return jni_new("android/graphics/Bitmap");
}
