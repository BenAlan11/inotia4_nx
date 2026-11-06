/* jni_fake.c -- the Java shell, reimplemented in C
 *
 * libgame.so never touches the filesystem or the audio device by itself: the
 * APK's Java layer does that and the engine calls into it through JNI. On the
 * Switch there is no Dalvik, so this file plays the part of the shell.
 *
 * Dispatch is driven by the JNI signature the engine passes to GetMethodID:
 * the argument list is decoded from the descriptor, which means one generic
 * reader serves every method and adding a handler cannot desynchronise the
 * varargs.
 *
 * Every method the engine reaches is logged once (name + signature + return
 * value) so debug.log doubles as a trace of what the shell is being asked for.
 */

#include <switch.h>

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "jni_fake.h"
#include "libc_shim.h"
#include "config.h"
#include "util.h"
#include "audio.h"
#include "text.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006
#define JNI_ABORT 2

typedef uint8_t  jboolean;
typedef int32_t  jint;
typedef int64_t  jlong;
typedef float    jfloat;
typedef double   jdouble;

/* ------------------------------------------------------------------------- */
/* fake object model                                                         */
/* ------------------------------------------------------------------------- */

enum {
  TAG_OBJECT = 0x4f424a31,  /* 'OBJ1' */
  TAG_STRING = 0x53545231,  /* 'STR1' */
  TAG_ID     = 0x4d494431,  /* 'MID1' */
  TAG_ARRAY  = 0x41525231,  /* 'ARR1' */
};

typedef struct {
  uint32_t tag;
  char label[80];
  intptr_t ivalue;      /* FileDescriptor.descriptor, View ids, ... */
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;
  int len;
} FakeString;

typedef struct {
  uint32_t tag;
  char name[64];
  char sig[96];
} FakeID;

typedef struct {
  uint32_t tag;
  int elem_size;
  int len;
  void *data;
} FakeArray;

static unsigned long g_call_count, g_obj_count;

/* Java's GLSurfaceView runs in RENDERMODE_WHEN_DIRTY: the engine calls
 * requestRender() when it has a finished frame, and only then is the back buffer
 * presented. Swapping on every loop iteration instead made the screen flicker
 * between the drawn buffer and the untouched one. */
static int g_render_requests;
int jni_take_render_request(void) {
  const int n = g_render_requests;
  g_render_requests = 0;
  return n;
}

/* set when the engine asks for config/HUB_LOGIN_INFO -- the step right before
 * it puts up the "log in to Hive?" prompt, whose own text we cannot read */
static int g_hub_probe;

int jni_take_hub_probe(void) {
  const int v = g_hub_probe;
  g_hub_probe = 0;
  return v;
}

void jni_note_hub(void) { g_hub_probe = 1; }
unsigned long jni_call_count(void) { return g_call_count; }
unsigned long jni_obj_count(void) { return g_obj_count; }

void *jni_make_object(const char *label) {
  g_obj_count++;
  FakeObject *o = calloc(1, sizeof(*o));
  if (!o) return NULL;
  o->tag = TAG_OBJECT;
  snprintf(o->label, sizeof(o->label), "%s", label ? label : "object");
  return o;
}

void *jni_make_fd(int fd) {
  FakeObject *o = jni_make_object("java/io/FileDescriptor");
  if (o) o->ivalue = fd;
  return o;
}

void *jni_make_string(const char *utf) {
  g_obj_count++;
  FakeString *s = calloc(1, sizeof(*s));
  if (!s) return NULL;
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  s->len = (int)strlen(s->utf);
  return s;
}

void *jni_make_byte_array(const void *data, int len) {
  g_obj_count++;
  FakeArray *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->tag = TAG_ARRAY;
  a->elem_size = 1;
  a->len = len;
  a->data = calloc(1, len > 0 ? len : 1);
  if (data && len > 0) memcpy(a->data, data, len);
  return a;
}

static int is_tag(const void *p, uint32_t tag) {
  return p && ((const FakeObject *)p)->tag == tag;
}

static const char *str_of(void *p) {
  return is_tag(p, TAG_STRING) ? ((FakeString *)p)->utf : "";
}

static int fd_of(void *p) {
  if (!is_tag(p, TAG_OBJECT)) return -1;
  return (int)((FakeObject *)p)->ivalue;
}

/* ------------------------------------------------------------------------- */
/* method / field id pool                                                    */
/* ------------------------------------------------------------------------- */

/* A busy engine asks for a few hundred method/field IDs during start-up. Running
 * out used to hand back id_pool[0], i.e. "any method you like is the first one
 * you asked for" -- the caller then uses a String return as an object and dies
 * somewhere unrelated. Be generous, and say so if it ever runs dry. */
#define MAX_IDS 4096
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *name, const char *sig, int is_field) {
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    static int warned;
    if (!warned++) {
      debugPrintf("JNI: id pool full (%d), reusing slot 0 -- this will misfire\n",
                  MAX_IDS);
    }
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  snprintf(id->name, sizeof(id->name), "%s", name ? name : "");
  snprintf(id->sig, sizeof(id->sig), "%s", is_field ? "(F)" : (sig ? sig : ""));
  return id;
}

/* The engine caches method ids in its own globals and, on the paths that only
 * run after a screen transition, occasionally calls one that was never resolved
 * -- so the id arrives as NULL. Dereferencing it dies at address 0x44 (id->sig)
 * inside read_jvalues, which is a data abort with no clue attached. Treat any
 * id that is not one of ours as "unknown method" and carry on: a no-op call is
 * far better than a crash, and the log line names the caller so the real
 * culprit can be found later. */
static FakeID id_unknown = { TAG_ID, "unknown", "()V" };

static FakeID *id_safe(FakeID *id, const char *fn, void *caller) {
  uintptr_t p = (uintptr_t)id;
  if (p >= 0x10000 && p < 0x800000000000ULL && ((FakeID *)p)->tag == TAG_ID)
    return id;
  static int warned;
  if (warned < 24) {
    warned++;
    debugPrintf("JNI: %s got a bogus method id %p (caller %p)\n", fn, (void *)id,
                caller);
  }
  return &id_unknown;
}

#define ID_SAFE(id) (id) = id_safe((id), __func__, __builtin_return_address(0))

/* ------------------------------------------------------------------------- */
/* signature-driven varargs reader                                           */
/* ------------------------------------------------------------------------- */

typedef struct {
  char kind;        /* S string, O object, I int, J long, F double, Z bool, A array */
  const char *s;
  void *o;
  int64_t i;
  double f;
} JVal;

static int read_args(const char *sig, va_list va, JVal *out, int maxn) {
  int n = 0;
  const char *p = strchr(sig, '(');
  if (!p) return 0;
  p++;
  while (*p && *p != ')' && n < maxn) {
    JVal *v = &out[n];
    memset(v, 0, sizeof(*v));
    switch (*p) {
      case 'L': case '[': {
        const char *end = strchr(p, ';');
        if (!end) { p++; v->kind = 'O'; break; }
        const int is_array = (*p == '[');
        v->kind = is_array ? 'A' : 'O';
        v->o = va_arg(va, void *);
        if (!is_array) {
          /* strings arrive as jobject too; detect by tag */
          if (is_tag(v->o, TAG_STRING)) { v->kind = 'S'; v->s = str_of(v->o); }
        }
        p = end + 1;
        n++;
        continue;
      }
      case 'I': v->kind = 'I'; v->i = va_arg(va, jint); break;
      case 'Z': v->kind = 'Z'; v->i = va_arg(va, int); break;
      case 'B': v->kind = 'I'; v->i = va_arg(va, int); break;
      case 'S': v->kind = 'I'; v->i = va_arg(va, int); break;
      case 'C': v->kind = 'I'; v->i = va_arg(va, int); break;
      case 'J': v->kind = 'J'; v->i = va_arg(va, jlong); break;
      case 'F': v->kind = 'F'; v->f = va_arg(va, double); break;
      case 'D': v->kind = 'F'; v->f = va_arg(va, double); break;
      default:  v->kind = 'I'; v->i = va_arg(va, jint); break;
    }
    p++;
    n++;
  }
  return n;
}

static const char *ret_kind(const char *sig) {
  const char *p = strrchr(sig, ')');
  return p ? p + 1 : "V";
}

/* log the first time a given (name, signature) pair shows up */
/* set by the Call*Method wrappers, where return_address(0) is the engine's */
static void *g_jni_caller;
/* one frame further up: the engine always reaches us through its
 * _JNIEnv::Call*Method helper, so RA(0) is that helper and RA(1) is the code
 * that actually decided to make the call */
static void *g_jni_caller2;

/* log_once() dedupes, so the last line of a log is not necessarily where the
 * process died. Keep the most recent method name and let the heartbeat print it:
 * the last beat then names the call that was in flight. */
/* native callback the Java shell delivered once a request finished; without it
 * the engine polls netHttpConnectEx forever and the screen stays black */
static void (*cb_http_connect)(int handle, int result);
void jni_set_http_hook(void (*fn)(int, int)) { cb_http_connect = fn; }

static char g_last_method[96];
static int g_trace_calls;      /* log this many upcoming calls verbatim */
void jni_trace(int n) { g_trace_calls = n; }
const char *jni_last_method(void) { return g_last_method[0] ? g_last_method : "-"; }

static void log_once(const char *name, const char *sig) {
  static char seen[64][100];
  static int n = 0;
  char key[100];
  snprintf(key, sizeof(key), "%s%.40s", name, sig);
  for (int i = 0; i < n; i++)
    if (!strcmp(seen[i], key))
      return;
  if (n < 64) snprintf(seen[n++], sizeof(seen[n]), "%s", key);
  /* the caller's address tells us which engine function wants this method,
   * which is what turns a garbled name into a location */
  debugPrintf("JNI: %s%s  <- %p (via %p)\n", name, sig, g_jni_caller,
              g_jni_caller2);
}

/* ------------------------------------------------------------------------- */
/* Android services the engine actually depends on                           */
/* ------------------------------------------------------------------------- */

static JniSoundPlayFn  cb_sound_play = NULL;
static JniSoundStopFn  cb_sound_stop = NULL;
static JniSoundVolumeFn cb_sound_volume = NULL;

void jni_set_audio_hooks(JniSoundPlayFn play, JniSoundStopFn stop,
                         JniSoundVolumeFn volume) {
  cb_sound_play = play;
  cb_sound_stop = stop;
  cb_sound_volume = volume;
}

/* Assets are addressed by fd: the engine opens the file here and then wraps the
 * descriptor in a java.io.FileDescriptor it reads back through GetIntField. */
static void *open_asset(const char *name, const int *sizes, int nsizes) {
  char buf[512];
  char full[600];
  snprintf(full, sizeof(full), "%s/%s", g_data_dir, name ? name : "");
  const char *p = shim_path(full, buf, sizeof(buf));
  if (!shim_path_resolve(p, buf, sizeof(buf)))
    snprintf(buf, sizeof(buf), "%s", p);

  int fd = open(buf, O_RDONLY);
  if (fd < 0) {
    debugPrintf("JNI: asset MISS %s\n", buf);
    return NULL;
  }
  struct stat st;
  if (fstat(fd, &st) == 0 && sizes && nsizes > 0) {
    /* the caller passes an int[] it fills with width/height/size hints; we only
     * know the file length, which is what the engine uses for texture blobs */
    (void)sizes;
  }
  return jni_make_fd(fd);
}

/* device / account questions: answer like a plain offline Android device */
static const char *device_string(const char *what) {
  if (!strcmp(what, "model")) return "Nintendo Switch";
  if (!strcmp(what, "package")) return shim_package_name();
  if (!strcmp(what, "country")) return "US";
  if (!strcmp(what, "language")) return "en";
  if (!strcmp(what, "version")) return "4.4.2";
  if (!strcmp(what, "id")) return "0123456789abcdef";
  if (!strcmp(what, "mac")) return "00:00:00:00:00:00";
  if (!strcmp(what, "userdir")) return g_save_dir;
  return "";
}

/* The "...A" variants receive their arguments as a jvalue array instead of a
 * varargs list. Mapping them onto the varargs implementation made it va_start()
 * a list that does not exist, so it read whatever the caller had in registers --
 * which is a crash a few instructions after the method id was resolved. */
typedef union {
  uint8_t z; int8_t b; uint16_t c; int16_t s; int32_t i; int64_t j;
  float f; double d; void *l;
} JValueArg;

static int read_jvalues(const char *sig, const JValueArg *args, JVal *out, int maxn) {
  int n = 0;
  if (!args)
    return 0;
  const char *p = sig ? sig : "()V";
  if (*p == '(') p++;
  while (*p && *p != ')' && n < maxn) {
    switch (*p) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        out[n].kind = 'I'; out[n].i = args[n].i; break;
      case 'J':
        out[n].kind = 'J'; out[n].i = args[n].j; break;
      case 'F': case 'D':
        out[n].kind = 'F'; out[n].f = args[n].d; break;
      case 'L': case '[':
        out[n].kind = 'O'; out[n].o = args[n].l; break;
      default:
        out[n].kind = 'I'; out[n].i = 0; break;
    }
    while (*p == '[') p++;                 /* arrays: element type follows */
    if (*p == 'L') { while (*p && *p != ';') p++; }
    p++;
    n++;
  }
  return n;
}

/* ------------------------------------------------------------------------- */
/* dispatch                                                                  */
/* ------------------------------------------------------------------------- */

static int64_t dispatch_int(const char *name, const char *sig, JVal *a, int n) {
  (void)sig;

  if (!strcmp(name, "getStringWidth") || !strcmp(name, "measureText"))
    return n > 0 ? text_measure(a[0].kind == 'S' ? a[0].s
                                                : (a[0].kind == 'A' ? (const char *)((FakeArray *)a[0].o)->data : "")) : 0;

  /* the engine sizes its resource/texture caches from these two, so they have
   * to be plausible; an unhandled name used to fall through to a pointer-sized
   * value and the engine then asked for an absurd allocation */
  if (!strcmp(name, "getNativeAvailableMemory")) return 320 * 1024 * 1024;
  if (!strcmp(name, "getMaxMemory") || !strcmp(name, "getTotalMemory") ||
      !strcmp(name, "maxMemory")) {
    debugPrintf("JNI: %s -> 512 MB\n", name);
    return (int64_t)512 * 1024 * 1024;
  }
  if (!strcmp(name, "getVolume") || !strcmp(name, "soundGetVolume"))
    return (int)(audio_sfx_volume() * 100.0f);
  if (!strcmp(name, "getBGMVolume")) return (int)(audio_bgm_volume() * 100.0f);
  if (!strcmp(name, "soundPlay") || !strcmp(name, "playSound")) {
    /* Which UI handler ran is otherwise invisible: the skill page's two
     * buttons are told apart by the id they play (learn 0x12, cancel-shortcut
     * 2). Limited, so a burst of taps cannot flood the log. */
    static int sfx_logs;
    if (sfx_logs < 40 && n >= 1) {
      sfx_logs++;
      debugPrintf("sfx: play %d\n", (int)a[0].i);
    }
    if (n >= 1 && cb_sound_play)
      cb_sound_play((int)a[0].i, n >= 2 ? (float)a[1].f : 1.0f, 0);
    return 0;
  }
  if (!strcmp(name, "soundLoad") || !strcmp(name, "createSound"))
    return audio_sfx_load(a[0].kind == 'S' ? a[0].s : "");
  /* The engine's HTTP layer is a Java-side async request: it asks for the body
   * length once nativeConnectCB has been delivered. We answer the callback
   * ourselves (see the connect handler below) and report an empty body, which
   * lets the caller finish instead of polling for a connection that no Java
   * thread will ever complete. */
  if (!strcmp(name, "netHttpGetResponseBodyLength") ||
      !strcmp(name, "netHttpGetLengthEx"))
    return 0;

  if (!strcmp(name, "netHttpConnectEx")) {
    const int handle = n > 0 ? (int)a[0].i : 0;
    if (cb_http_connect) cb_http_connect(handle, 0);
    return 1;                      /* non-zero == the poll is satisfied */
  }
  /* Refusing to open a request is how "no Hive, no cloud" is expressed to the
   * engine: every call fails on the spot instead of sitting on a connection
   * that will never complete, so its Hive/notice code takes the offline path
   * and the save list comes from the local saveN.dat files. */
  if (!strcmp(name, "netHttpOpenEx")) {
    /* The anti-tamper check (s.com2us.net/common/protect/check.c2s) is not a
     * Hive request: when it fails the engine raises a "连接出错。请稍后再试"
     * box and stops loading the profile -- empty inventory, zero stats, no
     * save data. Answering it with an empty 200 is enough for it to move on. */
    const char *url = "";
    if (n > 0) {
      if (a[0].kind == 'S')
        url = a[0].s ? a[0].s : "";
      else if (a[0].kind == 'O')
        url = str_of(a[0].o);
    }
    /* The URL arrives as a Java string object that str_of() cannot read back
     * (kind 'O', empty text), so the request cannot be identified by its
     * address. The anti-tamper check is the very first request the engine
     * makes at boot, and it is the only one whose failure raises the
     * "连接出错" box, so that call is the one that gets an empty 200. */
    static int http_opens;
    const int first = http_opens++;
    debugPrintf("net: open #%d kind=%c url='%s'\n", first,
                n > 0 ? a[0].kind : '?', url);
    if (first < 2)
      return 1;
    return -1;
  }
  /* CloseEx is polled every frame until it reports the request is gone: leaving
   * it at 0 is what kept the engine in the "closing" state forever. */
  if (!strcmp(name, "netHttpIsContains") ||
      !strcmp(name, "netHttpGetStatusEx") || !strcmp(name, "netHttpIsFinished") ||
      !strcmp(name, "netHttpCloseEx"))
    return 1;

  if (!strcmp(name, "netHttpGetResponseCode"))
    return 200;
  if (!strcmp(name, "getPhoneNumber") || !strcmp(name, "getRoaming") ||
      !strcmp(name, "getSubscriptionProductInfo") || !strcmp(name, "coinsReward") ||
      !strcmp(name, "mercuryGetBadge") || !strcmp(name, "getPackageName2"))
    return 0;
  if (!strcmp(name, "getPackageName"))
    return 0;
  return 0;
}

static void dispatch_void(const char *name, const char *sig, JVal *a, int n) {
  (void)sig;

  /* ---- Hive IAP (InAppV4) -------------------------------------------------
   * The engine's CS_IapV4* layer calls methods on the Java InAppV4 object, and
   * the object hands results back by calling the native callbacks it was given.
   * marketConnect(J) receives InAppV4MarketListCB as that J argument; the real
   * Java connects to Google/OneStore and then calls it with the market list.
   * Here the connection is instantaneous and the list is a single entry, which
   * is what makes the store think a market is available. */
  if (!strcmp(name, "marketConnect")) {
    typedef void (*market_cb)(long result, const char *list, int len,
                              const void *extra);
    const long cb_addr = n > 0 ? (long)a[0].i : 0;
    static const int market_code = 3;
    debugPrintf("iap: marketConnect(cb %lx)\n", (unsigned long)cb_addr);
    if (cb_addr)
      ((market_cb)(uintptr_t)cb_addr)(0, "google", 6, &market_code);
    return;
  }

  if (!strcmp(name, "drawText") || !strcmp(name, "drawTextInRect")) {
    /* handled below in the text block */
  }

  if (!strcmp(name, "soundStop") || !strcmp(name, "soundDestroy") ||
      !strcmp(name, "stopCurrent") || !strcmp(name, "stopAll")) {
    if (cb_sound_stop && n >= 1) cb_sound_stop((int)a[0].i);
    else if (cb_sound_stop) cb_sound_stop(-1);
    return;
  }
  if (!strcmp(name, "soundPause") || !strcmp(name, "soundResume")) return;
  if (!strcmp(name, "setVolume") || !strcmp(name, "soundSetVolume")) {
    if (n >= 2 && cb_sound_volume) cb_sound_volume((int)a[0].i, (float)a[1].f);
    return;
  }
  if (!strcmp(name, "setBGMVolume")) {
    audio_set_bgm_volume(n >= 1 ? (float)a[0].f : 1.0f);
    return;
  }
  if (!strcmp(name, "startVibrate") || !strcmp(name, "cancelVibrate")) return;
  if (!strcmp(name, "closeFileDescriptorAll")) return;
  if (!strcmp(name, "setDataFromNative") || !strcmp(name, "setAppProperty") ||
      !strcmp(name, "setPropertyAutoCapitalizationType") ||
      !strcmp(name, "setPropertyKeyboardType") ||
      !strcmp(name, "setPropertyKeyboardAlwaysShow") ||
      !strcmp(name, "setPropertyText") || !strcmp(name, "setPropertyTextType") ||
      !strcmp(name, "setPropertyTextAlignmentVertical") ||
      !strcmp(name, "setPropertyTextAlignmentHorizontal") ||
      !strcmp(name, "setPropertyBackColor") || !strcmp(name, "setPropertySecure") ||
      !strcmp(name, "setPropertyFrame") || !strcmp(name, "setDisplay") ||
      !strcmp(name, "requestReturnEventForcedly"))
    return;
  if (!strcmp(name, "requestRender")) {
    g_render_requests++;
    /* how fast the engine thinks time passes: a splash that takes 15 frames
     * should take a quarter of a second, not fifteen seconds */
    {
      static u64 last;
      static int n;
      const u64 now = armGetSystemTick();
      if (n < 14) {
        debugPrintf("JNI: requestRender #%d, %llu ms since the last one\n", n,
                    last ? (unsigned long long)((now - last) * 1000 /
                                                armGetSystemTickFreq())
                         : 0ull);
        n++;
      }
      last = now;
    }
    return;
  }
  /* store / account flows: the game must not block on them */
  if (!strcmp(name, "showCharge") || !strcmp(name, "showMarketSelection") ||
      !strcmp(name, "showInAppMessages") || !strcmp(name, "showUserAgreeTerms") ||
      !strcmp(name, "showReviewPopup") || !strcmp(name, "showDialog") ||
      !strcmp(name, "showDialogForChatbot") || !strcmp(name, "alertPopup") ||
      !strcmp(name, "setJNIActiveUserNotifier") || !strcmp(name, "setPGSAutoLoginWithCB") ||
      !strcmp(name, "setAuthDialogClear") || !strcmp(name, "setJNIUserAgreeNotifier") ||
      !strcmp(name, "setIsShowPermissionView") || !strcmp(name, "setEmergencyModeWithUid") ||
      !strcmp(name, "initImageLoader") || !strcmp(name, "setUp") ||
      !strcmp(name, "FlurrySetAge") || !strcmp(name, "guestAcquireUid") ||
      !strcmp(name, "guestBind") || !strcmp(name, "userDelete") ||
      !strcmp(name, "makeWebView") || !strcmp(name, "netHttpSetProperty") ||
      !strcmp(name, "netHttpSetRequestMethodEx") ||
      !strcmp(name, "netHttpSetRequestPropertyEx") ||
      !strcmp(name, "netHttpConnectEx") || !strcmp(name, "start") ||
      !strcmp(name, "destroy") || !strcmp(name, "destroyUserInput") ||
      !strcmp(name, "createUserInput") || !strcmp(name, "onExitApplication"))
    return;

  if (!strcmp(name, "showDialogAndExit"))
    debugPrintf("JNI: showDialogAndExit(%d) -- engine wants to show a dialog\n",
                n >= 1 ? (int)a[0].i : -1);

  log_once(name, "");        /* per-frame calls would otherwise bury the log */
}

static int64_t dispatch_bool(const char *name, const char *sig, JVal *a, int n) {
  (void)sig; (void)a; (void)n;
  if (!strcmp(name, "isContains") || !strcmp(name, "netHttpIsContains"))
    return 0;
  /* The engine asks Java whether there is a network. Saying "no" makes it stop
   * before it ever tries, and the UI ends up showing a connection error instead
   * of running its normal server-unreachable path. Report online; the actual
   * connect() is refused further down (ENETUNREACH), which is the case the
   * engine handles by degrading to local. */
  if (!strcmp(name, "isNetworkAvailable") || !strcmp(name, "netIsOnline"))
    return 1;
  if (!strcmp(name, "hasInternetConnection"))
    return 1;
  return 0;
}

/* returns a jobject */
static void *dispatch_object(const char *name, const char *sig, JVal *a, int n) {
  (void)sig;

  if (!strcmp(name, "getFileDescriptorFromAsset") ||
      !strcmp(name, "openAsset") || !strcmp(name, "getAssetFileDescriptor")) {
    const char *asset = n > 0 ? (a[0].kind == 'S' ? a[0].s : "") : "";
    void *sizes = n > 1 ? a[1].o : NULL;
    int ns = 0;
    if (sizes && is_tag(sizes, TAG_ARRAY)) ns = ((FakeArray *)sizes)->len;
    return open_asset(asset, sizes ? ((FakeArray *)sizes)->data : NULL, ns);
  }
  /* Android's getUserDir() is a directory the engine concatenates names onto
   * without adding a separator ("%s%s/temp"), so it has to end with a slash --
   * without it the process-dump path came out as "...savecom.com2us...". */
  if (!strcmp(name, "getUserDir")) {
    static char dir[300];
    snprintf(dir, sizeof(dir), "%s/", g_save_dir);
    return jni_make_string(dir);
  }
  if (!strcmp(name, "getPackageName")) return jni_make_string(shim_package_name());
  if (!strcmp(name, "getDeviceModel")) return jni_make_string(device_string("model"));
  if (!strcmp(name, "getDeviceId")) return jni_make_string(device_string("id"));
  if (!strcmp(name, "getMacAddress")) return jni_make_string(device_string("mac"));
  if (!strcmp(name, "getSystemVersion")) return jni_make_string(device_string("version"));
  if (!strcmp(name, "getCountry")) return jni_make_string(device_string("country"));
  if (!strcmp(name, "getLanguage")) return jni_make_string(device_string("language"));
  /* HubSave_LoadKey builds the save key out of these two properties. Answering
   * empty leaves its 64-byte buffer untouched, so the engine encrypts with
   * whatever was on the stack -- a different key every run, and the reason a
   * save written in one session reads back as ERROR. */
  if (!strcmp(name, "getAppProperty")) {
    /* Signature is ([B)[B on this build -- the key arrives as a byte array and
     * the answer must be one too, which is why the String-based version above
     * never matched. */
    const char *key = NULL;
    if (n > 0) {
      if (a[0].kind == 'S') {
        key = a[0].s;
      } else if (a[0].kind == 'O' && a[0].o) {
        /* Not one of our tagged objects: the engine passes a raw buffer whose
         * first bytes are the key text. Take it only if it looks like ASCII. */
        if (is_tag(a[0].o, TAG_ARRAY))
          key = (const char *)((const FakeArray *)a[0].o)->data;
        else if (is_tag(a[0].o, TAG_STRING))
          key = ((const FakeString *)a[0].o)->utf;
        else {
          const unsigned char *raw = (const unsigned char *)a[0].o;
          if (raw[0] >= 0x20 && raw[0] < 0x7f)
            key = (const char *)raw;
        }
      }
    }
    {
      static int key_logs;
      if (key_logs < 6) {
        key_logs++;
        if (key)
          debugPrintf("save: getAppProperty key '%s'\n", key);
        else
          debugPrintf("save: getAppProperty unparsed kind %c ptr %p tag %08x\n",
                      n > 0 ? a[0].kind : '?', n > 0 ? a[0].o : NULL,
                      (n > 0 && a[0].o) ? *(unsigned *)a[0].o : 0u);
      }
    }
    if (key && strstr(key, "uniqueUserId")) {
      static const char id[] =
          "SWITCH0000INOTIA40000USER0000000000000000000000000000";
      debugPrintf("save: property '%s' -> fixed user id\n", key);
      return jni_make_byte_array(id, sizeof(id) - 1);
    }
    if (key && strstr(key, "uniqueDeviceId")) {
      debugPrintf("save: property '%s' -> \"1\"\n", key);
      return jni_make_byte_array("1", 1);
    }
    return jni_make_byte_array("", 1);
  }
  if (!strcmp(name, "getPhoneNumber")) return jni_make_string("");
  if (!strcmp(name, "getPropertyReturnKeyType") ||
      !strcmp(name, "GetStringFromXml") || !strcmp(name, "getSavedString"))
    return jni_make_string("");
  /* HUB_LOGIN_INFO is the stored Hive session. Reading it (finding it empty) is
   * what makes the engine raise the login prompt, so remembering the read tells
   * main.c exactly which prompt it is about to see. */
  if (!strcmp(name, "GetStringFromXmlEx") || !strcmp(name, "GetStringFromXmlEx2")) {
    const char *key = "";
    if (n > 1) {
      if (a[1].kind == 'S')
        key = a[1].s ? a[1].s : "";
      else if (a[1].kind == 'O')
        key = str_of(a[1].o);      /* Java String arguments arrive as objects */
    }
    if (strstr(key, "HUB_LOGIN_INFO")) {
      /* Pretend a local guest session is already stored. With an empty value the
       * engine raises the Hive prompt, and the "declined" branch it takes after
       * that never finishes its screen transition -- the save-slot panel opens,
       * fades out and the picture stays black. A non-empty record skips the
       * prompt entirely and sends the engine straight to the slots. */
      g_hub_probe = 1;
      return jni_make_string(
          "{\"HUB_UID\":\"local\",\"HUB_TOKEN\":\"local\","
          "\"NICKNAME\":\"Player\",\"SERVER_ID\":\"0\"}");
    }
    return jni_make_string("");
  }
  if (!strcmp(name, "originalJson")) return jni_make_string("");
  /* Hive reads the country/language to pick a server; a placeholder object
   * instead of a string makes that logic take a nonsense branch. */
  if (!strcmp(name, "getInitialCountry")) return jni_make_string("US");
  if (!strcmp(name, "getInitialLanguage")) return jni_make_string("en");

  /* Hash helpers must come back as real byte arrays: the engine reads them with
   * GetArrayLength/GetByteArrayElements and a placeholder object there turns
   * into a wild length or a stray pointer. */
  if (!strcmp(name, "generateHash")) {
    jlong mix = 1469598103934665603LL;
    for (int i = 0; i < n; i++) {
      if (a[i].kind == 'O' && is_tag(a[i].o, TAG_ARRAY)) {
        const FakeArray *arr = (const FakeArray *)a[i].o;
        for (int k = 0; k < arr->len; k++)
          mix = (mix ^ ((const uint8_t *)arr->data)[k]) * 1099511628211LL;
      }
    }
    uint8_t digest[16];
    for (int i = 0; i < 16; i++)
      digest[i] = (uint8_t)(mix >> ((i & 7) * 8));
    return jni_make_byte_array(digest, sizeof(digest));
  }
  if (!strcmp(name, "getAppProperty")) {
    uint8_t v[4] = { 0, 0, 0, 0 };
    return jni_make_byte_array(v, sizeof(v));
  }

  /* Never return NULL from an object-returning method. The engine stores it and
   * dereferences it much later, which turns "a call we did not model" into a
   * crash with no relation to the actual cause. A tagged placeholder answers
   * GetStringUTFChars with "" and GetObjectClass with a class. */
  log_once(name, "?obj");
  return jni_make_object("object");
}

/* drawText / drawTextInRect receive the string as a byte[] plus geometry. The
 * exact packing is engine-internal, so decode defensively: pull the text out of
 * whichever argument carries it and treat the remaining ints in order. */
static void text_draw_from_jni(const char *name, JVal *a, int n) {
  const char *str = NULL;
  int ivals[10];
  int ni = 0;

  for (int i = 0; i < n && ni < 10; i++) {
    if (a[i].kind == 'S') {
      str = a[i].s;
    } else if (a[i].kind == 'A') {
      const FakeArray *fa = a[i].o;
      if (is_tag(fa, TAG_ARRAY) && fa->data)
        str = (const char *)fa->data;
    } else if (a[i].kind == 'I' || a[i].kind == 'Z') {
      ivals[ni++] = (int)a[i].i;
    }
  }
  if (!str)
    return;

  const int x = ni > 0 ? ivals[0] : 0;
  const int y = ni > 1 ? ivals[1] : 0;
  if (!strcmp(name, "drawTextInRect")) {
    const int w = ni > 2 ? ivals[2] : 0;
    const int h = ni > 3 ? ivals[3] : 0;
    const int color = ni > 4 ? ivals[4] : (int)0xffffffff;
    const int size = ni > 5 ? ivals[5] : 0;
    text_draw_rect(str, x, y, w, h, color, size);
  } else {
    const int color = ni > 2 ? ivals[2] : (int)0xffffffff;
    const int size = ni > 3 ? ivals[3] : 0;
    text_draw(str, x, y, color, size);
  }
}

/* ------------------------------------------------------------------------- */
/* JNIEnv table                                                              */
/* ------------------------------------------------------------------------- */

static jint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return jni_make_object(name);
}

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  log_once(name ? name : "?", sig ? sig : "?");
  return get_id(name, sig, 0);
}
static void *j_GetStaticMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  log_once(name ? name : "?", sig ? sig : "?");
  return get_id(name, sig, 0);
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; (void)cls;
  return get_id(name, sig, 1);
}
static void *j_GetObjectClass(void *env, void *obj) { (void)env; (void)obj; return jni_make_object("class"); }
static void *j_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static void  j_DeleteRef(void *env, void *obj) { (void)env; (void)obj; }
static void  j_void0(void *env) { (void)env; }
static jint  j_int1(void *env, void *a) { (void)env; (void)a; return 0; }
static jint  j_int2(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 0; }
static jint  j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static jint  j_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static void *j_PopLocalFrame(void *env, void *res) { (void)env; return res; }
/* Direct buffers: the engine takes a ByteBuffer from Java, asks for its raw
 * pointer once at init, and writes its event queue straight into it. Handing
 * back NULL is not a soft failure there -- the first enqueue dereferences it. */
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env;
  return is_tag(buf, TAG_ARRAY) ? ((FakeArray *)buf)->data : NULL;
}
static jint j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env;
  return is_tag(buf, TAG_ARRAY) ? ((FakeArray *)buf)->len : -1;
}
static void *j_NewDirectByteBuffer(void *env, void *addr, jlong cap) {
  (void)env;
  FakeArray *a = calloc(1, sizeof(*a));
  if (!a) return NULL;
  a->tag = TAG_ARRAY;
  a->elem_size = 1;
  a->len = (int)cap;
  a->data = addr;          /* caller owns the memory; nothing frees it */
  return a;
}

static jint  j_RegisterNatives(void *env, void *cls, void *m, int n) {
  (void)env; (void)cls; (void)m; (void)n; return 0;
}

static jint j_unimplemented(void) {
  /* called from a per-frame path in some builds: a handful of lines is enough
   * to notice, a thousand of them bury everything else in the log */
  static int n;
  if (n < 20) {
    /* the caller's address names the engine function that wants this slot;
     * subtract load_virtbase from the log to get an offset for tools/where.py */
    debugPrintf("JNI: unimplemented slot, called from %p\n",
                __builtin_return_address(0));
    n++;
  }
  return 0;
}

#define DISPATCH_PRELUDE                                  \
  JVal a[16];                                             \
  va_list va; va_start(va, id);                           \
  const int n = read_args(id->sig, va, a, 16);            \
  va_end(va);

static jboolean j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return (jboolean)dispatch_bool(id->name, id->sig, a, n);
}
static jboolean j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  ID_SAFE(id);
  (void)env; (void)obj;
  JVal a[16];
  const int n = read_args(id->sig, va, a, 16);
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return (jboolean)dispatch_bool(id->name, id->sig, a, n);
}

static jint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return (jint)dispatch_int(id->name, id->sig, a, n);
}
static jint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  ID_SAFE(id);
  (void)env; (void)obj;
  JVal a[16];
  const int n = read_args(id->sig, va, a, 16);
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return (jint)dispatch_int(id->name, id->sig, a, n);
}

#define DISPATCH_A_PRELUDE                                \
  JVal a[16];                                             \
  const int n = read_jvalues(id->sig, args, a, 16);

static void *j_CallObjectMethodA(void *env, void *obj, FakeID *id, JValueArg *args) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_A_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return dispatch_object(id->name, id->sig, a, n);
}
static void j_CallVoidMethodA(void *env, void *obj, FakeID *id, JValueArg *args) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_A_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  dispatch_void(id->name, id->sig, a, n);
}
static jint j_CallIntMethodA(void *env, void *obj, FakeID *id, JValueArg *args) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_A_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return (jint)dispatch_int(id->name, id->sig, a, n);
}
static jboolean j_CallBooleanMethodA(void *env, void *obj, FakeID *id, JValueArg *args) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_A_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return (jboolean)dispatch_bool(id->name, id->sig, a, n);
}

static jlong j_CallLongMethodA(void *env, void *obj, FakeID *id, JValueArg *args) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_A_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  log_once(id->name, id->sig);
  return (jlong)dispatch_int(id->name, id->sig, a, n);
}

static jlong j_CallLongMethod(void *env, void *obj, FakeID *id, ...) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return (jlong)dispatch_int(id->name, id->sig, a, n);
}

static jfloat j_CallFloatMethod(void *env, void *obj, FakeID *id, ...) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return 0.0f;
}

static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);

  if (!strcmp(id->name, "drawText") || !strcmp(id->name, "drawTextInRect")) {
    text_draw_from_jni(id->name, a, n);
    return;
  }
  dispatch_void(id->name, id->sig, a, n);
}
static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  ID_SAFE(id);
  (void)env; (void)obj;
  JVal a[16];
  const int n = read_args(id->sig, va, a, 16);
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  if (!strcmp(id->name, "drawText") || !strcmp(id->name, "drawTextInRect")) {
    text_draw_from_jni(id->name, a, n);
    return;
  }
  dispatch_void(id->name, id->sig, a, n);
}

static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  ID_SAFE(id);
  (void)env; (void)obj;
  DISPATCH_PRELUDE
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return dispatch_object(id->name, id->sig, a, n);
}
static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  ID_SAFE(id);
  (void)env; (void)obj;
  JVal a[16];
  const int n = read_args(id->sig, va, a, 16);
  g_jni_caller = __builtin_return_address(0);
  g_jni_caller2 = __builtin_return_address(1);
  g_call_count++;
  snprintf(g_last_method, sizeof(g_last_method), "%s", id->name);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: %s%s\n", id->name, id->sig);
  }
  log_once(id->name, id->sig);
  return dispatch_object(id->name, id->sig, a, n);
}

static void *j_NewObject(void *env, void *cls, FakeID *id, ...) {
  ID_SAFE(id);
  (void)env;
  return jni_make_object(is_tag(cls, TAG_OBJECT) ? ((FakeObject *)cls)->label : "new");
}
static void *j_NewObjectV(void *env, void *cls, FakeID *id, va_list va) {
  ID_SAFE(id);
  (void)env; (void)id; (void)va;
  return jni_make_object(is_tag(cls, TAG_OBJECT) ? ((FakeObject *)cls)->label : "new");
}

/* fields */
static void *j_GetObjectField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return NULL; }
static jint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env;
  if (is_tag(obj, TAG_OBJECT)) {
    FakeObject *o = obj;
    if (!strcmp(id->name, "descriptor")) return (jint)o->ivalue;
    return (jint)o->ivalue;
  }
  return 0;
}
static jlong j_GetLongField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return 0; }
static jboolean j_GetBooleanField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return 0; }
static jfloat j_GetFloatField(void *env, void *obj, FakeID *id) { (void)env; (void)obj; (void)id; return 0.0f; }

/* strings */
static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env;
  static int n;
  if (n < 40 && utf) {           /* the strings the engine hands Java say a lot
                                  * about which subsystem is talking */
    debugPrintf("JNI: NewStringUTF(\"%s\")\n", utf);
    n++;
  }
  return jni_make_string(utf);
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  const char *p = str_of(jstr);
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: GetStringUTFChars(\"%s\") = %p\n", p, (const void *)p);
  }
  return p;
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}
static jint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  const jint n = (jint)strlen(str_of(jstr));
  if (g_trace_calls > 0) {
    g_trace_calls--;
    debugPrintf("JNI+: GetStringUTFLength(\"%s\") = %d\n", str_of(jstr), (int)n);
  }
  return n;
}
static jint j_GetStringLength(void *env, void *jstr) {
  (void)env; return (jint)strlen(str_of(jstr));
}

/* arrays */
static jint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  return is_tag(arr, TAG_ARRAY) ? ((FakeArray *)arr)->len : 0;
}
static void *j_NewByteArray(void *env, jint len) { (void)env; return jni_make_byte_array(NULL, len); }
static void *j_NewIntArray(void *env, jint len) { (void)env; return jni_make_byte_array(NULL, len * 4); }
static void *j_GetByteArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  return is_tag(arr, TAG_ARRAY) ? ((FakeArray *)arr)->data : NULL;
}
static void j_ReleaseByteArrayElements(void *env, void *arr, void *elems, jint mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetByteArrayRegion(void *env, void *arr, jint start, jint len, void *buf) {
  (void)env;
  if (!is_tag(arr, TAG_ARRAY) || !buf) return;
  FakeArray *fa = arr;
  if (start < 0 || start + len > fa->len) return;
  memcpy(buf, (uint8_t *)fa->data + start, len);
}
static void j_SetByteArrayRegion(void *env, void *arr, jint start, jint len, const void *buf) {
  (void)env;
  if (!is_tag(arr, TAG_ARRAY) || !buf) return;
  FakeArray *fa = arr;
  if (start < 0 || start + len > fa->len) return;
  memcpy((uint8_t *)fa->data + start, buf, len);
}
static void *j_GetIntArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  return is_tag(arr, TAG_ARRAY) ? ((FakeArray *)arr)->data : NULL;
}
static void j_ReleaseIntArrayElements(void *env, void *arr, void *elems, jint mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetIntArrayRegion(void *env, void *arr, jint start, jint len, jint *buf) {
  (void)env;
  if (!is_tag(arr, TAG_ARRAY) || !buf) return;
  FakeArray *fa = arr;
  if (start < 0 || (start + len) * 4 > fa->len) return;
  memcpy(buf, (uint8_t *)fa->data + start * 4, len * 4);
}
static void j_SetIntArrayRegion(void *env, void *arr, jint start, jint len, const jint *buf) {
  (void)env;
  if (!is_tag(arr, TAG_ARRAY) || !buf) return;
  FakeArray *fa = arr;
  if (start < 0 || (start + len) * 4 > fa->len) return;
  memcpy((uint8_t *)fa->data + start * 4, buf, len * 4);
}

/* misc */
static jint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
static jint j_Throw(void *env, void *obj) { (void)env; (void)obj; return 0; }
static jint j_ThrowNew(void *env, void *cls, const char *msg) {
  (void)env; (void)cls;
  debugPrintf("JNI: engine threw '%s'\n", msg ? msg : "");
  return 0;
}
static jint j_MonitorEnter(void *env, void *obj) { (void)env; (void)obj; return 0; }

/* ------------------------------------------------------------------------- */
/* table assembly (indices per the JNI specification)                        */
/* ------------------------------------------------------------------------- */

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static jint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static jint vm_AttachThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static jint vm_ret0(void *vm) { (void)vm; return JNI_OK; }

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  id_count = 0;
  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  /* Indices follow the JNIEnv layout from the specification exactly: the
   * engine indexes this table by byte offset (slot * 8), so a single shift
   * would make it call an unrelated function. Verified against the binary:
   * CWrapperData_nativeInitialize uses slot 21 (NewGlobalRef),
   * nativeCheckQueueEvent uses slot 113 (GetStaticMethodID). */
  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[13]  = (void *)j_Throw;
  env_table[14]  = (void *)j_ThrowNew;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void0;               /* ExceptionDescribe */
  env_table[17]  = (void *)j_void0;               /* ExceptionClear */
  env_table[18]  = (void *)j_void0;               /* FatalError */
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteRef;           /* DeleteGlobalRef */
  env_table[23]  = (void *)j_DeleteRef;           /* DeleteLocalRef */
  env_table[24]  = (void *)j_int2;                /* IsSameObject */
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_int1;                /* EnsureLocalCapacity */
  /* Indices here follow the JNINativeInterface layout exactly -- the engine is
   * a plain C caller reading the table at slot*8, so an off-by-one does not
   * fail loudly, it hands back a function of the wrong signature. This block
   * used to be shifted by one from AllocObject onwards, which made GetObjectClass
   * return the integer 2 and every later method call operate on that. */
  env_table[27]  = (void *)j_NewObject;           /* AllocObject */
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[30]  = (void *)j_NewObject;           /* NewObjectA */
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_int1;                /* IsInstanceOf */
  env_table[33]  = (void *)j_GetMethodID;

  /* Call<Type>Method / V / A: three slots each, starting at CallObjectMethod */
  /* Slot numbers were read out of the engine's own _JNIEnv::* templates rather
   * than assumed: this libgame was built against a jni.h whose instance block
   * sits one slot lower than the one in a current NDK, and whose static block
   * sits 29 slots higher. With the textbook numbers the engine called a
   * varargs function where it expected a jvalue array, which is how
   * getMaxMemory() came back as 0 and the loader then refused to load. */
  env_table[34] = (void *)j_CallObjectMethod;
  env_table[35] = (void *)j_CallObjectMethodA;    /* engine's CallObjectMethod */
  env_table[36] = (void *)j_CallObjectMethodA;
  env_table[37] = (void *)j_CallBooleanMethod;
  env_table[38] = (void *)j_CallBooleanMethodA;   /* engine's CallBooleanMethod */
  env_table[39] = (void *)j_CallBooleanMethodA;
  env_table[49] = (void *)j_CallIntMethod;
  env_table[50] = (void *)j_CallIntMethodA;       /* engine's CallIntMethod */
  env_table[51] = (void *)j_CallIntMethodA;
  env_table[52] = (void *)j_CallLongMethod;
  env_table[53] = (void *)j_CallLongMethodA;
  env_table[54] = (void *)j_CallLongMethodA;
  env_table[55] = (void *)j_CallFloatMethod;
  env_table[56] = (void *)j_CallIntMethodA;
  env_table[57] = (void *)j_CallIntMethodA;
  env_table[61] = (void *)j_CallVoidMethod;
  env_table[62] = (void *)j_CallVoidMethodA;      /* engine's CallVoidMethod */
  env_table[63] = (void *)j_CallVoidMethodA;
  for (int base = 40; base <= 58; base += 3) {    /* Byte..Double */
    if (base == 49 || base == 52 || base == 55)
      continue;                                   /* set explicitly above */
    env_table[base + 0] = (void *)j_CallIntMethod;
    env_table[base + 1] = (void *)j_CallIntMethodV;
    env_table[base + 2] = (void *)j_CallIntMethod;
  }

  env_table[64]  = (void *)j_GetFieldID;
  env_table[65]  = (void *)j_GetObjectField;
  env_table[66]  = (void *)j_GetBooleanField;
  env_table[70]  = (void *)j_GetIntField;
  env_table[71]  = (void *)j_GetLongField;
  env_table[72]  = (void *)j_GetFloatField;
  env_table[73]  = (void *)j_GetFloatField;       /* GetDoubleField */

  env_table[83]  = (void *)j_GetStaticMethodID;
  env_table[84]  = (void *)j_CallObjectMethod;    /* CallStaticObjectMethod */
  env_table[85]  = (void *)j_CallObjectMethodV;
  env_table[86]  = (void *)j_CallObjectMethod;
  env_table[87]  = (void *)j_CallBooleanMethod;
  env_table[88]  = (void *)j_CallBooleanMethodV;
  env_table[89]  = (void *)j_CallBooleanMethodA;
  env_table[99]  = (void *)j_CallIntMethod;
  env_table[100] = (void *)j_CallIntMethodV;
  env_table[101] = (void *)j_CallIntMethodA;
  env_table[102] = (void *)j_CallLongMethod;
  env_table[103] = (void *)j_CallLongMethod;
  env_table[104] = (void *)j_CallLongMethodA;  /* CallStaticLongMethodA */
  env_table[105] = (void *)j_CallFloatMethod;
  env_table[106] = (void *)j_CallFloatMethod;
  env_table[107] = (void *)j_CallFloatMethod;
  env_table[111] = (void *)j_CallVoidMethod;
  env_table[112] = (void *)j_CallVoidMethodV;
  /* 113 is GetStaticMethodID in this engine's table, not CallStaticVoidMethodA:
   * jGetPackageName loads slot 0x388 and passes (clazz, "getPackageName",
   * "()Ljava/lang/String;"). Treating it as a call made the string pointer look
   * like a method id, which is where the truncated method names in the log came
   * from. The engine never uses CallStaticVoidMethodA. */
  env_table[113] = (void *)j_GetStaticMethodID;
  for (int base = 90; base <= 108; base += 3) {   /* static Byte..Double */
    if (base == 99 || base == 102 || base == 105)
      continue;
    env_table[base + 0] = (void *)j_CallIntMethod;
    env_table[base + 1] = (void *)j_CallIntMethodV;
    env_table[base + 2] = (void *)j_CallIntMethod;
  }

  /* This engine's static-method block sits 30 slots higher than the classic
   * jni.h layout: jGetPackageName resolves GetStaticMethodID at 113 and
   * _JNIEnv::CallStaticObjectMethod jumps through 115 (ldr x8,[x8,#0x398]).
   * Leaving 115 as GetStaticObjectField made that call return a field value (0)
   * where the engine expected a jstring. Cover the whole block with call
   * handlers so no slot in it can be mistaken for a field accessor. */
  env_table[114] = (void *)j_CallObjectMethod;    /* CallStaticObjectMethod */
  env_table[115] = (void *)j_CallObjectMethodA;   /* engine's CallStaticObjectMethod */
  env_table[116] = (void *)j_CallObjectMethodA;
  env_table[117] = (void *)j_CallBooleanMethod;
  env_table[118] = (void *)j_CallBooleanMethodA;  /* engine's CallStaticBooleanMethod */
  env_table[119] = (void *)j_CallBooleanMethodA;
  for (int i = 120; i <= 128; i++) env_table[i] = (void *)j_CallIntMethodA;
  env_table[129] = (void *)j_CallIntMethod;
  env_table[130] = (void *)j_CallIntMethodA;      /* engine's CallStaticIntMethod */
  env_table[131] = (void *)j_CallIntMethodA;
  env_table[132] = (void *)j_CallLongMethod;
  env_table[133] = (void *)j_CallLongMethodA;     /* engine's CallStaticLongMethod */
  env_table[134] = (void *)j_CallLongMethodA;
  for (int i = 135; i <= 140; i++) env_table[i] = (void *)j_CallIntMethodA;  /* float/double */
  env_table[141] = (void *)j_CallVoidMethod;
  env_table[142] = (void *)j_CallVoidMethodA;     /* engine's CallStaticVoidMethod */
  env_table[143] = (void *)j_CallVoidMethodA;
  /* static field accessors, in case the engine reaches them past the calls */
  env_table[144] = (void *)j_GetFieldID;
  env_table[173] = (void *)j_GetFieldID;          /* GetStaticFieldID, +29 form */

  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[184] = (void *)j_GetByteArrayElements;
  env_table[187] = (void *)j_GetIntArrayElements;
  env_table[192] = (void *)j_ReleaseByteArrayElements;
  env_table[195] = (void *)j_ReleaseIntArrayElements;
  env_table[200] = (void *)j_GetByteArrayRegion;
  env_table[203] = (void *)j_GetIntArrayRegion;
  env_table[208] = (void *)j_SetByteArrayRegion;
  env_table[211] = (void *)j_SetIntArrayRegion;

  env_table[215] = (void *)j_RegisterNatives;
  env_table[217] = (void *)j_MonitorEnter;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[229] = (void *)j_NewDirectByteBuffer;
  env_table[230] = (void *)j_GetDirectBufferAddress;
  env_table[231] = (void *)j_GetDirectBufferCapacity;

  vm_table[3] = (void *)vm_ret0;             /* DestroyJavaVM */
  vm_table[4] = (void *)vm_AttachThread;     /* AttachCurrentThread */
  vm_table[5] = (void *)vm_ret0;             /* DetachCurrentThread */
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachThread;     /* AttachCurrentThreadAsDaemon */

  /* name the gaps once at startup: an "unimplemented slot" in the middle of a
   * frame tells us nothing, a list up front tells us what to write next */
  char gaps[160];
  int gl = 0;
  for (int i = 0; i < 233; i++) {
    if (env_table[i] != (void *)j_unimplemented)
      continue;
    if (gl < (int)sizeof(gaps) - 6)
      gl += snprintf(gaps + gl, sizeof(gaps) - gl, "%d ", i);
  }
  debugPrintf("JNI: fake environment ready (env=%p vm=%p)\n", fake_env, fake_vm);
  debugPrintf("JNI: unimplemented slots: %s\n", gl ? gaps : "none");
}

void jni_update(void) {
  /* nothing is deferred yet: the swkbd / dialog paths are handled inline */
}
