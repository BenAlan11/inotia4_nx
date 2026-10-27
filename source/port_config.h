/* port_config.h -- Inotia 4 (Com2uS) for android32.
 *
 * Macros only: the assembler and the launcher read this file too.
 * The APK this targets: com.com2us.inotia4.normal.freefull.google.global.android.common
 * versionName 1.3.7 / versionCode 14, the armeabi-v7a libraries.
 */
#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

#define PORT_TITLE   "Inotia 4: Assassin of Berkel"
#define PORT_NAME    "inotia4_nx"
#define PORT_PACKAGE "com.com2us.inotia4.normal.freefull.google.global.android.common"

#define PORT_BANNER  "inotia4_nx: Inotia 4 (Com2uS GEM_LIB engine, armeabi-v7a)"

/* The engine reads its resources from
 * /sdcard/Android/data/<package>/files/resources/... -- see RT_PATH_* below. */
#define PORT_ABI_DIR "lib/armeabi-v7a/"

/* libgame.so maps to ~1.71 MiB; libHercules.so (the Hive SDK) is never loaded. */
#define PORT_SO_REGION_BYTES (8u * 1024 * 1024)

#define PORT_APK_DESC "Inotia 4 1.3.7 (armeabi-v7a)"
#define PORT_APK_ROLES                                                              \
  {.what = "the game",                                                              \
   .need = (const char *const[]){"lib/armeabi-v7a/libgame.so", NULL},               \
   .flags = RT_APK_HIGHEST_VERSION}
/* Another version of the game would move the engine's offsets: refuse it. */
#define RT_PACKAGE_MISMATCH_FATAL 1

#define PORT_LAUNCHER_START_NOTE "(the first start unpacks libgame.so and its assets from your APK)"
#define PORT_LAUNCHER_BYLINE     "Unofficial homebrew wrapper; bring your own APK"

/* The engine builds its resource paths from GetPackageName() and mkdir()s as it
 * goes, but the base chain has to exist before it asks. */
#define RT_PATH_SD_SUBDIRS "Android"
#define RT_PATH_EXTRA_DIRS                                                          \
  "Android",                                                                        \
  "Android/data",                                                                   \
  "Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common",    \
  "Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files", \
  "Android/data/com.com2us.inotia4.normal.freefull.google.global.android.common/files/resources"

/* The engine does not touch OpenSL ES: every sound goes through the Java
 * SoundPool/MediaPlayer path that inotia_audio.c implements. 0 = slCreateEngine
 * refuses, which is what we want. */
#define RT_OPENSLES 0

#define RT_PAD_MAX_PLAYERS 1
#define RT_SETUP_UPDATE_PERMILLE 500

#endif
