/* egl_helper.c -- EGL + OpenGL ES 1.1 context over the default NWindow
 *
 * Inotia 4 renders through the ES 1.1 fixed pipeline; mesa on the Switch
 * (swrast over nouveau) provides the whole feature set the binary imports,
 * including the 16.16 fixed-point entry points, so no translation layer is
 * needed -- see tools/check_gles.py for the symbol audit that confirms it.
 *
 * Derived from NaGaa95/swordigo_nx's egl_helper.c (MIT).
 */

#include <switch.h>
#include <EGL/egl.h>
#include <GLES/gl.h>

#include "egl_helper.h"
#include "util.h"
#include "error.h"

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLConfig  s_config;
static int s_width, s_height;

void egl_init(int w, int h) {
  NWindow *win = nwindowGetDefault();
  /* the window must be sized before the surface exists, otherwise mesa latches
   * the default 720p framebuffer and everything is stretched */
  nwindowSetDimensions(win, w, h);

  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (s_display == EGL_NO_DISPLAY)
    fatal_error("eglGetDisplay failed (0x%x)", eglGetError());
  if (!eglInitialize(s_display, NULL, NULL))
    fatal_error("eglInitialize failed (0x%x)", eglGetError());
  if (!eglBindAPI(EGL_OPENGL_ES_API))
    fatal_error("eglBindAPI failed (0x%x)", eglGetError());

  const EGLint cfg_attr[] = {
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      24,
    EGL_STENCIL_SIZE,    8,
    EGL_NONE
  };
  EGLint num_config = 0;
  if (!eglChooseConfig(s_display, cfg_attr, &s_config, 1, &num_config) || num_config == 0)
    fatal_error("no ES1-capable EGL config (0x%x)", eglGetError());

  s_surface = eglCreateWindowSurface(s_display, s_config, win, NULL);
  if (s_surface == EGL_NO_SURFACE)
    fatal_error("eglCreateWindowSurface failed (0x%x)", eglGetError());

  const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
  s_context = eglCreateContext(s_display, s_config, EGL_NO_CONTEXT, ctx_attr);
  if (s_context == EGL_NO_CONTEXT)
    fatal_error("eglCreateContext failed (0x%x)", eglGetError());

  if (!eglMakeCurrent(s_display, s_surface, s_surface, s_context))
    fatal_error("eglMakeCurrent failed (0x%x)", eglGetError());

  eglSwapInterval(s_display, 1);
  s_width = w;
  s_height = h;

  const char *ver = (const char *)glGetString(GL_VERSION);
  const char *ren = (const char *)glGetString(GL_RENDERER);
  debugPrintf("egl: ES1 context %dx%d\n  version: %s\n  renderer: %s\n",
              w, h, ver ? ver : "?", ren ? ren : "?");
}

void egl_swap(void) {
  if (s_display != EGL_NO_DISPLAY && s_surface != EGL_NO_SURFACE)
    eglSwapBuffers(s_display, s_surface);
}

void egl_resize(int w, int h) {
  if (w == s_width && h == s_height)
    return;
  NWindow *win = nwindowGetDefault();
  nwindowSetDimensions(win, w, h);

  EGLSurface old = s_surface;
  EGLSurface fresh = eglCreateWindowSurface(s_display, s_config, win, NULL);
  if (fresh == EGL_NO_SURFACE) {
    debugPrintf("egl: resize failed (0x%x)\n", eglGetError());
    return;
  }
  if (!eglMakeCurrent(s_display, fresh, fresh, s_context)) {
    debugPrintf("egl: makeCurrent after resize failed (0x%x)\n", eglGetError());
    return;
  }
  eglDestroySurface(s_display, old);
  s_surface = fresh;
  s_width = w;
  s_height = h;
  debugPrintf("egl: resized to %dx%d\n", w, h);
}

/* An EGL context is current per thread. The engine runs on its own thread so it
 * can have a large stack, which means the context created here has to be made
 * current again over there before the first GL call. */
void egl_release_thread(void) {
  if (s_display != EGL_NO_DISPLAY)
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

int egl_bind_thread(void) {
  if (s_display == EGL_NO_DISPLAY || s_context == EGL_NO_CONTEXT)
    return 0;
  if (!eglMakeCurrent(s_display, s_surface, s_surface, s_context))
    return 0;
  return 1;
}

void egl_deinit(void) {
  if (s_display == EGL_NO_DISPLAY)
    return;
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context != EGL_NO_CONTEXT) eglDestroyContext(s_display, s_context);
  if (s_surface != EGL_NO_SURFACE) eglDestroySurface(s_display, s_surface);
  eglTerminate(s_display);
  s_display = EGL_NO_DISPLAY;
  s_context = EGL_NO_CONTEXT;
  s_surface = EGL_NO_SURFACE;
}
