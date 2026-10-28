# inotia4_nx — Inotia 4 on the Nintendo Switch

An unofficial wrapper that runs the original 32-bit Android build of **Inotia 4: Assassin of Berkel** (Com2uS) on Atmosphère, through [`aks796/android32`](https://github.com/aks796/android32) — the shared AArch32 runtime behind 14 other Android-game ports.

Not an emulator: `libgame.so` from your own APK runs natively in a 32-bit Horizon process. No game code, assets or saves are in this repository.

| | |
|---|---|
| Package | `com.com2us.inotia4.normal.freefull.google.global.android.common` |
| Target version | **1.3.7 / versionCode 14**, `armeabi-v7a` |
| Engine | Com2uS in-house C engine (`GEM_LIB`, `Ino3_*`) behind a thin Java wrapper (`com.com2us.wrapper.*`) |
| Graphics | OpenGL ES **1.1** fixed function → Mesa's `libGLESv1_CM` on nouveau |
| Audio | no OpenSL ES in the engine: Java `SoundPool`/`MediaPlayer` → `stb_vorbis` + `audout` (this port) |
| Status | **stage 1a** — loads, relocates and resolves the engine; the entry sequence is the current work |

## What is already done

- `source/imports.c` — generated with the runtime's own `gen_imports.py`: **171 imports, 0 unresolved** (58 `gl*` go to the GL layer). Re-check with `./tools/verify.sh`.
- `source/inotia_bionic.c` — the 9 bionic `_FORTIFY` shims the runtime does not have (`__strcpy_chk`, `__open_2`, `__read_chk`, …). The other 4 gaps (`atof atoll rand srand`) are `PASSTHROUGH` in `tools/imports.cfg`.
- `source/inotia_boot.c` — `port_load` / `port_run`, the GLES1 context, the entry sequence, the frame loop, and the `assets/common/**` unpack step.
- `source/inotia_config.c` — `config.ini` (resolution, CPU boost, `log_java_calls`, …).
- `source/inotia_jni.c` — the fake Java VM's answers: `Context` paths and package name, `DisplayMetrics`, `File`, `MotionEvent`, `SoundPool`, `MediaPlayer`, plus wildcards for the wrapper classes so an unknown call returns 0 instead of faulting.
- `source/inotia_audio.c` — an 8-voice `stb_vorbis` mixer on `rt_audout` (the game's `.ogg` files are stored as `.jpg` in the APK; they decode as plain Ogg).
- `source/inotia_input.c` — touchscreen passthrough plus a stick-driven virtual finger for docked play.
- `source/inotia_font.c` — a stub; see "the two real risks" below.
- `launcher/icon.jpg` — **the game's own launcher icon**, taken from the APK (`res/I8.png`, 192×192) and upscaled to the 256×256 JPEG the NRO's NACP carries. That is the icon the HOME menu shows for the forwarder.

## Building

You need Docker Desktop, and the game's APK. Nothing else.

### 1. The runtime

`runtime/` is already in this tree (a copy of `aks796/android32` @ main, MIT). To track it instead:

```sh
rm -rf runtime
git submodule add https://github.com/aks796/android32 runtime
```

`./tools/verify.sh` checks the tree without the toolchain: the import table, the symbols the runtime requires from a port, and that no file names another game.

### 2. `libnx32` and Mesa — already staged, nothing to build

Both are published prebuilt (neither is in devkitPro's packages), and both are **already in this tree**:

```
third_party/libnx32/   libnx32 4.12.0  — lib/libnx.a, include/switch.h, switch32.*
portlibs32/            Mesa 26.2      — libEGL.a, libGLESv1_CM.a, libnouveau_horizon.a, …
```

All ten libraries `runtime.mk`'s Mesa 26 branch links are present (`-lEGL -lGLESv2 -lGLESv1_CM -lglapi -lmesa_util_c11 -lblake3 -lmesa_util -lmesa_util_simd -lexpat -lz`), and `libnouveau_horizon.a` is what makes it pick 26 instead of 20.

To refresh them (e.g. after a new release):

```powershell
.\tools\get-deps.ps1        # Windows: downloads through api.github.com, lays both out
```

```sh
# POSIX
unzip libnx32.zip       -d third_party/libnx32
unzip mesa-switch32.zip -d portlibs32
```

> **Use mesa-switch32 (Mesa 26.2), not mesa32 (20.1).** In 26.2 GLES 1 is its own library with weak symbols and the runtime links `-lGLESv1_CM` next to `-lGLESv2`. In 20.1 the two define the same symbols and only `-lGLESv2` is linked, which this game cannot use — it is a pure GLES 1 title.

Windows notes, the error table and why a native devkitPro install does not help: [`docs/构建环境.md`](docs/构建环境.md).

### 3. The two programs

```sh
cd inotia4_nx
./build.sh                 # -> inotia4_nx.nsp + inotia4_nx.build  (the 32-bit program)
cd launcher
./build.sh                 # -> inotia4_nx.nro                     (the 64-bit launcher)
```

`./build.sh rt-files` prints what is built from where if the link complains.

### 4. Or let GitHub build it

`.github/workflows/build.yml` does all of the above on a GitHub runner — no local toolchain at all. It fetches the runtime and the two prebuilt dependencies, builds both programs, and uploads them as one artifact:

```
Actions → build → the newest run → Artifacts → inotia4_nx
  inotia4_nx.nsp   the 32-bit game program
  inotia4_nx.nro   the 64-bit launcher
  inotia4_nx.build
```

Push to `main` (or run the workflow by hand) and download that zip. Nothing that is not source is committed: the game's APK never leaves your SD card.

## Installing

```
sd:/switch/inotia4_nx/
├── inotia4_nx.nro
├── Inotia4.apk            <- your own 1.3.7 APK, any name
└── (created on first run: libgame.so, Android/, config.ini, save/, debug.log)
```

1. Copy the folder above.
2. In **sphaira**, find *Inotia 4* under Homebrew → **Install Forwarder**.
3. Launch the icon on the HOME menu. The launcher unpacks `libgame.so` and `assets/common/**` out of your APK on the first start (with a progress bar), then restarts into the wrapper.

## What to do when it does not work

Everything is in `sd:/switch/inotia4_nx/debug.log`. Set these in `config.ini` for a first bring-up:

```ini
[debug]
log_java_calls=1
boot_log=1
gl_selftest=1
```

The lines to look for, in order:

| In the log | Means |
|---|---|
| `[load] mapped … unresolved 0` | the loader is fine; the engine is in memory |
| `[game] JNIEnv …` | the fake Java VM started |
| `[game] nativeSetClass(…)` | the wrapper classes were handed over |
| `[game] nativeStart returned` | the engine's entry ran |
| `[game] first frame presented` | the frame loop is alive |
| `unhandled` / `missing` lines | the engine asked for a Java method nothing answers — that is the next thing to add |

Send `debug.log` back with the last thing it printed.

## The Java side, as the dex has it

The engine is a C core behind a thin Java layer (`com.com2us.wrapper.*`), and the
Java layer — not the core — decides the order the natives are called in. That
order is readable straight out of the APK, and `source/inotia_boot.c` follows it:

```
CWrapper.<clinit>              CWrapper.nativeSetClass()          static
CWrapper.<init>                mUseNativeCallback = true;
                               onCreate(getClass().getName(), true)
CWrapper.onCreate(String,Z)    nativeInitialize(moduleName)       instance
CWrapperData.<init>            nativeInitialize(TimeZone.getRawOffset())
CWrapperKernel$1$1.run (GL)    state == INITIAL ? nativeStart() : nativeSurfaceRecreated()
```

`nativeInitialize` is a **dispatcher**, not a lifecycle call: it looks the module
name up and runs that module's native init, which caches the `jclass` and the
`jmethodID`s the core later calls back through. The twelve names it accepts are
the dotted literals in `libgame.so` (`CWrapperKernel` and `CWrapperData` are not
among them — their natives are called directly).

Two tools read this out, and both are worth knowing about:

```sh
python3 tools/dex_natives.py  game.apk            # every native prototype
python3 tools/dex_class.py    game.apk com.com2us.wrapper.kernel.CWrapper
python3 tools/dex_grep.py     game.apk com.com2us.wrapper.kernel.CWrapperKernel nativeStart
```

`dex_class.py` decodes the bytecode (registers and all) and `dex_grep.py` finds
call sites across every class, which is what turns "the engine asked for
something" into "the engine asked for this, here, with these arguments".

## The stage plan

| Stage | Goal | Gate |
|---|---|---|
| 0 | APK recognised, `libgame.so` unpacked, loaded, imports bound | `unresolved 0` in the log |
| 1a | the entry sequence runs without faulting | `nativeStart returned` |
| 1b | GLES1 first frame | `first frame presented` |
| 1c | assets found at the engine's paths | title screen |
| 1d | input bridge | can reach the menu |
| 1e | audio | BGM and effects |
| 1f | **font** | text is not blank |
| 1g | saves | progress survives a restart |
| 1h | performance | stable 60 FPS at 720p |

### The two real risks

1. **Fonts.** `libgame.so` imports no FreeType and no Skia: it asks Java for glyphs through `CFont` (`nativeCreateFromTypeface`). Either hook the engine's own `CS_font*` entry and rasterise with `stb_truetype`, or implement `Typeface`/`Paint`/`Canvas`/`Bitmap` in the JNI table. Stage 1b will show which one is needed — look for `[font] glyph request` in the log.
2. **The Hive login screen.** The engine carries Com2uS's Hive SDK (`libHercules.so`, `ActiveUser.*`, `InAppV4.*`, `Mercury.*`). This port never loads `libHercules.so` and answers those 32 JNI entries with zero, so the offline path has to be reachable. If the game parks on a login page, that branch needs a hook.

## Legal

The port ships no APK, no `libgame.so`, no assets and no saves. You must own a copy of the game. "Inotia" and related marks belong to Com2uS. `android32`, `libnx32` and Mesa are MIT; `stb_vorbis` is public domain.
