/* audio_bridge.c -- replace the engine's CS_mdaSound* C API with the port mixer
 *
 * Why not the JNI layer?  The engine's Android shell owns a
 * com.com2us.wrapper.media.CSoundManager Java class.  On Android the Java side
 * creates a SoundPool / MediaPlayer; here there is no Dalvik, so jni_fake.c
 * answers those calls.  That path is dead for three independent reasons:
 *
 *   1. jSoundManagerInitialize() is a Java -> native entry nobody calls on the
 *      Switch.  It is the ONLY place that caches the soundPlay(IZ)Z jmethodID
 *      at [module+0x6e4680].  With no Java, that slot stays NULL and every
 *      jSoundPlay() arrives at CallBooleanMethodA as a bogus id -- debug.log
 *      showed 24 "got a bogus method id 0x0" lines and zero soundPlay calls.
 *   2. jni_fake.c dispatches soundLoad / playSound / createSound, but the
 *      engine's real names are soundCreate / soundPrepare / soundPlay.  So
 *      soundCreate always returned 0 and soundPrepare returned false.
 *   3. The PLT stub the engine uses is
 *        _ZN7_JNIEnv17CallBooleanMethodEP8_jobjectP10_jmethodIDz   (varargs)
 *      while env_table[38] is wired to the jvalue-array (A) reader.  Giving
 *      soundCreate a real non-zero id would make the next call read that id
 *      as a jvalue* and take a data abort.
 *
 * All of that is bypassed by overriding the engine's own C API instead:
 *
 *   SOUNDSYSTEM_Play          -> CS_mdaSoundPlay(handle, 0)
 *   SOUNDSYSTEM_PlayBGM_IPHONE-> CS_mdaSoundCreate(&h, "SOUND/BGM/%s",..,1)
 *                                CS_mdaSoundPrepare(h)
 *                                CS_mdaSoundPlay(h, 1)      <- BGM loops
 *                                CS_mdaSoundSetVolume(h,100)
 *
 * so one hook family covers both effects and music.  Handles are 64-bit slots;
 * BGM gets a synthetic handle and goes through the streaming decoder in
 * audio.c so a 1 MB ogg never becomes 40 MB of resident PCM.
 */

#include <switch.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "audio_bridge.h"
#include "config.h"
#include "util.h"

#define SND_MAX       256     /* must be >= MAX_SFX in audio.c */
#define BGM_HANDLE    0x40000000LL
#define RES_PREFIX    "resources/common/game_res/"

static int  slot_vol[SND_MAX];
static char bgm_name[256];
static int  bgm_loaded;

static int is_bgm_name(const char *name) {
  return name && (!strncmp(name, "SOUND/BGM/", 10) ||
                  !strncmp(name, "sound/bgm/", 10));
}

/* The engine passes logical resource names ("SOUND/SE/UI_Menu_Op") with no
 * extension; the SD-card tree stores them as bare Ogg files below game_res/.
 * This is the same prefix the engine's own RES loader uses (see the fs: open
 * lines in debug.log), so no new config key is needed. */
static void sound_path(const char *name, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s%s", g_data_dir, RES_PREFIX, name ? name : "");
}

void audio_bridge_init(void) {
  for (int i = 0; i < SND_MAX; i++)
    slot_vol[i] = 100;
  bgm_loaded = 0;
  debugPrintf("audio bridge: CS_mdaSound* -> port mixer (%s/%s...)\n",
              g_data_dir, RES_PREFIX);
}

/* CS_mdaSoundCreate(int64_t *out, const char *name, i64, i64, int is_bgm)
 * The original stores jSoundCreate(name, flag)'s result in *out and returns 0
 * on success.  We do the same, but load the file ourselves. */
int64_t nx_mda_sound_create(int64_t *out, const char *name,
                            int64_t a2, int64_t a3, int flag) {
  (void)a2; (void)a3; (void)flag;
  if (!out || !name || !*name)
    return -9;

  char path[512];
  sound_path(name, path, sizeof(path));

  if (is_bgm_name(name)) {
    if (audio_bgm_load(path) != 0) {
      debugPrintf("audio bridge: BGM load failed '%s'\n", path);
      *out = -1;
      return -9;
    }
    snprintf(bgm_name, sizeof(bgm_name), "%s", name);
    bgm_loaded = 1;
    *out = BGM_HANDLE;
    debugPrintf("audio bridge: BGM '%s' -> handle %lld\n", name,
                (long long)BGM_HANDLE);
    return 0;
  }

  const int id = audio_sfx_load(path);
  if (id < 0) {
    debugPrintf("audio bridge: sfx load failed '%s'\n", path);
    *out = -1;
    return -9;
  }
  if (id < SND_MAX)
    slot_vol[id] = 100;
  *out = id;
  return 0;
}

/* The remaining entry points all follow the same convention as the original:
 * 0 == success, -9 == "sound subsystem unavailable".  Returning 0 keeps the
 * engine's SOUNDSYSTEM state machine moving instead of parking it in an error
 * branch that never plays anything. */
int64_t nx_mda_sound_prepare(int64_t id) {
  (void)id;
  return 0;
}

int64_t nx_mda_sound_play(int64_t id, int loop) {
  static int logs;
  if (id == BGM_HANDLE) {
    audio_bgm_play(loop ? 1 : 0);
    debugPrintf("audio bridge: BGM play loop=%d\n", loop);
    return 0;
  }
  if (id < 0 || id >= SND_MAX)
    return -9;
  const float v = slot_vol[id] / 100.0f;
  audio_sfx_play((int)id, v, loop ? 1 : 0);
  if (logs < 20) {
    logs++;
    debugPrintf("audio bridge: play id=%lld vol=%.2f loop=%d\n",
                (long long)id, v, loop);
  }
  return 0;
}

int64_t nx_mda_sound_stop(int64_t id) {
  if (id == BGM_HANDLE) { audio_bgm_stop(); return 0; }
  if (id >= 0 && id < SND_MAX)
    audio_sfx_stop((int)id);
  return 0;
}

int64_t nx_mda_sound_destroy(int64_t id) {
  /* samples are cached for the whole run (170 SE files, ~2.3 MB of ogg); the
   * engine re-creates the same names on every screen change, so freeing here
   * would just re-decode them a frame later */
  return nx_mda_sound_stop(id);
}

int64_t nx_mda_sound_pause(int64_t id) {
  if (id == BGM_HANDLE) audio_bgm_pause();
  return 0;
}

int64_t nx_mda_sound_resume(int64_t id) {
  if (id == BGM_HANDLE) audio_bgm_play(1);
  return 0;
}

int64_t nx_mda_sound_set_volume(int64_t id, int vol) {
  if (vol < 0) vol = 0;
  if (vol > 100) vol = 100;
  if (id == BGM_HANDLE) {
    audio_set_bgm_volume((float)vol / 100.0f);
    return 0;
  }
  if (id >= 0 && id < SND_MAX)
    slot_vol[id] = vol;
  return 0;
}

int64_t nx_mda_sound_get_volume(int64_t id) {
  if (id == BGM_HANDLE)
    return (int64_t)(audio_bgm_volume() * 100.0f);
  if (id >= 0 && id < SND_MAX)
    return slot_vol[id];
  return 0;
}

int64_t nx_mda_stop_all(void) {
  audio_sfx_stop_all();
  audio_bgm_stop();
  return 0;
}
