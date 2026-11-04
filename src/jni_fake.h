#ifndef JNI_FAKE_H
#define JNI_FAKE_H

#include <stdint.h>

/* The game's native code talks to its Java shell through a JNIEnv it is handed
 * at JNI_OnLoad time. We hand it a table whose slots answer by name. */
extern void *fake_env;
extern void *fake_vm;

void jni_init(void);

/* name of the most recent Java method the engine called (for crash forensics) */
const char *jni_last_method(void);

/* bring-up counters: every Java call and every fake object we hand back */
unsigned long jni_call_count(void);
unsigned long jni_obj_count(void);

/* number of requestRender() calls since the last call, then resets it */
int jni_take_render_request(void);

/* The engine reads config/HUB_LOGIN_INFO immediately before it decides to show
 * the "log in to Hive?" prompt. main.c uses that as the fingerprint for which
 * prompt it is looking at, because the prompt text itself is not a C string.
 * Returns 1 once after such a read, then clears. */
int jni_take_hub_probe(void);
void jni_note_hub(void);

/* log the next n Java calls verbatim (dedupe defeats a crash trace otherwise) */
void jni_trace(int n);

/* deliver CWrapperHttp.nativeConnectCB through this once a request is answered */
void jni_set_http_hook(void (*fn)(int handle, int result));

void *jni_make_object(const char *label);
void *jni_make_string(const char *utf);
void *jni_make_fd(int fd);
void *jni_make_byte_array(const void *data, int len);

/* Pumped once per frame: anything the engine asked Java to do asynchronously
 * (text input, dialogs) is serviced here. */
void jni_update(void);

/* Set by main.c so the Java-side audio sinks can reach the port's mixer. */
typedef void (*JniSoundPlayFn)(int id, float volume, int loop);
typedef void (*JniSoundStopFn)(int id);
typedef void (*JniSoundVolumeFn)(int id, float volume);
void jni_set_audio_hooks(JniSoundPlayFn play, JniSoundStopFn stop,
                         JniSoundVolumeFn volume);

#endif
