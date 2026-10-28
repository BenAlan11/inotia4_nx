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
/* The engine builds /sdcard/Android/data/<package>/files/resources/... from
 * getPackageName(), and dcr_path maps that to <root>/external/files/... --
 * "external" is the runtime's name for shared storage, not "Android". Writing
 * anywhere else leaves the engine looking at an empty folder. */
#define RES_ROOT "external/files/resources"

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
  /* The key carries the layout: builds before this one unpacked into
   * Android/data/..., which dcr_path never looks at, so those copies have to
   * be redone rather than trusted. */
  if (rt_setup_stamp_get("assets-external", &crc, &size) &&
      crc == (unsigned long)dcr_manifest_version_code()) {
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
  rt_setup_stamp_set("assets-external", (unsigned long)dcr_manifest_version_code(), (unsigned long)done);
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
typedef int (*fn_env_obj_obj_2i)(void *env, void *obj, void *a, void *b, int c, int d);

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

static void busy_begin(const char *what);
static void busy_end(void);

static void call0(fn_env_obj f, void *obj, const char *what) {
  if (!f) {
    debugPrintf("[game] %s: MISSING\n", what);
    return;
  }
  debugPrintf("[game] -> %s\n", what);
  log_flush_ring();
  busy_begin(what);
  const int rc = f(g_jni_env, obj);
  busy_end();
  debugPrintf("[game] <- %s = %d\n", what, rc);
  log_flush_ring();
}

/* ------------------------------------------------------------- heartbeat */
/* A log that stops in the middle of a call cannot tell a hang from a slow
 * load, and the engine's own logging goes to a RAM ring that is only written
 * out when someone flushes it -- so the last line in debug.log is the last
 * flushed one, not the last executed one. While a call is on the stack this
 * prints once a second and flushes, which makes both questions answerable. */
static volatile int g_busy;
static volatile const char *g_busy_what = "?";
static volatile uint64_t g_busy_since; /* ns */

static void heartbeat_thread(void *arg) {
  (void)arg;
  for (;;) {
    svcSleepThread(1000000000ll);
    if (!g_busy)
      continue;
    const uint64_t s = (armTicksToNs(armGetSystemTick()) - g_busy_since) / 1000000000ull;
    debugPrintf("[game] still inside %s: %llu s\n", (const char *)g_busy_what,
                (unsigned long long)s);
    log_flush_ring();
  }
}

static void heartbeat_start(void) {
  static Thread th;
  const Result rc = threadCreate(&th, heartbeat_thread, NULL, NULL, 0x4000, 0x2C, -2);
  if (R_FAILED(rc)) {
    debugPrintf("[game] no heartbeat thread (0x%x)\n", rc);
    return;
  }
  if (R_FAILED(threadStart(&th))) {
    threadClose(&th);
    debugPrintf("[game] no heartbeat thread (start)\n");
  }
}

/* Every call into the engine is bracketed with these. A call that takes more
 * than a second says so on the way out, which is how a slow load and a hang
 * get told apart without another run. */
static void busy_begin(const char *what) {
  g_busy_what = what;
  g_busy_since = armTicksToNs(armGetSystemTick());
  g_busy = 1;
}

static void busy_end(void) {
  const uint64_t ms = (armTicksToNs(armGetSystemTick()) - g_busy_since) / 1000000ull;
  g_busy = 0;
  if (ms >= 1000)
    debugPrintf("[game] %s took %llu ms\n", (const char *)g_busy_what, (unsigned long long)ms);
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
  heartbeat_start(); /* from here on, every engine call is bracketed */
  busy_begin("constructors");
  so_execute_init_array(&g_game);
  busy_end();

  jni_init();
  debugPrintf("[game] JNIEnv %p JavaVM %p\n", g_jni_env, g_jni_vm);

  /* The engine's own JNI_OnLoad registers its natives and caches the
   * jmethodIDs it will call back through. */
  fn_env_obj onload = (fn_env_obj)entry("JNI_OnLoad");
  if (onload) {
    busy_begin("JNI_OnLoad");
    const int rc = onload(g_jni_vm, NULL);
    busy_end();
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
    busy_begin("nativeSetClass");
    set_class(g_jni_env, NULL);
    busy_end();
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
      busy_begin(k_modules[i]);
      init_module(g_jni_env, thiz, name);
      busy_end();
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
    busy_begin("CWrapperData.nativeInitialize");
    data_init(g_jni_env, NULL, tz_ms);
    busy_end();
    debugPrintf("[game] <- CWrapperData.nativeInitialize\n");
    log_flush_ring();
  }

  /* CEventHandler.<init> allocates two direct buffers and hands them over:
   * 16 bytes of manage data and 2048 of event queue (256 events x 8 bytes),
   * with (mEventMax, mSizeForEvent) as the last two arguments. jProcessEvent,
   * the first thing nativeProcess does, reads through them. */
  {
    static unsigned char manage[16];
    static unsigned char queue[2048];
    fn_env_obj_obj_2i ev_init =
        (fn_env_obj_obj_2i)entry(ENTRY("wrapper_kernel_CEventHandler_nativeEventHandlerInitialize"));
    if (ev_init) {
      debugPrintf("[game] -> nativeEventHandlerInitialize(16, 2048, 256, 8)\n");
      log_flush_ring();
      busy_begin("nativeEventHandlerInitialize");
      ev_init(g_jni_env, NULL, inotia_direct_buffer(manage, sizeof manage),
              inotia_direct_buffer(queue, sizeof queue), 256, 8);
      busy_end();
      debugPrintf("[game] <- nativeEventHandlerInitialize\n");
      log_flush_ring();
    } else {
      debugPrintf("[game] nativeEventHandlerInitialize MISSING\n");
    }
  }

  if (set_size) {
    debugPrintf("[game] -> nativeSetScreenSize(%d,%d x3)\n", w, h);
    log_flush_ring();
    busy_begin("nativeSetScreenSize");
    set_size(g_jni_env, NULL, w, h, w, h, w, h);
    busy_end();
    debugPrintf("[game] <- nativeSetScreenSize\n");
    log_flush_ring();
  }

  call0((fn_env_obj)entry(ENTRY("wrapper_common_CCommonAPI_nativeInitialize")), NULL,
        "CCommonAPI.nativeInitialize");

  /* Where the engine will look for its resources: /sdcard/Android/data/<pkg>/
   * files/resources/common through dcr_path. Say whether it is there, so a
   * black screen with ENOENT in the log is one line to explain. */
  {
    char probe[DCR_PATH_MAX];
    struct stat st;
    snprintf(probe, sizeof probe, "%s/%s/common", dcr_game_root(), RES_ROOT);
    debugPrintf("[setup] resources: %s -> %s\n", probe,
                stat(probe, &st) == 0 ? "present" : "MISSING");
    log_flush_ring();
  }

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
  busy_begin("nativeStart");
  start(g_jni_env, NULL);
  busy_end();
  debugPrintf("[game] <- nativeStart\n");
  log_flush_ring();

  /* startClet died with no message at all: the log is written through, so it
   * is not a lost line -- the process went away. The first thing it does is a
   * straight line of engine calls, so the port runs the argument-free ones
   * itself first, one log line each. Where the log stops names the culprit;
   * if all five come back, the fault is further down the chain
   * (OriginalDisplay_ConfigInfo / SGL_grpInitialize / MEM_Create / ...). */
  {
    typedef int (*fn_i)(int);
    typedef int (*fn_v)(void);
    typedef void (*fn_ii)(int, int);
    fn_i set_logged = (fn_i)entry("CS_ActiveUserSetLogged");
    fn_i set_orient = (fn_i)entry("CS_SetModuleOrientation");
    fn_v get_w = (fn_v)entry("CS_knlGetRealScreenWidth");
    fn_v get_h = (fn_v)entry("CS_knlGetRealScreenHeight");
    fn_ii set_original = (fn_ii)entry("jSetOriginalSize");

    int rw = w, rh = h;
    if (set_logged) {
      debugPrintf("[game] -> CS_ActiveUserSetLogged(0)\n");
      log_flush_ring();
      busy_begin("CS_ActiveUserSetLogged");
      const int rc = set_logged(0);
      busy_end();
      debugPrintf("[game] <- CS_ActiveUserSetLogged = %d\n", rc);
      log_flush_ring();
    }
    if (set_orient) {
      debugPrintf("[game] -> CS_SetModuleOrientation(2)\n");
      log_flush_ring();
      busy_begin("CS_SetModuleOrientation");
      const int rc = set_orient(2);
      busy_end();
      debugPrintf("[game] <- CS_SetModuleOrientation = %d\n", rc);
      log_flush_ring();
    }
    if (get_w && get_h) {
      rw = get_w();
      rh = get_h();
      debugPrintf("[game] CS_knlGetRealScreen: %dx%d\n", rw, rh);
      log_flush_ring();
    }
    if (set_original && rw > 0 && rh > 0) {
      debugPrintf("[game] -> jSetOriginalSize(%d,%d)\n", rw, rh);
      log_flush_ring();
      busy_begin("jSetOriginalSize");
      set_original(rw, rh);
      busy_end();
      debugPrintf("[game] <- jSetOriginalSize\n");
      log_flush_ring();
    }
  }

  /* nativeStartClet is Protection checks + startClet():
   *
   *   Protection::getInstance()->startCheckSpeedHack()   (a `return 1` stub)
   *   if (isRunningMemoryHack()) jShowDialogAndExit()
   *   if (jWD_GetIsTerminateIfCracked() && isRooted()) jShowDialogAndExit()
   *   startClet()                                        <- the real work
   *
   * isRunningMemoryHack is Android-only: it builds a path out of
   * CFunction.getUserDir() + getPackageName(), runs system(...) (a -1 stub
   * here) and then fopen()s a /proc-ish path. On this port the run died
   * inside it, right after getUserDir, with no message -- a silent exit, not a
   * fault. There is no Android to check and nothing to detect, so the port
   * calls startClet() itself and skips the scan.
   *
   * startClet is a straight line of engine initialisers (CS_ActiveUserSetLogged,
   * jSetOriginalSize, SGL_grpInitialize, MEM_Create, UTIL_GetUDID,
   * APPINFO_Initialize, IMGSYS_LoadTitleImg, CS_knlDefTimer, ...) and ends by
   * arming the engine's 100 ms timer. */
  {
    void (*start_clet)(void) = (void (*)(void))entry("startClet");
    if (start_clet) {
      debugPrintf("[game] -> startClet (Protection's memory-hack scan skipped)\n");
      log_flush_ring();
      busy_begin("startClet");
      start_clet();
      busy_end();
      debugPrintf("[game] <- startClet\n");
      log_flush_ring();
    } else {
      debugPrintf("[game] startClet MISSING\n");
    }
  }

  if (surface) {
    debugPrintf("[game] -> nativeSurfaceRecreated\n");
    log_flush_ring();
    busy_begin("nativeSurfaceRecreated");
    surface(g_jni_env, NULL);
    busy_end();
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

    /* The first frame is the one that tells a hang from a slow load: the
     * engine parses its resource tables here and can sit for a while. Each
     * side of the call is logged once so a log that stops mid-frame says so,
     * and the heartbeat thread keeps counting while it runs. */
    if (g_frames == 0) {
      debugPrintf("[game] -> nativeProcess (first frame)\n");
      log_flush_ring();
    }
    busy_begin("nativeProcess");
    process(g_jni_env, NULL);
    g_busy = 0; /* not busy_end(): a slow frame every frame would flood the log */
    if (g_frames == 0) {
      debugPrintf("[game] <- nativeProcess (first frame, %llu ms)\n",
                  (unsigned long long)((armTicksToNs(armGetSystemTick()) - g_busy_since) / 1000000ull));
      log_flush_ring();
    }

    if (!eglSwapBuffers(g_dpy, g_surf)) {
      debugPrintf("[game] eglSwapBuffers failed at frame %llu: 0x%x\n",
                  (unsigned long long)g_frames, eglGetError());
      log_flush_ring();
      break;
    }
    dcr_boost_frame_end(g_frames);

    if (g_frames == 0) {
      dcr_launch_ready();
      debugPrintf("[game] first frame presented\n");
      log_flush_ring();
    }
    g_frames++;

    if (g_frames == 2 || g_frames == 10 || !(g_frames % 60)) {
      debugPrintf("[game] frame %llu\n", (unsigned long long)g_frames);
      log_flush_ring();
    }
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
