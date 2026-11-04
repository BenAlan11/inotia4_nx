# Inotia 4 for Nintendo Switch

A source port that runs the **original, unmodified** `libgame.so` from the
Android release of *Inotia 4: Assassin of the Night* on Switch homebrew.

Nothing is recompiled from Com2uS's code and no assets are converted: the port
is a loader plus a fake Android/Java shell. The game binary is mapped into
memory as executable code, its 231 dynamic imports are answered by this
project's own implementations, and the 37 `com.com2us.wrapper.*` Java classes
it calls are emulated in-process. Rendering goes through the same ES 1.1
fixed-function pipeline the game used on Android.

---

## What you need

| | |
|---|---|
| Console | Any Switch that can run homebrew (Atmosphère / hbmenu) |
| SD card | ~61 MB free, FAT32 or exFAT |
| `Inotia4.apk` | 49 MB, the arm64-v8a build (contains `lib/arm64-v8a/libgame.so`) |
| Build host | Python 3.9+, ~600 MB of devkitPro packages (fetched by the tools) |

The port needs the code-memory syscalls, so **launch it with a game override
(hold R while starting it from the album) or install a forwarder NSP.** A plain
hbmenu launch in applet mode will stop at a clear on-screen error instead of
crashing.

---

## Building

```sh
python build.py             # -> build/inotia4_nx.nro
python build.py --clean     # full rebuild
```

`build.py` compiles everything with `zig cc` (clang, AArch64) and links with
`zig ld.lld` against devkitPro's newlib, libnx and the mesa/swrast GLES1
portlibs. There is no devkitPro installation to perform — the toolchain
packages live under `tools/dkp_rootfs/`. `tools/dkp_pull.py` is what put them
there: it walks a registry mirror's manifest and extracts only
`opt/devkitpro` from the image layers, so no Docker (and no devkitPro
installer, which needs Linux and refuses to run headless) is involved.

Two non-obvious pieces of build plumbing worth knowing about:

* `switch.ld` ships without a `PT_TLS` segment, so lld refuses objects that
  contain `STT_TLS` symbols. `patch_linker_script()` injects the segment and
  folds `.tdata`/`.tbss` into it while keeping them in the RW load segment.
* `libharfbuzz.a` is C++ and pulls in the whole libstdc++ runtime, so
  `-lstdc++ -lsupc++` sit inside the same `--start-group` as libc.

## Packaging the SD card

```sh
python tools/pack_data.py --out D:/        # writes D:/switch/inotia4/
```

This reads the APK straight from `~/Downloads` by default (`--apk` to point
elsewhere) and produces:

```
switch/inotia4/
├─ inotia4_nx.nro          the port
├─ libgame.so              the original game binary, untouched
├─ libHercules.so          its companion library (integrity / i18n)
├─ config.ini              generated on first run, see below
├─ data/
│  ├─ resources/common/game_res/   732 game assets
│  └─ res/H7.mp4                   intro movie
├─ save/                   the engine writes save%d.dat here
└─ debug.log               written on every run
```

### About the asset names

Every file inside the APK's `assets/` tree carries a bogus `.jpg` tail:
`game.dat` ships as `game.dat.jpg`, `font.fnt` as `font.fnt.jpg`, a PNG texture
as `UI_003.png.jpg`, and the Ogg Vorbis BGM tracks as `BGM05.jpg`. `libgame.so`
contains no `"jpg"` literal anywhere, so the engine asks for the *real* names and
the tail is purely asset-ripping camouflage. The packer strips it.

Files that would end up with no extension at all — the sound banks fed to
`SOUND/BGM/%s` and `SOUND/SE/%s` — are also written a second time under the
original masked name, because the index that supplies those names lives inside
`game.dat` and has not been fully reversed yet. That costs 14 MB and removes a
class of "music is silent" failures.

`texture.store`, `appinfo.dat`, `envinfo.dat`, `filetext.dat`, `i_pack.dat` and
`thpcheckslot*.dat` are **not** in the APK: the engine creates them on first run
as caches and slot metadata.

---

## Configuration

`config.ini` is written next to the NRO on first run and re-read on every
start; a missing file just means defaults.

```ini
[display]
screen_width = 0        ; 0 = auto (1280x720 handheld)
screen_height = 0
dock_1080p = 1          ; 1920x1080 in TV mode; 0 if the frame rate suffers

[game]
language = 0            ; 0 = follow the console
skip_intro = 1          ; no h264 decoder, so the movie can only be skipped

[audio]
audio_frequency = 48000
audio_latency = 1024    ; samples per audout buffer; raise if music stutters
music_volume = 80
sfx_volume = 90

[input]
inject = 0              ; see "Controls" below
touch_x_pct = 12
touch_y_pct = 78
button_pct = 18

[debug]
log_level = 1
show_fps = 0
string_draw = 1         ; fill the engine's glyph atlas with FreeType text
                        ; (prologue / tutorial hint / dialog bodies)
```

`string_draw = 0` turns the atlas fill off and puts the "black bar, no text"
behaviour back, which is the quickest A/B when a text bug is suspected.

---

## Controls

The game is **touch-only** — there is no keypad code in `libgame.so` at all.
Every button is drawn by the engine and read through its pointer path, so the
pad drives a virtual finger instead of pretending to be a controller.

There are two modes, both shown on screen so the player always knows where a
press will land:

**Focus mode** (default) — a yellow box walks a list of menu targets:

| Input | Effect |
|---|---|
| Left stick ←/→, D-pad ←/→, L/R | move the box between SETTINGS / START / MORE GAMES |
| A or ZR | tap the boxed button |
| Y | switch to free cursor |

**Free cursor** — a crosshair the right stick aims, for anything not in the
list (in-game menus, dragging an inventory, the map):

| Input | Effect |
|---|---|
| Right stick | move the crosshair |
| A or ZR | press where it is; keep holding to drag |
| B | release |
| Y | back to focus mode |

| Touchscreen | passed straight through, one finger per contact |

The focus list lives in `src/input.c` (`focus[]`, percentages of the view) if
the title screen ever moves; the box is drawn with scissored clears, so it
needs no texture and costs a few hundred pixels of fill per frame.

**`inject = 0` is the default and only logs.** The engine's injection points
(`InputQueue_CreateEventItem` and `InputQueue_Put`) are located and bound, but
the event codes and the payload field order are inferred from disassembly
rather than confirmed on hardware, and a wrong guess corrupts the engine's
event list instead of failing cleanly. Set `inject = 1` in `config.ini` to
enable the mirroring and watch `debug.log`; the touchscreen path works
regardless.

---

## How it works

```
main.c            the Android boot sequence, then nativeProcess + eglSwap at
                  60 Hz -- this is what MainActivity/CRenderer did
 ├ so_util.c      maps libgame.so as executable code (svcMapProcessCodeMemory,
 │                RELA + RELR, cross-module resolution for libHercules)
 ├ imports.c      231 symbol implementations, generated from the .so's own
 │                undefined-symbol table by tools/gen_imports.py
 ├ libc_shim.c    bionic compatibility: __sF, __errno, fortify wrappers,
 │                __system_property_get, getauxval, statfs, and the path
 │                rewriting that moves /sdcard/Android/data/<pkg>/files/...
 │                onto the SD card
 ├ jni_fake.c     233-slot JNIEnv table plus the 37 fake com.com2us.wrapper
 │                classes. Arguments are decoded from the method signature, not
 │                read positionally, because the engine passes varargs
 ├ egl_helper.c   ES 1.1 context through mesa/swrast + nouveau
 ├ audio.c        stb_vorbis decode -> software mixer -> audout
 ├ text.c         FreeType text for the Java-side strings only (the engine
 │                draws its own UI with its bundled font.fnt). The glyph source
 │                is the console's own shared font, so no TTF is needed and CJK
 │                renders out of the box
 └ input.c        pad -> synthetic pointer events, touchscreen passthrough
```

Three details that were the difference between "black screen" and a running
game:

1. **TLS.** bionic reads its stack canary from `TPIDR_EL0 + 0x28`; libnx keeps
   the thread pointer in `TPIDRRO_EL0` and leaves `TPIDR_EL0` at zero. Before
   any engine code — including `.init_array` — runs, `main` points `TPIDR_EL0`
   at a zeroed block.
2. **JNI table indices.** The engine indexes the table by byte offset
   (`slot * 8`), so a single off-by-one calls an unrelated function. The table
   follows the JNI specification exactly and was checked against the binary
   (`NewGlobalRef` at 21, `GetStaticMethodID` at 113).
3. **RX before RW.** The kernel forbids mapping W then X. `so_finalize` maps
   each segment twice: RX first, then RW over the same pages.

---

## Status

Running end to end on both hardware and Eden: title screen, save-slot page,
character select, and the first map all come up and are playable. What is left
is text — see [Where the work stands](#where-the-work-stands-2026-11-03) for the
exact list and the next step for each.

| Area | State |
|---|---|
| ELF loading, relocation, init_array | implemented, RELA + RELR + Android RELR |
| 231 imports / 37 fake classes | implemented |
| GLES1 fixed pipeline | all 58 `gl*` entry points present, incl. the 16 fixed-point variants (`glOrthox`, `glRotatex`, `glTexEnvx`, ...) |
| Audio (BGM + SE) | ogg decode and mixing implemented, `audout` output |
| Save data | via the engine's own `save%d.dat`, with the slot key pinned (see below) |
| Pad input | mapped, injection gated behind `inject` |
| Intro movie | skipped — no h264 decoder in the toolchain |

### Not done

* **Hardware bring-up.** Expect the first run to land in `debug.log` before it
  renders; the boot log is written step by step for exactly this reason.
* **`texture.store` / `i_pack.dat` layout.** The engine creates them, but it is
  unconfirmed that a fresh creation produces a complete texture cache rather
  than waiting for a download step. If textures come out missing, that is the
  first place to look.
* **Sound name spelling.** See the packer note above.
* **Docked 1080p performance.** mesa/swrast is fill-rate bound; `dock_1080p = 0`
  is the escape hatch.

---

## Where the work stands (2026-11-03)

Everything below is about text. The 3D, the UI plates, the input and the audio
are fine; the remaining defects are all "the rectangle is there and the glyphs
are not".

### Verified working

| Screen | Note |
|---|---|
| Title menu | engine font path (`nx_grp_draw_string` family) |
| Save-slot page | 9-slice panel + `连接出错。请稍后重试。` + `确认` |
| Character info page | alignment fixed; `align_x()` maps the engine's 0/1/2 to left/right/centre |
| In-game hint bar | **string-image path**, white glyphs on the engine's dark plate |

### Open

1. **The NPC name plate has no text.** The plate sits *above* the hint bar. It
   is **not** the string-image path: standing next to an NPC produced zero
   `SGL_grpCreateStringImageInRect` calls over a whole session (the only three
   in the log belong to dialogs). So the name comes from one of the 15 drawers
   that `text_trace` watches. `README` section *Driving a run unattended* below
   says how to catch it.
2. **The picture twitches when an NPC is approached.** Suspected to be the same
   missing-text path retrying every frame, but not yet reproduced under
   instrumentation, so unconfirmed.
3. **The new-game opening black phase has no text.** During the `state = 5`
   phase (`prev = 4`) that follows character select, no traced text entry point
   fires and there are no JNI movie calls. `skip_intro` is parsed in
   `config.c` but read by nothing, so the phase is engine-side and its text
   source is still unidentified.
4. **Emphasis colour is lost.** In the original, `每日奖励` inside the hint line
   is blue. The string the engine hands to `SGL_grpCreateStringImageInRect` has
   already had its `$`-codes stripped (`点击左侧闪烁的收费商店按钮，可以获得每日奖励。`),
   so the fill cannot see which span is emphasised. Two candidate answers:
   the engine draws the same atlas several times with different UV ranges and
   colours, or the codes are recoverable earlier in its layout code. The
   `atlas-draw` probe currently requires `count == 4`, which would hide extra
   quads — widen it before deciding.

### How the string-image text works

`SGL_grpCreateStringImageInRect(max_width, str, len, font, enc)` rasterises a
string into a `GL_ALPHA` atlas which the engine then draws as a single quad.
The port no-ops the rasteriser inside it (`SGL_RenderFontToAlphaImageInRect`
takes a byte count, not a rectangle), so the atlas is all zeroes: the plate is
drawn, the quad is drawn, and the glyphs are not. That is the "black bar, no
text" symptom.

The fix keeps the engine in charge of position, size and colour and only fills
the atlas:

| Piece | Where |
|---|---|
| wraps the three create entry points | `grp_csir_common()`, `main.c:2875` |
| promotes a small `A8` alloc to `GL_RGBA` and remembers it | `nx_glTexImage2D`, `main.c:3454` |
| renders the string with FreeType, caches by `(tex,w,h,hash)`, uploads it | `strimg_fill()`, `main.c:2822` |
| lifts a near-black current colour to white for a filled atlas | `nx_glDrawArrays`, `main.c:3176` |
| the renderer itself (white RGB, coverage in alpha) | `text_render_rgba()`, `text.c:419` |

The lift is needed because the engine draws that quad with
`glColor(0, 0, 0, 0.6)` — black at 60 % — which multiplies the glyphs away. The
previous colour is restored straight after the draw.

A healthy run logs this sequence exactly once per string (the engine's own
cache answers the rest, so `touched` drops to 0):

```
text: csir w=937 len=-1 -> tex 87 (512x32) touched 1 str '点击左侧闪烁的收费商店按钮，可以获得每日奖励。'
gl: atlas-draw tex 87 (80,152)-(1057,192) col 0.00 0.00 0.00 0.60 blend 1 colarr 0 mode 0x6
gl: lifted near-black text tint to white (alpha 0.60)
```

Two traps worth remembering from getting here:

* Widening the `A8`/565 uploads unconditionally broke the whole 3D view —
  Mesa rejects an RGBA upload into a `GL_RGB` texture with `0x502` and silently
  keeps the old contents. Every widening is now gated on the texture's real
  internal format, tracked by hand in `g_texfmt` (ES1 has no
  `glGetTexLevelParameteriv`).
* A first attempt replaced the atlas quad with a `text_draw_engine()` call and
  fell back to "whatever texture is bound" on a cache hit, which painted the
  virtual buttons over with 30 px text. The quad-replacement path is gone; only
  the fill-the-engine's-own-atlas form survives.

### Driving a run unattended

`config.ini` has everything needed to play the game without a controller:

```ini
[debug]
tap_script        = "960,919@600; 959,279@1200; 640,360@2400"
shot_every_frames = 30      ; writes r%03d.ppm next to the NRO (6 MB each!)
gl_probe          = 1       ; atlas-draw / per-frame GL state
text_trace        = 1       ; logs the 15 text entry points we do not replace
```

`tap_script` coordinates are **framebuffer** pixels, so with `dock_1080p = 1`
(1920x1080) the title's `开始游戏` is `960,919` and a save slot is `959,279`;
with `dock_1080p = 0` (1280x720) they halve to `640,613` / `639,186`. Each tap
presses for 8 frames, so leave at least ~40 frames between them.

Run it headless:

```sh
taskkill //F //IM eden-cli.exe          # otherwise the copy fails with WinError 32
python deploy.py --eden --no-build --no-pack
eden-cli.exe -g "%APPDATA%\eden\sdmc\switch\inotia4\inotia4_nx.nro"
```

**Pitfall:** careless taps land on `是否删除已有的存档，重新开始游戏？`. Back up
`save/` before scripting a run — only `save2.dat` matters, everything else in
that folder is a cache the engine regenerates. `save_backup/` next to `save/`
holds a copy.

**Always put the production config back afterwards.** The shipped one is
`log_level = 1`, `shot_every_frames = 0`, no `tap_script`, no `gl_probe`, no
`text_trace`, `dock_1080p = 1`. A pristine copy lives in `config.prod.ini`:

```sh
cp config.prod.ini dist/switch/inotia4/config.ini && python deploy.py --eden --no-build --no-pack
```

### Next steps

1. `text_trace = 1`, walk to an NPC, and read the
   `text: <entry>(...) str@xN '...'` lines. Whatever fires is the name plate's
   drawer; implement it the way `nx_grp_draw_string` is implemented.
2. Widen the `atlas-draw` probe to log any vertex count, then look for extra
   quads with a non-black colour over the hint bar. That decides how the blue
   emphasis has to be reproduced.
3. Start a new game with `text_trace = 1` and grep the opening black phase for
   entry points that fire with a string argument.
4. For the twitch, grab consecutive frames standing next to an NPC and diff
   them. The `frame:` log line already prints the centre pixel, so an
   alternating colour there means the whole scene is being re-rendered
   differently rather than one quad flickering.

### 2026-11-04: the block over a finished dialogue line

It is not a layer anything draws -- it is the engine's own string-image quad
with garbage inside. `SGL_grpCreateStringImageInRect` allocates a small
`GL_ALPHA` atlas, calls `SGL_RenderFontToAlphaImageInRect` (a no-op here) and
then uploads that buffer anyway. Nothing ever wrote glyphs into it, so what goes
up is uninitialised heap; `expand_alpha_to_rgba` carried it into the atlas
untouched and the quad painted it over the line our own drawer had already
rendered. At 1080p the atlas is 256x32 and the block measures ~250x65, which is
how it was finally identified.

The fix is in the upload path, not the draw path: `stratlas_add()` registers the
atlases csir allocates, their `A8` uploads are replaced with zeroes, and the
allocation itself no longer reinterprets an A8 buffer as RGBA (a 4x over-read).
An empty atlas draws nothing; `strimg_fill()` still fills the ones that turn out
to have no other drawer, and that upload is `GL_RGBA` so it is not caught.
`stratlas_drop()` unregisters an id as soon as it is re-allocated for something
else, since texture ids are recycled.

### 2026-11-04, later: the three reports from play testing

**1. Slot 3's save could not be deleted.** The row's bin tap never reached the
bin. The offline boot leaves a `连接出错。请稍后重试。` box on the save page whose
`确认` control is laid out at y 470, outside the visible panel, 300x92 -- i.e.
exactly over the third row's bin at 848,466. The engine hands the tap to that
button, so the delete dialog is never even built for that slot.

The fix does **not** delete anything itself: when the tap on a bin is swallowed
(no confirm box within 45 frames), `delete_fallback_tick()` runs the engine's own
`SaveSlot_SlotButtonDelExe` from the main loop with the bin control the tap was
aimed at. That handler derives the slot from the bin's own child index and puts up
the stock dialog, so the confirmation, the 是/否 answer and the removal all stay
the engine's. `delete_slot_rename()` only runs after the player has actually
confirmed, and only to clear what the engine's Hive-gated path leaves behind --
the data file goes first (the engine's `unlink` works now), the sibling
`thpcheckslot<N+1>.dat` follows as `*.deleted`.

Two attempts before this one are worth remembering, because both "worked" and
both were wrong: deleting the files directly from the bin tap (no dialog, and the
engine's slot bookkeeping then points at a save that is no longer there -- the
*next* new game lands in the just-emptied row), and answering the box with
`UIPopupMsg_ButtonOKExe` on the player's behalf. The engine already has a working
delete path; the only broken part was delivery.

**2. "Picking the same class starts the old save instead."** The character-select
screen is *pushed on top of* the save-slot page rather than replacing it, and the
engine's hit test walks the root's children in order -- the save page comes
first, so it claims taps aimed at the class screen whenever the two overlap. The
row handler is not passive: `SaveSlot_SlotButtonExe` clears `GAMESTATE_bNewGame`
and starts a load, which is why the class screen came and went and the old
character walked out.

Two changes: `is_save_page_hit()` drops a tap while the class screen is up if it
lands on a 634x150 row or a 66x66 bin *and* no button of the class screen covers
that point (`tree_has_button_at`), and `GAMESTATE_bNewGame` is re-asserted on
every frame the class screen is up (`force_new_game_flag`), cleared again on
`MAP_Load` or when (1,0) is pushed. The flag lives at `0x29f928`, as a pointer to
the byte `STATE_EnterGame` tests -- note the extra indirection, `ldr x8,[0x29f928]`
then `ldrb w8,[x8]`.

**3. The gold pattern over a finished dialogue line.** `our_text_recent()` only
matched the string exactly. A dialogue body is *typed out*, so our drawer only
ever sees a prefix of the line; the moment the typewriter reaches the last
character the engine stops calling the drawer and creates the string image for
the complete line instead. The prefix never matched, `strimg_fill()` filled the
atlas, and the engine's own quad painted it over the line we had just finished
drawing. The match is now prefix-tolerant in both directions (at least 8 bytes),
with the fill decision looking back 8 frames instead of 4.

Also fixed while testing this: `tap_script` never armed anything. The ini reader
keeps the quotes the README shows, so `sscanf("%d,%d@%u")` saw a leading `"` and
recorded zero taps; `tap_script_parse()` now skips a leading quote itself.

---

## Testing loop

Bringing an NRO up on hardware is a hundred short iterations, so do not do it by
copying files to the card.

**On the console, over the network.** Put the console on the same LAN, open
hbmenu and press **Y** (netloader). From then on every iteration is one command:

```
python deploy.py --watch
```

That builds, refreshes the SD tree, uploads the NRO, launches it and streams the
console's stdout back to the terminal. When the NRO exits or crashes, hbmenu is
back and still listening — nothing is power cycled and the card never leaves the
console. `build/live.log` keeps the transcript; the last line is normally the
step that failed. `tools/build_nxlink.py` rebuilds the uploader itself (zig +
zlib, no devkitPro).

**On a PC, in an emulator.** Eden (and the current yuzu line) boots an NRO
directly, so no homebrew menu is involved:

```
python deploy.py --eden
```

mirrors `dist/switch/inotia4` into `%APPDATA%\eden\sdmc\switch\inotia4`, then
either use **File -> Load File...** on that `inotia4_nx.nro`, or run it from a
terminal (this is also what to script when iterating):

```
eden-cli.exe -g "%APPDATA%\eden\sdmc\switch\inotia4\inotia4_nx.nro"
```

What actually works there: the ELF loader, the relocations, all 231 import
resolutions, the JNI stand-in, `svcMapProcessCodeMemory`, and — surprisingly —
the GL path, because Eden implements enough of the nouveau ioctls that mesa
comes up as `NV120` and hands back a real ES1 context. Audio, the shared font
and the input queue also initialise. See `emu.md` for the current blocker.

If a run dies before writing anything useful, `sd:/atmosphere/crash_reports/`
holds the exception report; `PC`/`LR` there are absolute, so subtract the module
base to get an offset into `build/inotia4_nx.elf`.

---

## Troubleshooting

`debug.log` next to the NRO is the first place to look; the boot sequence is
logged line by line. If the port cannot start at all it writes `fatal.txt` and
shows the message on screen with a `+` to exit.

### Getting a log out of a crashed run

Every line is flushed as it is written, and the previous run is kept as
`debug.prev.log`, because the usual sequence is "it crashed, run it again to go
and fetch the log".

```
python tools/fetch_log.py            # or: -d E: if auto-detection misses the card
```

finds the SD card (any drive letter) and the emulator's virtual card, copies
`debug.log`, `debug.prev.log`, `fatal.txt`, `config.ini` and the newest
Atmosphère crash reports into `logs/<time>/`, prints a verdict, and writes
`logs/inotia4-report-<time>.zip`. That zip is the whole diagnosis.

The log opens with `port: build <timestamp>`, so it always names the binary that
produced it, and every boot step appears twice — `step` before the call and
`step -> ok (N ms)` after it. A step whose `-> ok` line is missing is where the
process died; a step that took 40 seconds is a stall, not a crash.

If the system reported an exception, the crash report carries `pc`/`lr` and a
stack trace of `module + 0xoffset` entries:

```
python tools/where.py logs/<time>/crash_report_0_*.log
```

resolves those offsets against `build/inotia4_nx.elf` and prints function names.

| Symptom | Cause |
|---|---|
| "svcMapProcessCodeMemory is unavailable" | launched in applet mode — use a game override (hold R) or a forwarder |
| "could not find .../libgame.so" | run `tools/pack_data.py`, or the NRO is not next to `libgame.so` |
| Black screen after `nativeStart` | check how far the log got; the last line names the step that failed |
| Music stutters | raise `audio_latency`, lower `dock_1080p` |
| No sound at all | try the `.jpg`-suffixed sound names — the packer already ships both |
| A save slot reads back as corrupt / the character is gone after a restart | the slot file was written with another key; it is renamed `*.dat.bad` at boot, see "Save slots" |

---

## Save slots

`libgame.so` encrypts each slot with a key of its own (`HubSave_GetKey`), and it
puts the slot under `save/<package>/<hex>/save%d.dat`, where `<hex>` is a hash of
the account id. Both halves of that -- the folder and the cipher -- depend on the
same string, so the port pins it (`nx_hubkey` in `src/main.c`) instead of
letting `HubSave_LoadKey` build it from Java app properties that are empty here.
An empty key is not a harmless default: `ENCRYPT_Process2` divides by
`strlen(key)`, and a zero divisor turns the key index into an unbounded offset
that reads past the buffer, i.e. the XOR mask becomes "whatever BSS held at the
time". A slot written that way is readable only by the session that wrote it.

Two consequences worth knowing:

* `save: write '<path>' N bytes, engine key '<key>'` and `save: round-trip ok`
  in `debug.log` mean the file this session wrote is the file the next session
  will accept. `engine key '(empty)'` is the old, broken case.
* At boot, every `save*.dat` / `thpcheckslot*.dat` that the pinned key cannot
  decrypt is renamed to `*.dat.bad` (`quarantine_stale_saves`). A corrupt slot is
  not just a dead slot: the error popup it raises owns the input tree, so the
  next tap on any slot lands in a stale root. Turning it into an empty slot is
  what keeps the rest of the page usable. Nothing is deleted.

### Deleting a slot

The save page's bin button opens a real Yes/No dialog and the engine deletes the
file itself — but only after `bionic`'s `unlink()` is handed a path it can
resolve. The engine builds that path from the Java-side user dir and can produce
`/switch/inotia4/save/...`, i.e. the card path without its `sdmc:` prefix.
`shim_path()` used to return such a path untouched while its callers went on to
use their own buffer, so every `unlink()` ran with an empty name, failed with
`-1`, and the slot stayed on disk. `shim_path()` now maps that form, and
`unlink`/`remove`/`mkdir`/`rmdir`/`rename` all go through `shim_path_resolve()`
so the resolved name always lands in the caller's buffer.

A slot's `thpcheckslot<N>.dat` lives in the sibling `<hex>A` folder. The engine
removes the data file and leaves the checkslot behind; `drop_orphan_checkslots()`
renames such a leftover to `.orphan` at boot, because a slot that is half there
answers a tap, fails to load and then has nothing left for the bin to delete.

The third row's bin is a special case. It sits at 848,466 66x66 and an
unanswered message box keeps an OK button at 726,470 300x92 on top of it, so the
engine hands the tap to the popup: no confirmation dialog is created and that
slot cannot be deleted at all. Calling the bin's handler directly from the touch
path fixed the tap but left the engine's current-slot pointer on that slot,
which sent every later autosave there -- so it is not redirected any more.
Instead `delete_fallback_tick()` waits a few frames after a bin tap; if no popup
appeared the tap was swallowed, and the engine's own delete-button handler is
run from the main loop with the current-slot pointer saved and restored around
it. The dialog, the confirmation and the deletion stay the engine's.

---

## Known good builds

Snapshots live under `snapshots/`, one folder with the NRO plus a `VERSION.txt`
and one zip with the sources that produced it. Restoring a known state is
"copy the NRO over `switch/inotia4/inotia4_nx.nro`" (or unpack the zip and run
`build.py`).

| Snapshot | Build | NRO md5 | Verified for |
|---|---|---|---|
| `snapshots/known-good-20261104-2211/` + `save-slot-20261104-2211.zip` | 2026-11-04 22:10:51 | `a8adfa63d39a19bf308d8dd942146860` | **current default**: everything below, plus the tutorial hint sits inside its black bar (text is centred on the anchor) |
| `snapshots/known-good-20261104-1946/` + `save-slot-20261104-1946.zip` | 2026-11-04 19:46:51 | `5760bb9fb50e4295d417286d76181f97` |: everything below, plus the in-game HUD no longer freezes when it draws a number |
| `snapshots/known-good-20261104-1846/` + `save-slot-20261104-1846.zip` | 2026-11-04 18:46:08 | `2a6d92db69db83f485a664fe1beef2fc` | save slots land in the row they were created in; engine-owned delete dialog; START works again after returning to the title; class-screen tap filter; `$X` colour runs and `&N` line breaks in text |
| `snapshots/known-good-20261103-1958/` | 2026-11-03 19:58 | see its `VERSION.txt` | save read/write, cross-restart slot load, delete, back-to-menu |

The 19:46 build supersedes 18:46: same behaviour, plus the in-game HUD can draw
a number again. That freeze (every frame stuck at
`UIPorting_DrawNumber -> GRPX_DrawStringWithFont`) was **not** the text renderer
-- it was a line of diagnostic logging inside `nx_grpx_draw_string` that called
`grpx_measure()`, i.e. the engine's own `SGL_GetTextSize` through a vtable slot.
That path goes into Android font metrics which do not exist in this shell, and
the first in-game number never came back. Frame 0 of the stack reads
`UIPorting_DrawNumber+0x68` and `lr` points just past the `blr` -- that is how it
was pinned (NRO base `0x80eb3000`, `lr 0x80ec90e4` = `nx_grpx_draw_string+0x214`).
The engine's width was only printed for comparison, so the call was removed; the
port never calls the engine's text metrics now.

Two things this build still does not do:

* the offline `连接出错。请稍后重试。` notice still sits on the save page (its
  button rects are what used to swallow the third row's bin -- the delete path
  now works around it, but the box itself is still drawn);
* a line coloured by the *engine's own state* rather than by a `$` code still
  comes out white. The save page's `黑暗骑士团营地` is the example: the string
  carries no colour code and `glColor` is `ffffffff` at that call, so the
  dark-gold tint in the original comes from something else (a per-vertex colour
  array or the control's own field). The `gl: grpx text ... col %08x` log line
  prints the value for whichever line is being investigated.

## Why in-game hints looked "misaligned"

The screenshot that started this: white hint text, and under it a gold band that
looked like the box it was supposed to sit in. There was no box -- the band was
a **second copy of the same sentence**, drawn by the engine itself. Its position
comes from the engine's own string-image rect, about 20 px below the line it
asks the port to draw, and its glyphs are whatever the stubbed-out Android
rasteriser left in the atlas.

`X_TEXTCTRL_CreateImgString` now drops the image when the string is one our
drawer has already put on screen (`our_text_recent(plain, 60)`), so a control
paints once, from us. Strings we never draw keep their image and are filled by
`strimg_fill()` as before; `string_draw = 2` still forces the image off
everywhere for A/B testing.

The line also hung below that bar, which is a separate thing and now fixed:
`X_TEXTCTRL_DrawTextControl` hands over a y that is the **centre** of the string
image, not the top of the glyphs. The proof is the bar itself, caught with a
targeted GL probe (`bar: quad 80,152-1057,172 tex 31 col 0 0 0 0.60 blend 1`):
it is a 20 px tall black quad centred on y = 162, which is exactly the y the
control passes. Our drawer put the ink's top there, so the whole line sat half a
line low and its lower half fell outside the bar. It is now centred on the
anchor (`y - text_ink_height(str, px) / 2`), which leaves the ink at ~154..171
inside a bar spanning 152..172. `textctrl_dy` overrides the offset in pixels if
it ever needs a nudge; 0 means "centre".

An earlier attempt lifted the line by a whole font ascent, and by a 3/4 ascent
after that. Both were guesses and both were wrong -- the numbers above come from
the engine's own quad, which is what the probe exists for.

Two smaller text fixes in the same pass:

* colour runs of one line are aligned on a common baseline (`text_ascent_at()`).
  Each `$S...$B` span is its own quad, and a span holding only "。" used to be
  anchored at the line's top, which is why the full stop rendered as a raised
  circle;
* the card font is the original `DroidSansFallback.ttf` (`dist/switch/inotia4/
  font.ttf`), so glyph shapes and advances match the device the game shipped on.
  Without it the port falls back to the console's shared font.

Nothing here can restore the engine's own text renderer: that is Android's Skia
(`android.graphics.Paint` / `Canvas.drawText`), which lives in the Android
framework, not in the APK, and cannot be dropped onto a Switch. The port answers
the engine's text entry points itself; matching the *font* is what makes its
measurements and our glyphs agree.

## Backup

Sources, snapshots and the notes in `.workbuddy/memory/` are mirrored to
<https://github.com/BenAlan11/inotia4_nx> (branch `main`). Game data, the APK
dump, toolchains and saves are not published there.

On this machine `git push` cannot reach `github.com` (the egress proxy only
forwards `api.github.com`), so updates go through the REST API:

```
GH_TOKEN=... python tools/gh_push.py BenAlan11/inotia4_nx main
```

It creates blobs, trees and a commit through the Git data API and moves the
branch, which produces exactly the tree `git push` would have produced. The
commit continues the remote branch's history when one exists.

## Testing on the second screen

Launches are placed on monitor 2 so they never cover or minimise whatever is on
the primary screen:

```
python tools/win_place.py --monitor 2 --watch 30   # after starting eden
python tools/win_place.py --list                   # monitors + window rects
```

It uses `SetWindowPos` with `SWP_NOACTIVATE`, so the window does not take focus.

## Layout of this repository

```
build.py              compile + link + package
deploy.py             build -> pack -> push to console or emulator
src/                  the port itself
tools/elfinfo.py      ELF/JNI/import inspector for libgame.so
tools/disas.py        disassemble one exported function by name
tools/xref.py         find and disassemble the code that references a string
tools/gen_imports.py  regenerate src/imports.c from the .so's import table
tools/pack_data.py    APK -> SD card tree
tools/make_icon.py    APK's own launcher icon -> 256x256 icon.jpg
tools/axml.py         AndroidManifest / resources.arsc reader
tools/build_nxlink.py cross-compile nxlink for the deploy loop
tools/dkp_pull.py     fetch devkitPro packages through the registry mirrors
data/                 libgame.so, libHercules.so (extracted from the APK)
ref/                  reference ports kept for API shape comparison
config.prod.ini       pristine shipping config; copy over dist/.../config.ini
                      after any run that turned on debug knobs
```

`tools/elfinfo.py data/libgame.so` is the fastest way to re-check the facts this
port is built on — 231 imports, 58 JNI exports, 58 GLES1 symbols, 0 EGL, and
`DT_NEEDED: libGLESv1_CM.so, libdl.so, liblog.so, libc.so, libm.so`.

## Legal

*Inotia 4* is © Com2uS Corp. This repository contains no game code and no game
assets; both are extracted from an APK you supply. Use it only with a copy you
legitimately own.
