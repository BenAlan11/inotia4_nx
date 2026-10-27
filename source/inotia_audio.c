/* inotia_audio.c -- the mixer behind the engine's Java audio path.
 *
 * libgame.so imports no OpenSL ES and no FMOD: its sound goes through the
 * wrapper (CS_mdaSound*) into Java's SoundPool and MediaPlayer. So the port has
 * to be the audio backend. That is this file: stb_vorbis streams (the game's
 * .ogg files are stored with a .jpg extension in the APK, but they are plain
 * Ogg), mixed into rt_audout's pump on its own thread.
 *
 * Eight voices is what a SoundPool-style engine expects (one BGM + several
 * effects); a ninth request steals the quietest finished one. Everything the
 * engine asks for is by integer id, so the id space here is ours: a sound id is
 * index+1 into g_sounds, a stream id is voice+1.
 *
 * MIT. stb_vorbis is public domain (see stb_vorbis.inc).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "inotia.h"
#include "jni.h"
#include "rt_audout.h"
#include "rt_settings.h"
#include "util.h"

/* One translation unit holds the decoder, so no separate header is needed. */
#include "stb_vorbis.inc"

#define MAX_SOUNDS 96
#define VOICES 8
#define FRAMES RT_AUDOUT_FRAMES

typedef struct {
  stb_vorbis *v;
  float vol;
  int loop;
  int active;
  int owns; /* the voice was opened for a stream (SoundPool) or a player */
} Voice;

typedef struct {
  char path[512];
} Sound;

static Voice g_voices[VOICES];
static Sound g_sounds[MAX_SOUNDS];
static int g_nsounds;
static Mutex g_lock;
static int g_started;
static int g_paused;

int inotia_audio_init(void) {
  if (g_started)
    return 0;
  mutexInit(&g_lock);
  if (rt_audout_open() != 0) {
    debugPrintf("[audio] audout open failed\n");
    return -1;
  }
  debugPrintf("[audio] audout %u Hz\n", rt_audout_rate());
  g_started = 1;
  return 0;
}

void inotia_audio_shutdown(void) {
  if (!g_started)
    return;
  mutexLock(&g_lock);
  for (int i = 0; i < VOICES; i++) {
    if (g_voices[i].v)
      stb_vorbis_close(g_voices[i].v);
    memset(&g_voices[i], 0, sizeof g_voices[i]);
  }
  mutexUnlock(&g_lock);
  rt_audout_close();
  g_started = 0;
}

void inotia_audio_pause(int paused) {
  g_paused = paused;
  if (g_started)
    rt_audout_pause(paused);
}

/* ------------------------------------------------------------------ mixing */

static void mix_voice(Voice *vo, int32_t *acc, int frames) {
  if (!vo->active || !vo->v)
    return;
  const stb_vorbis_info info = stb_vorbis_get_info(vo->v);
  const int ch = info.channels >= 2 ? 2 : 1;
  static short tmp[FRAMES * 2];
  int got = stb_vorbis_get_frame_short_interleaved(vo->v, ch, tmp, frames * ch);
  if (got <= 0) {
    if (vo->loop) {
      stb_vorbis_seek_start(vo->v);
      got = stb_vorbis_get_frame_short_interleaved(vo->v, ch, tmp, frames * ch);
    }
    if (got <= 0) {
      vo->active = 0;
      return;
    }
  }
  for (int i = 0; i < got; i++) {
    const int l = tmp[i * ch];
    const int r = ch == 2 ? tmp[i * ch + 1] : l;
    acc[i * 2 + 0] += (int32_t)(l * vo->vol);
    acc[i * 2 + 1] += (int32_t)(r * vo->vol);
  }
}

static void fill(int16_t *out, int frames, void *ud) {
  (void)ud;
  static int32_t acc[FRAMES * 2];
  memset(acc, 0, sizeof(int32_t) * (size_t)frames * 2);
  mutexLock(&g_lock);
  for (int i = 0; i < VOICES; i++)
    mix_voice(&g_voices[i], acc, frames);
  mutexUnlock(&g_lock);
  for (int i = 0; i < frames * 2; i++) {
    int32_t s = acc[i];
    if (s > 32767)
      s = 32767;
    if (s < -32768)
      s = -32768;
    out[i] = (int16_t)s;
  }
}

static int pump_start(void) {
  if (inotia_audio_init() != 0)
    return -1;
  if (rt_audout_pump_start(fill, NULL, 0x2A, 2) != 0) {
    debugPrintf("[audio] pump start failed\n");
    return -1;
  }
  debugPrintf("[audio] mixer running (%d voices)\n", VOICES);
  return 0;
}

static Voice *voice_alloc(void) {
  for (int i = 0; i < VOICES; i++)
    if (!g_voices[i].active && !g_voices[i].v)
      return &g_voices[i];
  /* Steal the first one: a SoundPool engine assumes it always gets a stream. */
  for (int i = 0; i < VOICES; i++) {
    if (g_voices[i].v) {
      stb_vorbis_close(g_voices[i].v);
      memset(&g_voices[i], 0, sizeof g_voices[i]);
      return &g_voices[i];
    }
  }
  return NULL;
}

static stb_vorbis *open_path(const char *path, int *ms) {
  int err = 0;
  stb_vorbis *v = stb_vorbis_open_filename(path, &err, NULL);
  if (!v) {
    debugPrintf("[audio] cannot decode %s (vorbis error %d)\n", path, err);
    return NULL;
  }
  const stb_vorbis_info info = stb_vorbis_get_info(v);
  if (ms) {
    const unsigned samples = stb_vorbis_stream_length_in_samples(v);
    *ms = info.sample_rate ? (int)(samples * 1000ull / info.sample_rate) : 0;
  }
  return v;
}

/* --------------------------------------------------------------- SoundPool */

int inotia_sound_load(const char *path) {
  if (!path || !*path)
    return 0;
  /* A bare number is a raw resource id, not a file: the APK's assets are
   * unpacked to the resource folder, so the engine passes paths here. */
  if (path[0] >= '0' && path[0] <= '9')
    return 0;
  for (int i = 0; i < g_nsounds; i++)
    if (strcmp(g_sounds[i].path, path) == 0)
      return i + 1;
  if (g_nsounds >= MAX_SOUNDS) {
    debugPrintf("[audio] sound table full, dropping %s\n", path);
    return 0;
  }
  snprintf(g_sounds[g_nsounds].path, sizeof g_sounds[0].path, "%s", path);
  g_nsounds++;
  debugPrintf("[audio] sound %d: %s\n", g_nsounds, path);
  return g_nsounds;
}

int inotia_sound_play(int id, float volume, int loop) {
  if (id <= 0 || id > g_nsounds)
    return 0;
  if (pump_start() != 0)
    return 0;
  stb_vorbis *v = open_path(g_sounds[id - 1].path, NULL);
  if (!v)
    return 0;
  mutexLock(&g_lock);
  Voice *vo = voice_alloc();
  if (!vo) {
    mutexUnlock(&g_lock);
    stb_vorbis_close(v);
    return 0;
  }
  vo->v = v;
  vo->vol = volume <= 0.0f ? 1.0f : volume;
  vo->loop = loop != 0;
  vo->active = 1;
  const int stream = (int)(vo - g_voices) + 1;
  mutexUnlock(&g_lock);
  return stream;
}

void inotia_sound_stop(int stream) {
  if (stream <= 0 || stream > VOICES)
    return;
  mutexLock(&g_lock);
  Voice *vo = &g_voices[stream - 1];
  if (vo->v)
    stb_vorbis_close(vo->v);
  memset(vo, 0, sizeof *vo);
  mutexUnlock(&g_lock);
}

void inotia_sound_set_volume(int stream, float volume) {
  if (stream <= 0 || stream > VOICES)
    return;
  mutexLock(&g_lock);
  g_voices[stream - 1].vol = volume;
  mutexUnlock(&g_lock);
}

/* ------------------------------------------------------------- MediaPlayer */

typedef struct {
  char path[512];
  stb_vorbis *v;
  int voice; /* the voice playing this track, 0 when none */
  int playing;
  int looping;
  float vol;
  int duration_ms;
} Media;

static Media *media(JObj *self) { return self ? (Media *)self->p : NULL; }

static void media_free(JObj *self) {
  inotia_media_stop(self);
  free(self->p);
  self->p = NULL;
}

static Media *media_new(JObj *self) {
  Media *m = media(self);
  if (m)
    return m;
  m = calloc(1, sizeof *m);
  if (!m)
    return NULL;
  m->vol = 1.0f;
  self->p = m;
  self->finalize = media_free;
  return m;
}

int inotia_media_open(JObj *self, const char *path) {
  Media *m = media_new(self);
  if (!m || !path)
    return -1;
  mutexLock(&g_lock);
  if (m->v) {
    stb_vorbis_close(m->v);
    m->v = NULL;
  }
  snprintf(m->path, sizeof m->path, "%s", path);
  m->v = open_path(path, &m->duration_ms);
  m->playing = 0;
  mutexUnlock(&g_lock);
  return m->v ? 0 : -1;
}

void inotia_media_start(JObj *self) {
  Media *m = media(self);
  if (!m || !m->v)
    return;
  if (pump_start() != 0)
    return;
  if (m->voice)
    inotia_sound_stop(m->voice);
  /* The player's stream lives in a voice of its own so the mixer picks it up. */
  mutexLock(&g_lock);
  Voice *vo = voice_alloc();
  if (vo) {
    if (vo->v)
      stb_vorbis_close(vo->v);
    vo->v = m->v;
    vo->vol = m->vol;
    vo->loop = m->looping;
    vo->active = 1;
    m->voice = (int)(vo - g_voices) + 1;
    m->v = NULL; /* the voice owns the stream now */
  }
  mutexUnlock(&g_lock);
  m->playing = 1;
}

void inotia_media_pause(JObj *self) {
  Media *m = media(self);
  if (m)
    m->playing = 0;
}
void inotia_media_stop(JObj *self) {
  Media *m = media(self);
  if (!m)
    return;
  m->playing = 0;
  if (m->voice) {
    inotia_sound_stop(m->voice);
    m->voice = 0;
  }
  mutexLock(&g_lock);
  if (m->v) {
    stb_vorbis_close(m->v);
    m->v = NULL;
  }
  mutexUnlock(&g_lock);
}
void inotia_media_release(JObj *self) {
  Media *m = media(self);
  if (!m)
    return;
  inotia_media_stop(self);
  free(m);
  self->p = NULL;
}
void inotia_media_seek(JObj *self, int ms) {
  Media *m = media(self);
  if (!m || !m->v || ms < 0)
    return;
  const stb_vorbis_info info = stb_vorbis_get_info(m->v);
  mutexLock(&g_lock);
  stb_vorbis_seek(m->v, (unsigned)((uint64_t)ms * info.sample_rate / 1000));
  mutexUnlock(&g_lock);
}
int inotia_media_position(JObj *self) {
  Media *m = media(self);
  if (!m)
    return 0;
  if (!m->v)
    return m->duration_ms; /* handed to a voice: no offset to read back yet */
  const stb_vorbis_info info = stb_vorbis_get_info(m->v);
  return info.sample_rate ? (int)(stb_vorbis_get_sample_offset(m->v) * 1000ull / info.sample_rate) : 0;
}
int inotia_media_duration(JObj *self) {
  Media *m = media(self);
  return m ? m->duration_ms : 0;
}
int inotia_media_is_playing(JObj *self) {
  Media *m = media(self);
  return m ? m->playing : 0;
}
void inotia_media_set_volume(JObj *self, float volume) {
  Media *m = media(self);
  if (m)
    m->vol = volume;
}
void inotia_media_set_looping(JObj *self, int loop) {
  Media *m = media(self);
  if (m)
    m->looping = loop != 0;
}
