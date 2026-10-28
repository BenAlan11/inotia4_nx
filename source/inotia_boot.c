/* inotia_boot.c -- port_load() and port_run() for Inotia 4.
 *
 * port_load: the APK's libgame.so out of the APK, mapped, relocated, its
 * imports bound, sealed. port_run: an OpenGL ES 1 context on the default
 * window, the engine's constructors, the fake Java VM, then the entry points
 * the game's MainActivity would have called -- in the order the Java side
 * calls them.
 *
 * The engine is a C engine (Com2uS "GEM_LIB", sources named Ino3_UI.c,
 * s_character.c) behind a thin Java wrapper (com.com2us.wrapper.*). It creates
 * no EGL surface of its own: on Android the GLSurfaceView does, and the engine
 * only draws on the thread that calls nativeProcess. That is why this file
 * makes the context current before the first entry point, and why the frame
 * loop calls nativeProcess + eglSwapBuffers itself.
 *
 * Stage 0/1a: every entry point is called through a pointer resolved by name
 * and logged. Whatever the order or the argument count turns out to be, the
 * log says which call failed -- that is the whole point of this stage.
 *
 * MIT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>

#include <EGL/egl.h>
#include <GLES/gl.h>

#include "dcr_boost.h"
#include "dcr_manifest.h"
#include "dcr_path.h"
#include "dcr_setup.h"
#include "error.h"
#include "gl_layer.h"
#include "imports.h"
#include "inotia.h"
#include "jni.h"
#include "rt_applet.h"
#include "rt_audout.h"
#include "rt_cfg.h"
#include "rt_settings.h"
#include "rt_window.h"
#include "so_util.h"
#include "util.h"

so_module g_game;
static uint64_t g_frames;

/* --------------------------------------------------------------- setup plan */

#define ASSETS_DIR "assets/common/"
#define RES_ROOT "Android/data/" PORT_PACKAGE "/files/resources"

static const char *const k_libs[] = {"libgame.so"};

/* assets/common/** -> <root>/Android/data/<package>/files/resources/common/**
 * The engine looks for its resources there (it builds the path from
 * GetPackageName()), so the copy the Java side used to do happens here.
 * Stamped with the APK's versionCode: a new APK unpacks again. */
static void extract_assets(RtSetupCtx *ctx) {
  if (!ctx->apk) {
    debugPrintf("[setup] no APK open for the assets\n");
    return;
  }
  unsigned long crc = 0, size = 0;
  if (rt_setup_stamp_get("assets", &crc, &size) && crc == (unsigned long)dcr_manifest_version_code()) {
    debugPrintf("[setup] assets already unpacked for versionCode %d\n", dcr_manifest_version_code());
    return;
  }

  const size_t prefix = strlen(ASSETS_DIR);
  const mz_uint total = mz_zip_reader_get_num_files(ctx->apk);
  char name[MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE];
  char dst[DCR_PATH_MAX];
  int done = 0;

  for (mz_uint i = 0; i < total; i++) {
    if (!mz_zip_reader_get_filename(ctx->apk, i, name, sizeof name))
      continue;
    if (strncmp(name, ASSETS_DIR, prefix) != 0)
      continue;
    if (name[strlen(name) - 1] == '/')
      continue;
    snprintf(dst, sizeof dst, "%s/%s/common/%s", dcr_game_root(), RES_ROOT, name + prefix);
    if (rt_setup_extract_entry(ctx->apk, (int)i, dst, "Unpacking the game's assets", ctx->p0, ctx->p1) == 0)
      done++;
  }

  debugPrintf("[setup] assets: %d files -> %s/common/\n", done, RES_ROOT);
  rt_setup_stamp_set("assets", (unsigned long)dcr_manifest_version_code(), (unsigned long)done);
  rt_setup_stamp_save();
}

static const RtSetupStep k_steps[] = {
    {"assets", RT_STEP_WITH_ZIP, 300, 990, extract_assets},
};

const RtSetupPlan port_setup_plan = {
    .libs = k_libs,
    .nlibs = 1,
    .libs_what = "Unpacking Inotia 4's engine",
    .apk_requirement = "This port needs Inotia 4 1.3.7 for 32-bit ARM (armeabi-v7a).",
    .libs_p0 = 100,
    .libs_p1 = 300,
    .classes_p0 = 300,
    .classes_p1 = 320,
    .steps = k_steps,
    .nsteps = 1,
};

/* ------------------------------------------------------------------ loading */

/* Every entry point the game's Java side calls, resolved by name. */
typedef struct {
  const char *sym;
  uintptr_t addr;
} InotiaEntry;

#define ENTRY(n) "Java_com_com2us_" n

static const char *const k_known[] = {
    ENTRY("wrapper_kernel_CWrapper_nativeInitialize"),
    ENTRY("wrapper_kernel_CWrapper_nativeSetClass"),
    ENTRY("wrapper_kernel_CWrapperKernel_nativeStart"),
    ENTRY("wrapper_kernel_CWrapperKernel_nativeProcess"),
    ENTRY("wrapper_kernel_CWrapperKernel_nativeSurfaceRecreated"),
    ENTRY("wrapper_kernel_CWrapperData_nativeSetScreenSize"),
    ENTRY("wrapper_common_CCommonAPI_nativeInitialize"),
    ENTRY("wrapper_kernel_CEventHandler_nativeEventHandlerInitialize"),
    ENTRY("wrapper_font_CFont_nativeFontInitialize"),
    ENTRY("wrapper_sensor_CSensor_nativeSensorInitialize"),
    NULL,
};

static char g_apk_path[DCR_PATH_MAX];

const char *inotia_apk_path(void) { return g_apk_path; }

int port_load(const char *apk) {
  snprintf(g_apk_path, sizeof g_apk_path, "%s", apk ? apk : "");
  dcr_setup_from_apk(apk);

  char path[DCR_PATH_MAX];
  snprintf(path, sizeof path, "%s/libgame.so", dcr_game_root());
  debugPrintf("[load] %s\n", path);

  if (so_load(&g_game, path, NULL, PORT_SO_REGION_BYTES) < 0)
    fatal_error("Could not load %s.\nIs the APK the 1.3.7 armeabi-v7a build? See debug.log.", path);

  if (so_relocate(&g_game) < 0)
    fatal_error("Relocation of libgame.so failed (see debug.log).");

  const int missing = so_resolve(&g_game, dcr_imports, dcr_imports_count, 1);
  const int kuser = so_fix_kuser_helpers(&g_game);
  debugPrintf("[load] mapped %u KB, unresolved %d, kuser literals %d\n",
              (unsigned)(g_game.load_size >> 10), missing, kuser);
  if (missing > 0)
    fatal_error("%d imports of libgame.so are unresolved. Send debug.log.", missing);

  so_finalize(&g_game);
  so_flush_caches(&g_game);

  for (int i = 0; k_known[i]; i++)
    debugPrintf("[load] %-88s %p\n", k_known[i], (void *)so_try_find_addr_rx(&g_game, k_known[i]));
  log_flush_ring();
  return 0;
}

/* -------------------------------------------------------------------- gfx */

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLSurface g_surf = EGL_NO_SURFACE;
static EGLContext g_ctx = EGL_NO_CONTEXT;

static int gfx_init(void) {
  if (log_console_active())
    log_console_close();
  dcr_window_prepare();

  g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major = 0, minor = 0;
  if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, &major, &minor)) {
    debugPrintf("[gfx] eglInitialize failed 0x%x\n", eglGetError());
    return -1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);

  /* GLES 1: the engine's imports are the fixed-function ones (glColor4x,
   * glOrthox, glMatrixMode, glVertexPointer...). Mesa serves them through
   * libGLESv1_CM, so the config asks for the ES1 renderable bit. */
  static const EGLint attrs[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
      EGL_DEPTH_SIZE, 24, EGL_NONE};
  EGLConfig cfg = 0;
  EGLint n = 0;
  if (!eglChooseConfig(g_dpy, attrs, &cfg, 1, &n) || n < 1) {
    static const EGLint fallback[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT,
                                      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    debugPrintf("[gfx] no 24-bit depth config, trying the fallback\n");
    if (!eglChooseConfig(g_dpy, fallback, &cfg, 1, &n) || n < 1) {
      debugPrintf("[gfx] no GLES1 config 0x%x\n", eglGetError());
      return -1;
    }
  }

  static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE};
  g_surf = eglCreateWindowSurface(g_dpy, cfg, (EGLNativeWindowType)nwindowGetDefault(), NULL);
  g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
  if (g_surf == EGL_NO_SURFACE || g_ctx == EGL_NO_CONTEXT ||
      !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
    debugPrintf("[gfx] surface/context/make-current failed 0x%x\n", eglGetError());
    return -1;
  }
  eglSwapInterval(g_dpy, 1);

  int w = 0, h = 0;
  dcr_window_size(&w, &h);
  glViewport(0, 0, w, h);
  debugPrintf("[gfx] EGL %d.%d %dx%d | %s | %s\n", major, minor, w, h,
              (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
  log_flush_ring();
  return 0;
}

/* --------------------------------------------------------------- the game */

typedef int (*fn_env)(void *env);
typedef int (*fn_env_obj)(void *env, void *obj);
typedef int (*fn_env_obj_obj)(void *env, void *obj, void *a);
typedef int (*fn_env_obj_1i)(void *env, void *obj, int a);
typedef int (*fn_env_obj_6i)(void *env, void *obj, int a, int b, int c, int d, int e, int f);

/* The module names CWrapper.nativeInitialize dispatches on: they are the
 * literals in libgame.so, and they are exactly the Java classes that extend
 * CWrapper and reach onCreate(name, true). Each one's native init caches the
 * jclass and jmethodIDs that module will call back through, so all of them have
 * to run before nativeStart.
 *
 * CWrapperKernel and CWrapperData are deliberately absent: they are not keys
 * (the engine has no literal for them) and their own natives are called
 * directly below. */
static const char *const k_modules[] = {
    "com.com2us.wrapper.function.CFunctionTaskRunner",
    "com.com2us.wrapper.utility.CUtility",
    "com.com2us.wrapper.network.CHttpManager",
    "com.com2us.wrapper.game.CGameManager",
    "com.com2us.wrapper.kernel.CEventHandler",
    "com.com2us.wrapper.media.CSoundManager",
    "com.com2us.wrapper.sensor.CSensor",
    "com.com2us.wrapper.ui.CTextInput",
    "com.com2us.wrapper.ui.CUserInput",
    "com.com2us.wrapper.font.CFont",
    "com.com2us.wrapper.font.CSGLFont",
    "com.com2us.wrapper.WrapperUserDefined",
    NULL,
};

static void *entry(const char *sym) { return (void *)so_try_find_addr_rx(&g_game, sym); }

static void call0(fn_env_obj f, void *obj, const char *what) {
  if (!f) {
    debugPrintf("[game] %s: MISSING\n", what);
    return;
  }
  debugPrintf("[game] -> %s\n", what);
  log_flush_ring();
  const int rc = f(g_jni_env, obj);
  debugPrintf("[game] <- %s = %d\n", what, rc);
  log_flush_ring();
}

void port_run(void) {
  int w = 0, h = 0;
  dcr_window_size(&w, &h);

  if (gfx_init() != 0) {
    debugPrintf("[game] no GL context: stopping before the engine starts\n");
    log_flush_ring();
    return;
  }

  debugPrintf("[game] constructors\n");
  log_flush_ring();
  so_execute_init_array(&g_game);

  jni_init();
  debugPrintf("[game] JNIEnv %p JavaVM %p\n", g_jni_env, g_jni_vm);

  /* The engine's own JNI_OnLoad registers its natives and caches the
   * jmethodIDs it will call back through. */
  fn_env_obj onload = (fn_env_obj)entry("JNI_OnLoad");
  if (onload) {
    const int rc = onload(g_jni_vm, NULL);
    debugPrintf("[game] JNI_OnLoad -> 0x%x\n", rc);
    log_flush_ring();
  }

  fn_env_obj set_class = (fn_env_obj)entry(ENTRY("wrapper_kernel_CWrapper_nativeSetClass"));
  fn_env_obj_obj init_module = (fn_env_obj_obj)entry(ENTRY("wrapper_kernel_CWrapper_nativeInitialize"));
  fn_env_obj_1i data_init =
      (fn_env_obj_1i)entry(ENTRY("wrapper_kernel_CWrapperData_nativeInitialize"));
  fn_env_obj_6i set_size =
      (fn_env_obj_6i)entry(ENTRY("wrapper_kernel_CWrapperData_nativeSetScreenSize"));
  fn_env_obj surface = (fn_env_obj)entry(ENTRY("wrapper_kernel_CWrapperKernel_nativeSurfaceRecreated"));
  fn_env_obj start = (fn_env_obj)entry(ENTRY("wrapper_kernel_CWrapperKernel_nativeStart"));
  fn_env_obj process = (fn_env_obj)entry(ENTRY("wrapper_kernel_CWrapperKernel_nativeProcess"));

  /* The order and the arities below are the dex's, not guesses
   * (tools/dex_class.py reads the bytecode; docs/native_signatures.txt has the
   * prototypes). The Java side is:
   *
   *   CWrapper.<clinit>                 -> CWrapper.nativeSetClass()
   *   CWrapper.<init>()                 -> mUseNativeCallback = true;
   *                                        onCreate(getClass().getName(), true)
   *   CWrapper.onCreate(String, Z)      -> nativeInitialize(mModuleName)
   *   CWrapperData.<init>()             -> nativeInitialize(TimeZone.getRawOffset())
   *   CWrapperKernel$1$1.run() (GL)     -> if state == INITIAL  setState(NATIVE_START)
   *                                          -> nativeStart()
   *                                        else nativeSurfaceRecreated()
   *
   * nativeInitialize is an *instance* method whose argument is the module name,
   * and it dispatches to that module's native init -- the one that caches the
   * jclass and the jmethodIDs the engine calls back through. Skipping it is why
   * the third run died inside jgetMediaPath with a NULL jclass: it does
   * GetStaticMethodID(CFunction, "getPackageName", "()Ljava/lang/String;") and
   * strcpy()s the result.
   *
   * nativeStart comes *before* nativeSurfaceRecreated: the GL thread runs
   * nativeStart while the kernel state is still INITIAL, and every later
   * surface event is the other one. */

  if (set_class) {
    debugPrintf("[game] -> nativeSetClass\n");
    log_flush_ring();
    set_class(g_jni_env, NULL);
    debugPrintf("[game] <- nativeSetClass\n");
    log_flush_ring();
  } else {
    debugPrintf("[game] nativeSetClass MISSING\n");
  }

  if (init_module) {
    for (int i = 0; k_modules[i]; i++) {
      char cls[96];
      snprintf(cls, sizeof cls, "%s", k_modules[i]);
      for (char *p = cls; *p; p++)
        if (*p == '.')
          *p = '/';
      JObj *thiz = jni_new(cls);
      JObj *name = jni_str(k_modules[i]);
      debugPrintf("[game] -> nativeInitialize(%s)\n", k_modules[i]);
      log_flush_ring();
      init_module(g_jni_env, thiz, name);
      debugPrintf("[game] <- nativeInitialize(%s)\n", k_modules[i]);
      log_flush_ring();
    }
  } else {
    debugPrintf("[game] CWrapper.nativeInitialize MISSING\n");
  }

  if (data_init) {
    /* CWrapperData.<init> passes TimeZone.getRawOffset(): milliseconds. */
    const int tz_ms = 8 * 3600 * 1000;
    debugPrintf("[game] -> CWrapperData.nativeInitialize(%d)\n", tz_ms);
    log_flush_ring();
    data_init(g_jni_env, NULL, tz_ms);
    debugPrintf("[game] <- CWrapperData.nativeInitialize\n");
    log_flush_ring();
  }

  if (set_size) {
    debugPrintf("[game] -> nativeSetScreenSize(%d,%d x3)\n", w, h);
    log_flush_ring();
    set_size(g_jni_env, NULL, w, h, w, h, w, h);
    debugPrintf("[game] <- nativeSetScreenSize\n");
    log_flush_ring();
  }

  call0((fn_env_obj)entry(ENTRY("wrapper_common_CCommonAPI_nativeInitialize")), NULL,
        "CCommonAPI.nativeInitialize");

  /* The engine's main entry. On Android this is where MainActivity hands the
   * thread over; if it returns, the game is driven by nativeProcess from the
   * GL thread instead. */
  if (!start || !process) {
    debugPrintf("[game] nativeStart/nativeProcess MISSING: the engine cannot run\n");
    log_flush_ring();
    return;
  }

  debugPrintf("[game] -> nativeStart\n");
  log_flush_ring();
  start(g_jni_env, NULL);
  debugPrintf("[game] <- nativeStart\n");
  log_flush_ring();

  if (surface) {
    debugPrintf("[game] -> nativeSurfaceRecreated\n");
    log_flush_ring();
    surface(g_jni_env, NULL);
    debugPrintf("[game] <- nativeSurfaceRecreated\n");
    log_flush_ring();
  }

  inotia_input_init();
  inotia_audio_init();

  debugPrintf("[game] frame loop\n");
  log_flush_ring();

  while (!rt_exit_requested() && appletMainLoop()) {
    rt_applet_poll();
    if (!rt_focused()) {
      svcSleepThread(50000000ll);
      continue;
    }

    dcr_boost_frame_begin();
    inotia_input_frame();
    process(g_jni_env, NULL);
    eglSwapBuffers(g_dpy, g_surf);
    dcr_boost_frame_end(g_frames);

    if (g_frames == 0) {
      dcr_launch_ready();
      debugPrintf("[game] first frame presented\n");
      log_flush_ring();
    }
    g_frames++;
  }

  debugPrintf("[game] leaving after %llu frames\n", (unsigned long long)g_frames);
  inotia_audio_shutdown();
  log_flush_ring();
}

/* The watchdog reads this to tell a hang from a slow load. */
uint64_t dcr_boot_frames(void) { return g_frames; }

const char *port_apk_help(void) {
  return "Put your own copy of the Inotia 4 1.3.7 APK (armeabi-v7a) in the game folder.\n"
         "The port reads libgame.so and the assets out of it; it ships none of them.";
}
