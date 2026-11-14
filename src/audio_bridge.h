#ifndef AUDIO_BRIDGE_H
#define AUDIO_BRIDGE_H

#include <stdint.h>

/* The engine's own C audio API (CS_mdaSound*) is overridden in main.c before
 * so_resolve() so that neither the fake JNIEnv nor its varargs/jvalue mismatch
 * is on the playback path. Signatures follow the original ARM64 ABI: sound
 * handles are 64-bit slots, volumes are 0..100.
 *
 * 0 == success, -9 == "sound system unavailable" (the engine's own error
 * convention, see CS_mdaSoundPlay's `mov x8, #-9`). */
void audio_bridge_init(void);

int64_t nx_mda_sound_create(int64_t *out, const char *name,
                            int64_t a2, int64_t a3, int flag);
int64_t nx_mda_sound_prepare(int64_t id);
int64_t nx_mda_sound_play(int64_t id, int loop);
int64_t nx_mda_sound_stop(int64_t id);
int64_t nx_mda_sound_destroy(int64_t id);
int64_t nx_mda_sound_pause(int64_t id);
int64_t nx_mda_sound_resume(int64_t id);
int64_t nx_mda_sound_set_volume(int64_t id, int vol);
int64_t nx_mda_sound_get_volume(int64_t id);
int64_t nx_mda_stop_all(void);

#endif
