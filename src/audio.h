#ifndef AUDIO_H
#define AUDIO_H

/* The game's audio lives on the Java side: a SoundPool for short effects and a
 * MediaPlayer for the looping BGM tracks. Both write to the same mixer here. */

void audio_init(void);
void audio_deinit(void);

/* mixer thread: pumps ~(buffer) frames when the audout ring has room */
void audio_pump(void);

/* --- short effects (CSoundPool semantics: preload, many instances) ------- */
int  audio_sfx_load(const char *path);          /* returns a slot id, -1 on error */
void audio_sfx_play(int id, float volume, int loop);
void audio_sfx_stop(int id);
void audio_sfx_stop_all(void);

/* --- music (CMediaPlayer semantics: one stream, looping) ---------------- */
int  audio_bgm_load(const char *path);
void audio_bgm_play(int loop);
void audio_bgm_pause(void);
void audio_bgm_stop(void);
void audio_bgm_set_loop(int loop);

void  audio_set_bgm_volume(float v);            /* 0.0 .. 1.0 */
void  audio_set_sfx_volume(float v);
float audio_bgm_volume(void);
float audio_sfx_volume(void);

#endif
