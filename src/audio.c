/* audio.c -- Ogg Vorbis -> mixer -> audout
 *
 * The Android build splits audio in two on the Java side:
 *   CSoundPool    short, preloaded effects, several instances at once
 *   CMediaPlayer  one long, looping music stream with volume fades
 * Both end up in the same 48 kHz stereo mixer below, which a dedicated thread
 * feeds to libnx's audout. Effects are decoded once into RAM (they are tiny),
 * music is decoded on the fly so a 1 MB BGM track does not become 40 MB of PCM.
 */

#include <switch.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>

#include "audio.h"
#include "jni_fake.h"
#include "config.h"
#include "util.h"
#include "error.h"

#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

#define MAX_SFX      96
#define MAX_VOICES   24
#define OUT_CHANNELS 2
#define AUDOUT_BUFFERS 4

typedef struct {
  s16 *pcm;          /* interleaved stereo, owned */
  size_t frames;
  int used;
  char name[128];
} SfxSample;

typedef struct {
  int active;
  int sample;        /* index into sfx[], -1 for the music stream */
  int loop;
  float volume;
  size_t pos;        /* frame cursor into the sample */
  double frac;       /* reserved for resampling */
} Voice;

static SfxSample sfx[MAX_SFX];
static int sfx_count;

static Voice voices[MAX_VOICES];

/* music stream */
static stb_vorbis *bgm;
static int bgm_channels;
static int bgm_rate;
static s16 bgm_buf[4096 * 2];
static int bgm_buf_frames;
static int bgm_buf_pos;
static int bgm_playing;
static int bgm_loop = 1;
static float bgm_gain = 0.8f;
static float sfx_gain = 0.9f;

static AudioOutBuffer out_buffers[AUDOUT_BUFFERS];
static s16 *out_data[AUDOUT_BUFFERS];
static int out_buffer_bytes;
static int out_frames;
static Thread thread;
static volatile int running;
static Mutex lock;

/* ------------------------------------------------------------------------- */

static void decode_to_pcm(const char *path, s16 **out, size_t *frames, int *channels) {
  int ch = 0, rate = 0;
  short *pcm = NULL;
  int n = stb_vorbis_decode_filename(path, &ch, &rate, &pcm);
  if (n < 0 || !pcm) {
    *out = NULL; *frames = 0; *channels = 0;
    return;
  }
  *channels = ch;
  *frames = (size_t)n;
  /* audout and the mixer run at 48 kHz stereo: resample mono/other rates here
   * rather than carrying a rate converter through the mixer */
  if (ch == OUT_CHANNELS && rate == cfg.audio_freq) {
    *out = pcm;
    return;
  }
  const size_t out_n = (size_t)((double)n * cfg.audio_freq / (double)rate) + 1;
  s16 *dst = calloc(out_n * OUT_CHANNELS, sizeof(s16));
  if (!dst) { free(pcm); *out = NULL; *frames = 0; return; }
  for (size_t i = 0; i < out_n; i++) {
    const double src = (double)i * rate / cfg.audio_freq;
    const size_t s0 = (size_t)src;
    if (s0 >= (size_t)n) break;
    for (int c = 0; c < OUT_CHANNELS; c++) {
      const int sc = (ch == 1) ? 0 : (c < ch ? c : ch - 1);
      dst[i * OUT_CHANNELS + c] = pcm[s0 * ch + sc];
    }
  }
  free(pcm);
  *out = dst;
  *frames = out_n;
}

int audio_sfx_load(const char *path) {
  if (!path || !*path) return -1;
  mutexLock(&lock);
  for (int i = 0; i < sfx_count; i++) {
    if (sfx[i].used && !strcmp(sfx[i].name, path)) {
      mutexUnlock(&lock);
      return i;
    }
  }
  if (sfx_count >= MAX_SFX) {
    mutexUnlock(&lock);
    return -1;
  }
  s16 *pcm = NULL;
  size_t frames = 0;
  int ch = 0;
  decode_to_pcm(path, &pcm, &frames, &ch);
  if (!pcm) {
    debugPrintf("audio: could not decode %s\n", path);
    mutexUnlock(&lock);
    return -1;
  }
  const int id = sfx_count++;
  sfx[id].pcm = pcm;
  sfx[id].frames = frames;
  sfx[id].used = 1;
  snprintf(sfx[id].name, sizeof(sfx[id].name), "%s", path);
  mutexUnlock(&lock);
  debugPrintf("audio: loaded sfx %d '%s' (%u frames)\n", id, path, (u32)frames);
  return id;
}

static Voice *alloc_voice(void) {
  for (int i = 0; i < MAX_VOICES; i++)
    if (!voices[i].active)
      return &voices[i];
  return &voices[0];  /* steal the oldest */
}

void audio_sfx_play(int id, float volume, int loop) {
  if (id < 0 || id >= sfx_count || !sfx[id].used) return;
  mutexLock(&lock);
  Voice *v = alloc_voice();
  v->active = 1;
  v->sample = id;
  v->loop = loop;
  v->volume = volume;
  v->pos = 0;
  mutexUnlock(&lock);
}

void audio_sfx_stop(int id) {
  mutexLock(&lock);
  for (int i = 0; i < MAX_VOICES; i++)
    if (voices[i].active && (id < 0 || voices[i].sample == id))
      voices[i].active = 0;
  mutexUnlock(&lock);
}

void audio_sfx_stop_all(void) { audio_sfx_stop(-1); }

/* ------------------------------------------------------------------------- */

int audio_bgm_load(const char *path) {
  if (bgm) {
    stb_vorbis_close(bgm);
    bgm = NULL;
  }
  int err = 0;
  bgm = stb_vorbis_open_filename(path, &err, NULL);
  if (!bgm) {
    debugPrintf("audio: could not open BGM '%s' (err %d)\n", path, err);
    return -1;
  }
  stb_vorbis_info info = stb_vorbis_get_info(bgm);
  bgm_channels = info.channels;
  bgm_rate = info.sample_rate;
  bgm_buf_frames = 0;
  bgm_buf_pos = 0;
  debugPrintf("audio: BGM '%s' %d ch %d Hz\n", path, bgm_channels, bgm_rate);
  return 0;
}

void audio_bgm_play(int loop) {
  mutexLock(&lock);
  bgm_loop = loop;
  bgm_playing = 1;
  mutexUnlock(&lock);
}

void audio_bgm_pause(void) { bgm_playing = 0; }

void audio_bgm_stop(void) {
  bgm_playing = 0;
  bgm_buf_frames = 0;
  bgm_buf_pos = 0;
}

void audio_bgm_set_loop(int loop) { bgm_loop = loop; }

void audio_set_bgm_volume(float v) { bgm_gain = v < 0 ? 0 : (v > 1 ? 1 : v); }
void audio_set_sfx_volume(float v) { sfx_gain = v < 0 ? 0 : (v > 1 ? 1 : v); }
float audio_bgm_volume(void) { return bgm_gain; }
float audio_sfx_volume(void) { return sfx_gain; }

/* ------------------------------------------------------------------------- */
/* mixing                                                                    */
/* ------------------------------------------------------------------------- */

static int refill_bgm(void) {
  if (!bgm)
    return 0;
  const int want = 4096;
  int got = stb_vorbis_get_samples_short_interleaved(bgm, OUT_CHANNELS,
                                                     bgm_buf, want * OUT_CHANNELS);
  if (got <= 0) {
    if (bgm_loop) {
      stb_vorbis_seek_start(bgm);
      got = stb_vorbis_get_samples_short_interleaved(bgm, OUT_CHANNELS,
                                                     bgm_buf, want * OUT_CHANNELS);
    }
    if (got <= 0) {
      bgm_playing = 0;
      return 0;
    }
  }
  bgm_buf_frames = got;
  bgm_buf_pos = 0;
  return got;
}

static void mix(s16 *dst, int frames) {
  static int32_t acc[4096 * 2];

  if (frames > 4096) frames = 4096;
  memset(acc, 0, frames * OUT_CHANNELS * sizeof(int32_t));

  /* music */
  if (bgm_playing && bgm) {
    int done = 0;
    while (done < frames) {
      if (bgm_buf_pos >= bgm_buf_frames) {
        if (!refill_bgm())
          break;
      }
      const int n = (frames - done < bgm_buf_frames - bgm_buf_pos)
                        ? (frames - done)
                        : (bgm_buf_frames - bgm_buf_pos);
      for (int i = 0; i < n * OUT_CHANNELS; i++)
        acc[done * OUT_CHANNELS + i] +=
            (int32_t)((float)bgm_buf[bgm_buf_pos * OUT_CHANNELS + i] * bgm_gain);
      bgm_buf_pos += n;
      done += n;
    }
  }

  /* effects */
  for (int v = 0; v < MAX_VOICES; v++) {
    Voice *vo = &voices[v];
    if (!vo->active || vo->sample < 0)
      continue;
    const SfxSample *s = &sfx[vo->sample];
    if (!s->used) { vo->active = 0; continue; }
    for (int i = 0; i < frames; i++) {
      if (vo->pos >= s->frames) {
        if (vo->loop) {
          vo->pos = 0;
        } else {
          vo->active = 0;
          break;
        }
      }
      for (int c = 0; c < OUT_CHANNELS; c++)
        acc[i * OUT_CHANNELS + c] +=
            (int32_t)((float)s->pcm[vo->pos * OUT_CHANNELS + c] * vo->volume * sfx_gain);
      vo->pos++;
    }
  }

  for (int i = 0; i < frames * OUT_CHANNELS; i++) {
    int32_t v = acc[i];
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    dst[i] = (s16)v;
  }
}

/* ------------------------------------------------------------------------- */

static void audio_thread(void *arg) {
  (void)arg;
  while (running) {
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    /* the wait times out after a second, so this doubles as a heartbeat: it
     * keeps ticking while the main thread is inside the engine, which is how a
     * stall gets told apart from a crash when there is no debugger around */
    if (R_FAILED(audoutWaitPlayFinish(&released, &count, 1000000))) {
      static u64 last_beat;
      const u64 now = armGetSystemTick();
      if (now - last_beat >= armGetSystemTickFreq()) {
        last_beat = now;
        /* malloc probe: if the engine dies right after a malloc, knowing
         * whether the allocator still answers from another thread separates
         * "heap is broken" from "engine is stuck" in one run */
        void *probe = malloc(0x2000);
        if (probe) {
          ((char *)probe)[0] = 1;
          free(probe);
        }
        debugPrintf("audio: alive (malloc %s, jni %s)\n",
                    probe ? "ok" : "FAILED", jni_last_method());
      }
      continue;
    }
    if (!released || !count)
      continue;
    s16 *buf = (s16 *)released->buffer;
    mutexLock(&lock);
    mix(buf, out_frames);
    mutexUnlock(&lock);
    audoutAppendAudioOutBuffer(released);
  }
}

void audio_init(void) {
  mutexInit(&lock);
  if (R_FAILED(audoutInitialize()))
    fatal_error("audoutInitialize failed (audio service missing).");
  if (R_FAILED(audoutStartAudioOut()))
    fatal_error("audoutStartAudioOut failed.");

  out_frames = cfg.audio_latency;
  if (out_frames < 256) out_frames = 256;
  out_buffer_bytes = out_frames * OUT_CHANNELS * sizeof(s16);

  for (int i = 0; i < AUDOUT_BUFFERS; i++) {
    out_data[i] = memalign(0x1000, out_buffer_bytes);
    if (!out_data[i])
      fatal_error("out of memory for audio buffers.");
    memset(out_data[i], 0, out_buffer_bytes);
    out_buffers[i].next = NULL;
    out_buffers[i].buffer = out_data[i];
    out_buffers[i].buffer_size = out_buffer_bytes;
    out_buffers[i].data_size = out_buffer_bytes;
    audoutAppendAudioOutBuffer(&out_buffers[i]);
  }

  running = 1;
  threadCreate(&thread, audio_thread, NULL, NULL, 0x8000, 0x1C, -2);
  threadStart(&thread);

  sfx_gain = cfg.sfx_volume / 100.0f;
  bgm_gain = cfg.music_volume / 100.0f;
  debugPrintf("audio: audout %d Hz, %d frames/buffer, %d buffers\n",
              cfg.audio_freq, out_frames, AUDOUT_BUFFERS);
}

void audio_deinit(void) {
  running = 0;
  threadWaitForExit(&thread);
  threadClose(&thread);
  audoutStopAudioOut();
  for (int i = 0; i < AUDOUT_BUFFERS; i++)
    free(out_data[i]);
  audoutExit();
  for (int i = 0; i < sfx_count; i++)
    free(sfx[i].pcm);
  if (bgm)
    stb_vorbis_close(bgm);
}

void audio_pump(void) {
  /* the mixer thread owns the buffer rotation; nothing to do per frame */
}
