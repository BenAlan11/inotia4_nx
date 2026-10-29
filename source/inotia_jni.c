/* inotia_jni.c -- the Java the engine talks to.
 *
 * libgame.so is a C engine wrapped in a thin Java layer (com.com2us.wrapper.*).
 * On a phone the wrapper objects do the platform work: they hand out the file
 * paths, play sounds through SoundPool/MediaPlayer, rasterise glyphs through
 * Typeface, and deliver touches. Here those answers come from this table.
 *
 * Order matters: the first match wins, so each class's named handlers come
 * before its wildcard entry. The wildcards are deliberate -- an unhandled
 * wrapper call returning 0 keeps the engine running instead of faulting
 * through a NULL method ID, and g_jni_log (config.ini [debug] log_java_calls)
 * records every call that lands on one, which is the to-do list for the next
 * stage.
 *
 * MIT.
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bionic_io.h"

#include "inotia.h"
#include "jni.h"
#include "rt_settings.h"
#include "util.h"

/* ------------------------------------------------------------- File objects
 * The engine builds its paths from Context.getFilesDir() / getExternalFilesDir
 * and then asks the File for its path. The path stays in Android form
 * (/sdcard/..., /data/data/...) on purpose: dcr_path translates it when the
 * engine finally opens it. */
static void file_free(JObj *self) {
  free(self->p);
  self->p = NULL;
}

static JObj *file_obj(const char *path) {
  JObj *o = jni_new("java/io/File");
  o->p = strdup(path);
  o->finalize = file_free;
  return o;
}

static const char *file_path(JObj *self) { return self && self->p ? (const char *)self->p : ""; }

static jvalue h_file_path(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  return jv_l(jni_str(file_path(self)));
}

static jvalue h_file_exists(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  FILE *f = fopen(file_path(self), "rb");
  if (!f)
    return jv_z(0);
  fclose(f);
  return jv_z(1);
}

/* --------------------------------------------------------------- Context */
#define MAIN_ACTIVITY PORT_PACKAGE "/MainActivity"

static jvalue h_package_name(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str(PORT_PACKAGE));
}

static jvalue h_ext_files(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(file_obj("/sdcard/Android/data/" PORT_PACKAGE "/files"));
}

static jvalue h_files_dir(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(file_obj("/data/data/" PORT_PACKAGE "/files"));
}

static jvalue h_cache_dir(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(file_obj("/data/data/" PORT_PACKAGE "/cache"));
}

static jvalue h_ext_storage(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(file_obj("/sdcard"));
}

/* ------------------------------------------------------------ DisplayMetrics */
/* The engine reads the screen size from the metrics object it is given, so the
 * handlers below fill the fields on it. */
static void fill_metrics(JObj *dm) {
  if (!dm)
    return;
  jni_set_field(dm, "widthPixels", jv_i(RT_SCREEN_W), 0);
  jni_set_field(dm, "heightPixels", jv_i(RT_SCREEN_H), 0);
  jni_set_field(dm, "density", jv_f(1.0f), 0);
  jni_set_field(dm, "densityDpi", jv_i(160), 0);
  jni_set_field(dm, "scaledDensity", jv_f(1.0f), 0);
  jni_set_field(dm, "xdpi", jv_f(160.0f), 0);
  jni_set_field(dm, "ydpi", jv_f(160.0f), 0);
}

static jvalue h_new_metrics(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  JObj *dm = jni_new("android/util/DisplayMetrics");
  fill_metrics(dm);
  return jv_l(dm);
}

static jvalue h_get_metrics(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  if (a && a[0].l)
    fill_metrics((JObj *)a[0].l);
  return jv_none();
}

/* ---------------------------------------------------------------- sound */
static jvalue h_sound_load(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  const char *path = a && a[0].l ? jni_utf(a[0].l) : NULL;
  if (!path)
    return jv_i(0);
  /* A raw resource id (an int) instead of a path: not a file we can open. */
  const int id = inotia_sound_load(path);
  return jv_i(id);
}

/* play(soundID, leftVolume, rightVolume, priority, loop, rate) */
static jvalue h_sound_play(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  if (!a)
    return jv_i(0);
  const int id = a[0].i;
  const float vol = a[1].f;
  const int loop = a[4].i;
  return jv_i(inotia_sound_play(id, vol, loop));
}

static jvalue h_sound_stop(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  inotia_sound_stop(a ? a[0].i : 0);
  return jv_none();
}

static jvalue h_sound_volume(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  inotia_sound_set_volume(a ? a[0].i : 0, a ? (float)a[1].f : 1.0f);
  return jv_none();
}

/* ------------------------------------------------------------ MediaPlayer */
/* One object per player; its state lives in the object's payload. */
static jvalue h_media_create(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_new("android/media/MediaPlayer"));
}

static jvalue h_media_set_source(JObj *self, const jvalue *a, const JMethod *m) {
  (void)m;
  const char *path = a && a[0].l ? jni_utf(a[0].l) : NULL;
  return jv_i(path ? inotia_media_open(self, path) : -1);
}

static jvalue h_media_start(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  inotia_media_start(self);
  return jv_none();
}
static jvalue h_media_pause(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  inotia_media_pause(self);
  return jv_none();
}
static jvalue h_media_stop(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  inotia_media_stop(self);
  return jv_none();
}
static jvalue h_media_release(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  inotia_media_release(self);
  return jv_none();
}
static jvalue h_media_seek(JObj *self, const jvalue *a, const JMethod *m) {
  (void)m;
  inotia_media_seek(self, a ? a[0].i : 0);
  return jv_none();
}
static jvalue h_media_pos(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  return jv_i(inotia_media_position(self));
}
static jvalue h_media_dur(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  return jv_i(inotia_media_duration(self));
}
static jvalue h_media_playing(JObj *self, const jvalue *a, const JMethod *m) {
  (void)a, (void)m;
  return jv_z(inotia_media_is_playing(self));
}
static jvalue h_media_volume(JObj *self, const jvalue *a, const JMethod *m) {
  (void)m;
  inotia_media_set_volume(self, a ? (float)a[0].f : 1.0f);
  return jv_none();
}
static jvalue h_media_looping(JObj *self, const jvalue *a, const JMethod *m) {
  (void)m;
  inotia_media_set_looping(self, a ? a[0].z : 0);
  return jv_none();
}
/* setOnCompletionListener etc.: keep the listener so a finished track can call
 * onCompletion() back. Storing it is enough for now -- the mixer calls it. */
static jvalue h_media_listener(JObj *self, const jvalue *a, const JMethod *m) {
  (void)m;
  if (a && a[0].l)
    jni_set_field(self, "listener", a[0], 1);
  return jv_none();
}

/* ------------------------------------------------------------ MotionEvent */
/* The engine reads the current touches from the object it polls; these answer
 * from the input bridge in inotia_input.c. */
static jvalue h_touch_count(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_i(inotia_touch_count());
}
static jvalue h_touch_action(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_i(0);
}
static jvalue h_touch_x(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  return jv_f((jfloat)inotia_touch_x(a ? a[0].i : 0));
}
static jvalue h_touch_y(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  return jv_f((jfloat)inotia_touch_y(a ? a[0].i : 0));
}

/* ------------------------------------------------------------------ Paint */
/* Text metrics: the engine asks how wide a string is before drawing it. A
 * rough advance per character keeps the layout sane until stage 1f puts a real
 * rasteriser behind CFont. */
static jvalue h_measure_text(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  const char *s = a && a[0].l ? jni_utf(a[0].l) : "";
  const float px = 12.0f;
  return jv_f((jfloat)(strlen(s) * px * 0.55f));
}

/* --------------------------------------- com.com2us.wrapper.function.CFunction
 * The engine's own answers about the device, asked as static calls on CFunction
 * (the dex has them: tools/dex_class.py com.com2us.wrapper.function.CFunction).
 * getPackageName is the first one the engine needs -- jgetMediaPath does
 * GetStaticMethodID(CFunction, "getPackageName", "()Ljava/lang/String;") and
 * strcpy()s the answer, so a NULL there is a fault at address 0. */
/* Trailing slash matters: the engine builds "<userDir><packageName>/appinfo.dat"
 * by concatenation, and run #11 showed ".../filescom.com2us.../appinfo.dat". */
static jvalue h_cf_user_dir(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str("/sdcard/Android/data/" PORT_PACKAGE "/files/"));
}

static jvalue h_cf_max_memory(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_j(512LL * 1024 * 1024);
}

/* The Java side answers ApplicationInfo.sourceDir, i.e. /data/app/<pkg>-1/base.apk.
 * The engine treats a name that starts with '/' as an absolute path and anything
 * else as a relative asset name, so handing it the SD path ("sdmc:/...") made it
 * prefix the resource folders: run #13 shows it looking for
 * resources/common/game_res/sdmc:/switch/inotia4_nx/Inotia4.apk and then feeding
 * the APK to libpng. dcr_path maps /data/app/<pkg>-1/base.apk to the player's own
 * APK, so the Android form is both faithful and functional. */
static jvalue h_cf_apk_path(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str("/data/app/" PORT_PACKAGE "-1/base.apk"));
}

static jvalue h_cf_device_id(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str("switch"));
}

static jvalue h_cf_model(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str("Switch"));
}

static jvalue h_cf_language(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str("en"));
}

static jvalue h_cf_country(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str("US"));
}

static jvalue h_cf_sys_version(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(jni_str("9"));
}

/* API 28: new enough for the engine's non-scoped-storage paths, and the value
 * the wrapper's own version check expects. */
static jvalue h_cf_sys_version_code(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_i(28);
}

static jvalue h_cf_free_memory(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_j(512LL * 1024 * 1024);
}

/* convertDPtoPX / convertOriginaltoDisplay: identity. The engine's layout was
 * built for one density and one resolution, and both are what it gets. */
static jvalue h_cf_same_int(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  return jv_i(a ? a[0].i : 0);
}

static jvalue h_cf_same_obj(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  return jv_l(a ? a[0].l : NULL);
}

static jvalue h_cf_activity(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_l(inotia_activity());
}

/* ------------------------------------------- CResource.getFileDescriptorFromAsset
 * This has to behave like Android's AssetManager, because that is what the
 * engine is written against: the FileDescriptor it gets back is the APK's own
 * fd, and the int[2] holds (offset, length) of the entry *inside* the APK. The
 * engine lseeks that fd to the offset and reads.
 *
 * Serving a separate file instead (offset 0, its own fd) worked for the first
 * image and then broke: the engine closes the fd and reopens the APK, the fd
 * number is reused, and the next read of the "asset" returns the zip header --
 * run #14's "libpng error: Not a PNG file".
 *
 * So the APK is opened once and kept, the central directory is parsed once into
 * a small table, and every asset answers (apk_fd, entry_offset, entry_size).
 * Only stored (uncompressed) entries qualify, which is all of them: aapt stores
 * the .jpg-named assets verbatim. */
#define APK_MAX_ENTRIES 2048
#define APK_NAME_MAX 88
static struct {
  char name[APK_NAME_MAX];
  unsigned off, size, method;
} g_apk_entries[APK_MAX_ENTRIES];
static int g_apk_entry_n = -1; /* -1: not parsed yet */
static int g_apk_fd = -1;
static int g_asset_miss;

static unsigned rd16(const unsigned char *p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void apk_parse_directory(void) {
  g_apk_entry_n = 0;
  FILE *f = fopen(inotia_apk_path(), "rb");
  if (!f)
    return;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return;
  }
  const long end = ftell(f);
  const long tail = end < 66000 ? end : 66000;
  unsigned char *buf = malloc((size_t)tail);
  if (!buf || tail < 22 || fseek(f, end - tail, SEEK_SET) != 0) {
    free(buf);
    fclose(f);
    return;
  }
  const size_t got = fread(buf, 1, (size_t)tail, f);
  long eocd = -1;
  for (long i = (long)got - 22; i >= 0; i--)
    if (rd32(buf + i) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd < 0) {
    free(buf);
    fclose(f);
    return;
  }
  const unsigned count = rd16(buf + eocd + 10);
  const unsigned cd_off = rd32(buf + eocd + 16);
  const unsigned cd_size = rd32(buf + eocd + 12);
  unsigned char *cd = malloc(cd_size ? cd_size : 1);
  if (!cd || fseek(f, (long)cd_off, SEEK_SET) != 0 || fread(cd, 1, cd_size, f) != cd_size) {
    free(cd);
    free(buf);
    fclose(f);
    return;
  }
  unsigned p = 0;
  for (unsigned i = 0; i < count && p + 46 <= cd_size && g_apk_entry_n < APK_MAX_ENTRIES; i++) {
    if (rd32(cd + p) != 0x02014b50)
      break;
    const unsigned name_len = rd16(cd + p + 28);
    const unsigned extra_len = rd16(cd + p + 30);
    const unsigned comment_len = rd16(cd + p + 32);
    const unsigned method = rd16(cd + p + 10);
    const unsigned csize = rd32(cd + p + 20);
    const unsigned lho = rd32(cd + p + 42);
    if (p + 46 + name_len <= cd_size && name_len < APK_NAME_MAX) {
      memcpy(g_apk_entries[g_apk_entry_n].name, cd + p + 46, name_len);
      g_apk_entries[g_apk_entry_n].name[name_len] = 0;
      /* the data starts after the local header's own name/extra fields */
      unsigned lname = 0, lextra = 0;
      if (fseek(f, (long)lho, SEEK_SET) == 0) {
        unsigned char lh[30];
        if (fread(lh, 1, sizeof lh, f) == sizeof lh && rd32(lh) == 0x04034b50) {
          lname = rd16(lh + 26);
          lextra = rd16(lh + 28);
        }
      }
      g_apk_entries[g_apk_entry_n].off = lho + 30 + lname + lextra;
      g_apk_entries[g_apk_entry_n].size = csize;
      g_apk_entries[g_apk_entry_n].method = method;
      g_apk_entry_n++;
    }
    p += 46 + name_len + extra_len + comment_len;
  }
  free(cd);
  free(buf);
  fclose(f);
  debugPrintf("[res] APK directory: %d entries indexed\n", g_apk_entry_n);
}

static const char *const k_asset_forms[] = {
    "assets/common/game_res/%s.jpg", "assets/common/game_res/%s",
    "assets/common/%s.jpg",          "assets/common/%s",
    "assets/%s.jpg",                 "assets/%s",
};

/* -> index in g_apk_entries, or -1 */
static int apk_find(const char *name) {
  if (g_apk_entry_n < 0)
    apk_parse_directory();
  char want[APK_NAME_MAX];
  for (unsigned i = 0; i < sizeof k_asset_forms / sizeof k_asset_forms[0]; i++) {
    snprintf(want, sizeof want, k_asset_forms[i], name);
    for (int k = 0; k < g_apk_entry_n; k++)
      if (!strcmp(g_apk_entries[k].name, want))
        return k;
  }
  return -1;
}

static jvalue h_cf_fd_asset(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  const char *name = (a && a[0].l) ? jni_utf(a[0].l) : "";
  JObj *out = (a && a[1].l) ? (JObj *)a[1].l : NULL;
  int *slot = (out && out->kind == JK_ARRAY && out->a.data && out->a.len >= 2)
                  ? (int *)out->a.data
                  : NULL;

  const int e = apk_find(name);
  if (e < 0 || g_apk_entries[e].method != 0) {
    if (slot) {
      slot[0] = -1;
      slot[1] = 0;
    }
    if (g_asset_miss++ < 12)
      debugPrintf("[res] asset '%s': not in the APK (or not stored)\n", name);
    return jv_l(NULL);
  }
  if (g_apk_fd < 0)
    g_apk_fd = b_open(inotia_apk_path(), O_RDONLY);
  if (g_apk_fd < 0) {
    if (slot) {
      slot[0] = -1;
      slot[1] = 0;
    }
    return jv_l(NULL);
  }

  if (slot) {
    slot[0] = (int)g_apk_entries[e].off;
    slot[1] = (int)g_apk_entries[e].size;
  }
  static int logged;
  if (logged++ < 6)
    debugPrintf("[res] asset '%s' -> %s +%u, %u bytes\n", name, g_apk_entries[e].name,
                g_apk_entries[e].off, g_apk_entries[e].size);

  JObj *fdo = jni_new("java/io/FileDescriptor");
  jni_set_field(fdo, "descriptor", jv_i(g_apk_fd), 0);
  return jv_l(fdo);
}

/* The fd above is the APK's own and is shared by every asset, exactly as
 * AssetManager shares it -- closing it on one asset would break the rest, so
 * these only exist to answer the calls. */
static jvalue h_cf_close_asset(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_none();
}

static jvalue h_cf_close_assets(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_none();
}

/* ------------------------------------------------------- direct ByteBuffers
 * CEventHandler's two queues are direct buffers the Java side allocates with
 * allocateDirect() (CEventHandler.<init>: 16 bytes of manage data, 2048 bytes
 * of event queue = 256 events x 8 bytes). The engine reads them through
 * GetDirectBufferAddress, so they have to be real memory with the layout the
 * runtime's NewDirectByteBuffer makes. */
JObj *inotia_direct_buffer(void *addr, int cap) {
  JObj *o = jni_new("java/nio/DirectByteBuffer");
  o->p = addr;
  o->v[0] = cap;
  return o;
}

/* ------------------------------------------------------- CSoundManager
 * soundCreate answered -1 through the wildcard, because the Java side resolves
 * the name with CResource.getFullPathName() and assetOpenFd(), neither of which
 * this port has, so it never gets an AssetFileDescriptor. The engine stores the
 * -1 and SOUNDSYSTEM_Process -- the last thing MainProcess does each tick --
 * goes looking for it. Run #24 died on the very tick after a burst of
 * soundCreate/soundPrepare, with the title screen already drawn.
 *
 * The port has a real mixer (stb_vorbis in inotia_audio.c) but the assets live
 * inside the APK, so wiring the name to a file is a separate job. Until then a
 * sound is a valid id that does nothing, which is what an engine can hold
 * without falling over. */
static int g_next_sound_id = 1;
static int g_sound_created, g_sound_played;

static jvalue h_snd_create(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  const char *name = (a && a[0].l) ? jni_utf(a[0].l) : "";
  const int id = g_next_sound_id++;
  if (g_sound_created++ < 12)
    debugPrintf("[snd] create '%s' -> %d (silent: no file backing yet)\n", name, id);
  return jv_i(id);
}

static jvalue h_snd_new_id(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_i(g_next_sound_id++);
}

static jvalue h_snd_true(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_z(1);
}

static jvalue h_snd_play(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)m;
  if (g_sound_played++ < 12)
    debugPrintf("[snd] play %d\n", a ? (int)a[0].i : -1);
  return jv_z(1);
}

static jvalue h_snd_volume(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_i(100);
}

/* ------------------------------------------------ the Java-side singletons
 * The engine never keeps a Java object it was handed: it reads the wrapper
 * layer's static fields. getWrapperData() is
 * GetStaticObjectField(CWrapperData, "mWrapperData"), and the hand-off after
 * CWrapperData.nativeInitialize(tz) goes through that object -- with NULL there
 * the engine calls setDataFromNative through a NULL method ID and never
 * receives its own settings (country, language, device, screen size). */
static JObj *gl_view(void) {
  static JObj *view;
  if (!view)
    view = jni_new("com/com2us/wrapper/game/CCustomGLSurfaceView");
  return view;
}

static JObj *wrapper_data(void) {
  static JObj *data;
  if (!data)
    data = jni_new("com/com2us/wrapper/kernel/CWrapperData");
  return data;
}

static jvalue f_activity(JObj *self, const JField *f) {
  (void)self, (void)f;
  return jv_l(inotia_activity());
}

static jvalue f_gl_view(JObj *self, const JField *f) {
  (void)self, (void)f;
  return jv_l(gl_view());
}

static jvalue f_wrapper_data(JObj *self, const JField *f) {
  (void)self, (void)f;
  return jv_l(wrapper_data());
}

static jvalue h_view_w(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_i(RT_SCREEN_W);
}

static jvalue h_view_h(JObj *self, const jvalue *a, const JMethod *m) {
  (void)self, (void)a, (void)m;
  return jv_i(RT_SCREEN_H);
}

/* ----------------------------------------------------------------- tables */

const JMethodDef jni_method_defs[] = {
    /* ---- java/io/File */
    {"java/io/File", "getAbsolutePath", NULL, h_file_path},
    {"java/io/File", "getPath", NULL, h_file_path},
    {"java/io/File", "getCanonicalPath", NULL, h_file_path},
    {"java/io/File", "toString", NULL, h_file_path},
    {"java/io/File", "exists", NULL, h_file_exists},
    {"java/io/File", "mkdirs", NULL, jni_h_true},
    {"java/io/File", "mkdir", NULL, jni_h_true},
    {"java/io/File", NULL, NULL, jni_h_null},

    /* ---- Context / Activity: paths and identity */
    {"android/content/Context", "getPackageName", NULL, h_package_name},
    {"android/content/Context", "getExternalFilesDir", NULL, h_ext_files},
    {"android/content/Context", "getExternalCacheDir", NULL, h_ext_files},
    {"android/content/Context", "getFilesDir", NULL, h_files_dir},
    {"android/content/Context", "getCacheDir", NULL, h_cache_dir},
    {"android/content/Context", "getExternalStorageDirectory", NULL, h_ext_storage},
    {"android/content/Context", "getApplicationContext", NULL, jni_h_self},
    {"android/content/Context", "getSystemService", NULL, jni_h_null},

    /* ---- screen size */
    {"android/util/DisplayMetrics", "<init>", NULL, h_new_metrics},
    {"android/view/Display", "getMetrics", NULL, h_get_metrics},

    /* ---- sound */
    {"android/media/SoundPool", "<init>", NULL, jni_h_self},
    {"android/media/SoundPool", "load", NULL, h_sound_load},
    {"android/media/SoundPool", "play", NULL, h_sound_play},
    {"android/media/SoundPool", "stop", NULL, h_sound_stop},
    {"android/media/SoundPool", "pause", NULL, h_sound_stop},
    {"android/media/SoundPool", "resume", NULL, jni_h_void},
    {"android/media/SoundPool", "setVolume", NULL, h_sound_volume},
    {"android/media/SoundPool", "unload", NULL, jni_h_true},
    {"android/media/SoundPool", "release", NULL, jni_h_void},

    {"android/media/MediaPlayer", "create", NULL, h_media_create},
    {"android/media/MediaPlayer", "<init>", NULL, jni_h_self},
    {"android/media/MediaPlayer", "setDataSource", NULL, h_media_set_source},
    {"android/media/MediaPlayer", "prepare", NULL, jni_h_void},
    {"android/media/MediaPlayer", "prepareAsync", NULL, jni_h_void},
    {"android/media/MediaPlayer", "start", NULL, h_media_start},
    {"android/media/MediaPlayer", "pause", NULL, h_media_pause},
    {"android/media/MediaPlayer", "stop", NULL, h_media_stop},
    {"android/media/MediaPlayer", "release", NULL, h_media_release},
    {"android/media/MediaPlayer", "reset", NULL, jni_h_void},
    {"android/media/MediaPlayer", "seekTo", NULL, h_media_seek},
    {"android/media/MediaPlayer", "getCurrentPosition", NULL, h_media_pos},
    {"android/media/MediaPlayer", "getDuration", NULL, h_media_dur},
    {"android/media/MediaPlayer", "isPlaying", NULL, h_media_playing},
    {"android/media/MediaPlayer", "setVolume", NULL, h_media_volume},
    {"android/media/MediaPlayer", "setLooping", NULL, h_media_looping},
    {"android/media/MediaPlayer", "setOnCompletionListener", NULL, h_media_listener},
    {"android/media/MediaPlayer", "setOnErrorListener", NULL, h_media_listener},
    {"android/media/MediaPlayer", "setOnPreparedListener", NULL, h_media_listener},

    /* ---- touch */
    {"android/view/MotionEvent", "getPointerCount", NULL, h_touch_count},
    {"android/view/MotionEvent", "getAction", NULL, h_touch_action},
    {"android/view/MotionEvent", "getActionMasked", NULL, h_touch_action},
    {"android/view/MotionEvent", "getX", NULL, h_touch_x},
    {"android/view/MotionEvent", "getY", NULL, h_touch_y},

    /* ---- CResource: the assets the engine would read out of the APK */
    {"com/com2us/wrapper/function/CResource", "getFileDescriptorFromAsset", NULL, h_cf_fd_asset},
    {"com/com2us/wrapper/function/CResource", "closeFileDescriptor", NULL, h_cf_close_asset},
    {"com/com2us/wrapper/function/CResource", "closeFileDescriptorAll", NULL, h_cf_close_assets},
    {"com/com2us/wrapper/function/CResource", NULL, NULL, jni_h_zero},

    /* ---- the game's own wrapper: how much memory it may use */
    {"com/com2us/wrapper/WrapperUserDefined", "getMaxMemory", NULL, h_cf_max_memory},

    /* ---- java.nio: the buffers above, so a Java-side call still chains */
    {"java/nio/ByteBuffer", "order", NULL, jni_h_self},
    {"java/nio/ByteBuffer", "rewind", NULL, jni_h_self},
    {"java/nio/ByteBuffer", "clear", NULL, jni_h_self},
    {"java/nio/ByteBuffer", "flip", NULL, jni_h_self},
    {"java/nio/ByteBuffer", NULL, NULL, jni_h_zero},

    /* ---- text metrics */
    {"android/graphics/Paint", "measureText", NULL, h_measure_text},

    /* ---- com.com2us.wrapper.function.CFunction: the device's own answers */
    {"com/com2us/wrapper/function/CFunction", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/function/CFunction", "getUserDir", NULL, h_cf_user_dir},
    {"com/com2us/wrapper/function/CFunction", "getApkFilePath", NULL, h_cf_apk_path},
    {"com/com2us/wrapper/function/CFunction", "getActivity", NULL, h_cf_activity},
    {"com/com2us/wrapper/function/CFunction", "getGLSurfaceView", NULL, h_cf_activity},
    {"com/com2us/wrapper/function/CFunction", "getDeviceId", NULL, h_cf_device_id},
    {"com/com2us/wrapper/function/CFunction", "getAndroidId", NULL, h_cf_device_id},
    {"com/com2us/wrapper/function/CFunction", "getCom2usUniqueId", NULL, h_cf_device_id},
    {"com/com2us/wrapper/function/CFunction", "getDeviceModel", NULL, h_cf_model},
    {"com/com2us/wrapper/function/CFunction", "getPhoneNumber", NULL, jni_h_empty_string},
    {"com/com2us/wrapper/function/CFunction", "getMIN", NULL, h_cf_same_obj},
    {"com/com2us/wrapper/function/CFunction", "getLanguage", NULL, h_cf_language},
    {"com/com2us/wrapper/function/CFunction", "getInitialLanguage", NULL, h_cf_language},
    {"com/com2us/wrapper/function/CFunction", "getCountry", NULL, h_cf_country},
    {"com/com2us/wrapper/function/CFunction", "getInitialCountry", NULL, h_cf_country},
    {"com/com2us/wrapper/function/CFunction", "getSystemVersion", NULL, h_cf_sys_version},
    {"com/com2us/wrapper/function/CFunction", "getSystemVersionCode", NULL, h_cf_sys_version_code},
    {"com/com2us/wrapper/function/CFunction", "getNativeAvailableMemory", NULL, h_cf_free_memory},
    {"com/com2us/wrapper/function/CFunction", "convertDPtoPX", NULL, h_cf_same_int},
    {"com/com2us/wrapper/function/CFunction", "convertOriginaltoDisplay", NULL, h_cf_same_obj},
    {"com/com2us/wrapper/function/CFunction", "convertDisplaytoOriginal", NULL, h_cf_same_obj},
    {"com/com2us/wrapper/function/CFunction", "getActiveAirplaneMode", NULL, jni_h_false},
    {"com/com2us/wrapper/function/CFunction", "getRoaming", NULL, jni_h_false},
    {"com/com2us/wrapper/function/CFunction", "isActiveUsim", NULL, jni_h_false},
    {"com/com2us/wrapper/function/CFunction", "startVibrate", NULL, jni_h_void},
    {"com/com2us/wrapper/function/CFunction", "stopVibrate", NULL, jni_h_void},
    {"com/com2us/wrapper/function/CFunction", NULL, NULL, jni_h_zero},

    /* ---- classes the engine asks for that the APK does not define (a newer
     * wrapper than this build shipped): FindClass has to succeed or the JNI
     * layer is left with a pending NoClassDefFoundError. */
    {"com/com2us/wrapper/WrapperStatistics", NULL, NULL, jni_h_zero},
    {"com/com2us/peppermint/HubBridge", NULL, NULL, jni_h_zero},

    /* ---- the GLSurfaceView the engine thinks it renders into */
    {"com/com2us/wrapper/game/CCustomGLSurfaceView", "getWidth", NULL, h_view_w},
    {"com/com2us/wrapper/game/CCustomGLSurfaceView", "getHeight", NULL, h_view_h},
    {"com/com2us/wrapper/game/CCustomGLSurfaceView", "queueEvent", NULL, jni_h_void},
    {"com/com2us/wrapper/game/CCustomGLSurfaceView", "queueEventForcedly", NULL, jni_h_void},
    {"com/com2us/wrapper/game/CCustomGLSurfaceView", "requestRender", NULL, jni_h_void},
    {"com/com2us/wrapper/game/CCustomGLSurfaceView", NULL, NULL, jni_h_zero},

    /* ---- the Com2uS wrapper classes: named answers first, then a wildcard so
     * an unknown call returns a zero instead of faulting. The getPackageName
     * entries are the safety net for the engine's cached jclass: whichever
     * wrapper class it ends up holding, asking it for the package name has to
     * give the package name. */
    {"com/com2us/wrapper/kernel/CWrapperKernel", "nativeStart", NULL, jni_h_void},
    {"com/com2us/wrapper/kernel/CWrapperKernel", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/kernel/CWrapperData", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/kernel/CWrapper", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/kernel/CEventHandler", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/common/CCommonAPI", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/font/CFont", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/font/CSGLFont", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/sensor/CSensor", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/ui/CTextInput", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/ui/CUserInput", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/network/CWrapperHttp", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/network/CHttpManager", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/utility/CUtility", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/game/CGameManager", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/function/CFunctionTaskRunner", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/media/CSoundManager", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/WrapperUserDefined", "getPackageName", NULL, h_package_name},
    {"com/com2us/wrapper/kernel/CWrapperKernel", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/kernel/CWrapperData", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/kernel/CWrapper", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/kernel/CEventHandler", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/common/CCommonAPI", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/font/CFont", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/font/CSGLFont", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/sensor/CSensor", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/ui/CTextInput", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/ui/CUserInput", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/network/CWrapperHttp", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/network/CHttpManager", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/utility/CUtility", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/game/CGameManager", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/function/CFunctionTaskRunner", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/media/CSoundManager", "createSoundId", NULL, h_snd_new_id},
    {"com/com2us/wrapper/media/CSoundManager", "soundCreate", NULL, h_snd_create},
    {"com/com2us/wrapper/media/CSoundManager", "soundPrepare", NULL, h_snd_true},
    {"com/com2us/wrapper/media/CSoundManager", "soundPlay", NULL, h_snd_play},
    {"com/com2us/wrapper/media/CSoundManager", "soundStop", NULL, h_snd_true},
    {"com/com2us/wrapper/media/CSoundManager", "soundPause", NULL, h_snd_true},
    {"com/com2us/wrapper/media/CSoundManager", "soundResume", NULL, h_snd_true},
    {"com/com2us/wrapper/media/CSoundManager", "soundDestroy", NULL, h_snd_true},
    {"com/com2us/wrapper/media/CSoundManager", "soundSetVolume", NULL, h_snd_true},
    {"com/com2us/wrapper/media/CSoundManager", "soundGetVolume", NULL, h_snd_volume},
    {"com/com2us/wrapper/media/CSoundManager", "getVolume", NULL, h_snd_volume},
    {"com/com2us/wrapper/media/CSoundManager", "setVolume", NULL, h_snd_true},
    {"com/com2us/wrapper/media/CSoundManager", "getCurrentPlaying", NULL, jni_h_zero},
    {"com/com2us/wrapper/media/CSoundManager", "stopAll", NULL, jni_h_void},
    {"com/com2us/wrapper/media/CSoundManager", "stopCurrent", NULL, jni_h_void},
    {"com/com2us/wrapper/media/CSoundManager", NULL, NULL, jni_h_zero},
    {"com/com2us/wrapper/WrapperUserDefined", NULL, NULL, jni_h_zero},

    {NULL, NULL, NULL, NULL},
};

const JFieldDef jni_field_defs[] = {
    /* The engine gates features on the API level: 24 is the APK's minSdk, and
     * the oldest one that keeps the engine on its non-scoped-storage paths. */
    {"android/os/Build$VERSION", "SDK_INT", NULL, 24, NULL},
    {"android/os/Build", "MODEL", NULL, 0, "Switch"},
    {"android/os/Build", "MANUFACTURER", NULL, 0, "Nintendo"},
    {"android/os/Build", "DEVICE", NULL, 0, "nx"},
    {"android/os/Build", "PRODUCT", NULL, 0, PORT_NAME},

    /* The wrapper layer's singletons, as the engine reaches for them. */
    {"com/com2us/wrapper/kernel/CWrapperKernel", "mActivity", f_activity, 0, NULL},
    {"com/com2us/wrapper/kernel/CWrapperKernel", "mGLSurfaceView", f_gl_view, 0, NULL},
    {"com/com2us/wrapper/kernel/CWrapperData", "mWrapperData", f_wrapper_data, 0, NULL},

    {NULL, NULL, NULL, 0, NULL},
};

/* The engine calls Context methods on the activity, so the activity has to be
 * an Activity and an Activity a Context. */
const char *const jni_class_supers[][2] = {
    {MAIN_ACTIVITY, "android/app/Activity"},
    {"android/app/Activity", "android/content/Context"},
    {"android/app/Activity", "android/view/ContextThemeWrapper"},
    {"android/view/ContextThemeWrapper", "android/content/ContextWrapper"},
    {"android/content/ContextWrapper", "android/content/Context"},
    {"android/media/MediaPlayer", "java/lang/Object"},
    {"java/io/File", "java/lang/Object"},
    {"java/nio/DirectByteBuffer", "java/nio/ByteBuffer"},
    {NULL, NULL},
};

/* Classes the engine asks for that the APK's own classes.txt does not list:
 * the framework ones, plus the wrapper classes (the APK has them, but a
 * classes.txt read from the dex is not guaranteed to be complete on the first
 * start). */
const char *const jni_missing_classes[] = {
    "android/content/Context",
    "android/app/Activity",
    "android/os/Build",
    "android/os/Build$VERSION",
    "android/util/DisplayMetrics",
    "android/view/Display",
    "android/view/MotionEvent",
    "android/media/SoundPool",
    "android/media/MediaPlayer",
    "android/graphics/Paint",
    "android/graphics/Typeface",
    "java/io/File",
    "java/nio/ByteBuffer",
    "java/nio/DirectByteBuffer",
    "com/com2us/wrapper/kernel/CWrapperKernel",
    "com/com2us/wrapper/kernel/CWrapperData",
    "com/com2us/wrapper/kernel/CWrapper",
    "com/com2us/wrapper/kernel/CEventHandler",
    "com/com2us/wrapper/common/CCommonAPI",
    "com/com2us/wrapper/font/CFont",
    "com/com2us/wrapper/sensor/CSensor",
    "com/com2us/wrapper/ui/CTextInput",
    "com/com2us/wrapper/ui/CUserInput",
    "com/com2us/wrapper/network/CWrapperHttp",
    "com/com2us/wrapper/utility/CUtility",
    "com/com2us/wrapper/function/CFunction",
    "com/com2us/wrapper/function/CFunctionTaskRunner",
    "com/com2us/wrapper/function/CResource",
    "com/com2us/wrapper/game/CGameManager",
    "com/com2us/wrapper/game/CCustomGLSurfaceView",
    "com/com2us/wrapper/WrapperStatistics",
    "com/com2us/peppermint/HubBridge",
    "com/com2us/wrapper/font/CSGLFont",
    "com/com2us/wrapper/media/CSoundManager",
    "com/com2us/wrapper/network/CHttpManager",
    "com/com2us/wrapper/WrapperUserDefined",
    NULL,
};

/* ------------------------------------------------------- the objects */
JObj *inotia_activity(void) {
  static JObj *activity;
  if (!activity)
    activity = jni_new(MAIN_ACTIVITY);
  return activity;
}

JObj *inotia_wrapper_class(const char *short_name) {
  char full[160];
  snprintf(full, sizeof full, "%s", short_name);
  for (char *p = full; *p; p++)
    if (*p == '.')
      *p = '/';
  JClass *c = jni_class(full);
  return c ? c->obj : NULL;
}
