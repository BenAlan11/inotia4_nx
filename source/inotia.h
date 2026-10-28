/* inotia.h -- what the port's own files share. MIT. */
#ifndef INOTIA_H
#define INOTIA_H

#include <stdint.h>

#include "jni.h"
#include "so_util.h"

/* The engine (libgame.so), loaded by port_load. */
extern so_module g_game;

/* The Java object the engine sees as its activity: it answers
 * Context.getPackageName() and the file paths the engine builds from it. */
JObj *inotia_activity(void);

/* The classes the engine is handed through CWrapper.nativeSetClass. NULL for
 * one it did not ask for. */
JObj *inotia_wrapper_class(const char *short_name);

/* The APK the game was loaded from, as the engine's Java layer asks for it
 * (CFunction.getApkFilePath). */
const char *inotia_apk_path(void);

/* ---------------------------------------------------------------- audio */
/* The mixer behind android/media/SoundPool and MediaPlayer: stb_vorbis
 * streams on rt_audout. Returns 0 when the pump started. */
int inotia_audio_init(void);
void inotia_audio_shutdown(void);
void inotia_audio_pause(int paused);

/* SoundPool */
int inotia_sound_load(const char *path);                 /* -> sound id, or 0 */
int inotia_sound_play(int id, float volume, int loop);   /* -> stream id, or 0 */
void inotia_sound_stop(int stream);
void inotia_sound_set_volume(int stream, float volume);

/* MediaPlayer, one instance per object (the object's payload holds the state) */
int inotia_media_open(JObj *self, const char *path);
void inotia_media_start(JObj *self);
void inotia_media_pause(JObj *self);
void inotia_media_stop(JObj *self);
void inotia_media_release(JObj *self);
void inotia_media_seek(JObj *self, int ms);
int inotia_media_position(JObj *self);
int inotia_media_duration(JObj *self);
int inotia_media_is_playing(JObj *self);
void inotia_media_set_volume(JObj *self, float volume);
void inotia_media_set_looping(JObj *self, int loop);

/* ---------------------------------------------------------------- input */
/* One frame of Switch input turned into the touches the engine polls through
 * CS_knlGetTouchCount / CUserInput. */
void inotia_input_init(void);
void inotia_input_frame(void);
int inotia_touch_count(void);
int inotia_touch_x(int index);
int inotia_touch_y(int index);
int inotia_touch_down(int index);

/* ---------------------------------------------------------------- font */
/* The engine asks Java for glyphs (CFont). Until that path is implemented
 * (stage 1f), these answer with an empty bitmap so the engine keeps running. */
JObj *inotia_font_bitmap(const char *utf8, int px, int *w, int *h);

#endif
