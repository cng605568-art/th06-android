# TH06 Android Port

Native Android engine for *Touhou Koumakyou ~ the Embodiment of Scarlet Devil* (2002),
written in C++20 on SDL2 + OpenGL ES 3. It reads **your own copy** of the game's archives on
the phone at runtime and runs the game's own compiled scripts: ECL for enemies and bullet
patterns, ANM for every animation. No game file is in this repository or in the APK.

Built entirely by GitHub Actions: no Android Studio, no local SDK.

## What works today

| Area | Status |
|---|---|
| CI | Host unit tests → release APK (Gradle 9.4.1, AGP 9.2.1, NDK r28c, CMake) → signed with your keystore → GitHub Release on tags |
| 2002 archives (`紅魔郷CM.DAT` / `KOUMAKYO_CM.dat` …, **PBG3**) | Full reader: bit-packed table + ZUN's LZSS. No encryption exists in this format. |
| New Classic archives (`th06CM.dat` …, **PKGL**, Steam 2026) | Reader (XOR keystream + Zstandard, BC7 DDS). The stage engine itself needs the 2002 files. |
| ANM VM | All 32 opcodes of the 2002 engine: sprites, scale/rotation/alpha/position interpolation, interrupts and labels, UV scroll, halt/static. Draw paths (Draw/Draw2/Draw3) with the original rounding and its UV-cache quirk; D3D8 blend states (`SRCALPHA/INVSRCALPHA`, `SRCALPHA/ONE`, `MODULATE`/`ADD`) mapped to GLES. 16-bit texture formats quantised like Direct3D did. |
| ECL VM | Stage timelines (all 13 opcodes) and enemy subs: variables (incl. float arguments that name variables), jumps/loops/compares, an 8-deep call stack, difficulty masks, movement and interpolation modes, bullet patterns with rank, interrupts, life/timer/death callbacks, boss slots, spellcard damage scaling. Quirks of the executable are kept (self-modifying `INC`/`DEC` on literals, interrupt return skipping one instruction). |
| Bullets | The original's 640 fixed slots and slot scan, 10 bullet types from `etama3/etama4.anm`, all 9 aim modes, spawn/cancel animations, speed-up/acceleration/velocity/turn/bounce flags, graze and hit boxes |
| Player | Movement speeds and clamping, 1.25 px hitbox, graze box, `player00.anm` scripts, death/respawn invulnerability, the Reimu A level-1 shot |
| Sound | SoundIdx → wav + DirectSound volume table, 3-entry per-frame queue with de-duplication, played once per frame |
| Main loop | `std::chrono` fixed timestep of exactly 1/60 s (rational arithmetic, no drift), rendering once per display refresh with positions interpolated to the present instant (60/90/120/144 Hz), vsync-jitter snapping, max 4 ticks per frame |
| Controls | Touch D-pad + Shot/Bomb/Focus/Pause registered as an **SDL virtual game controller**; physical gamepads and keyboards use the same path |

**Not ported yet** (shown live under *NOT PORTED YET* in the HUD whenever the game data uses
them): lasers, items, particle effects, dialogue (MSG) — the stage currently ends when its
timeline has run out instead of after the boss dialogue — stage backgrounds (STD), bombs,
spellcard name/bonus display, boss HUD, ECL ex-instructions 2–16, background music, menus,
other shot types and power levels.

Accuracy notes: game logic runs in the original order on the original data, but float math
is not bit-identical to the x87 code of 2002, and features that are not ported (items,
particles) would have consumed random numbers, so long runs drift from real replays.

## Set up the build (once)

1. Create a new GitHub repository and put all of these files in it (GitHub Desktop:
   *File → Add local repository*, then *Publish repository*).
2. Add your signing key as repository secrets: **Settings → Secrets and variables →
   Actions → New repository secret**

   | Secret | Value |
   |---|---|
   | `ANDROID_KEYSTORE_BASE64` | your `.jks` file as base64 |
   | `ANDROID_KEYSTORE_PASSWORD` | its password |
   | `ANDROID_KEY_ALIAS` | the key alias, only if it is not `release` |
   | `ANDROID_KEY_PASSWORD` | only if different from the keystore password |

   No keystore yet? With any JDK installed:
   ```
   keytool -genkeypair -v -keystore th06-release.jks -alias release -keyalg RSA -keysize 4096 -validity 10000
   ```
   Base64 on Windows PowerShell:
   `[Convert]::ToBase64String([IO.File]::ReadAllBytes("th06-release.jks")) | Set-Clipboard`
   Keep the `.jks` file and password safe and never commit them: losing them means users
   must uninstall before installing future updates.

## Get an APK

* **Every push to `main`:** open the **Actions** tab → the latest run → download the APK
  artifact at the bottom of the page.
* **A release:** **Releases → Draft a new release → Choose a tag →** type e.g. `v0.1.0-beta`
  → **Publish**. The workflow builds, signs and attaches the APK and `SHA256SUMS.txt`.
  Tags with a `-` (like `-beta`) become pre-releases.

Without the keystore secrets, branch builds are signed with a throwaway debug key (file name
ends in `-debugsigned`); they install for testing but cannot update each other. Tag builds
refuse to run unsigned.

## Play

1. Copy your game folder to the phone (USB, or a cloud drive synced to local storage):
   * 2002 original: the folder with `紅魔郷CM.DAT`, `紅魔郷ST.DAT` … (or `KOUMAKYO_*.dat`).
     The Steam bundle includes this version alongside New Classic.
   * New Classic (Steam): the `data` folder with `th06CM.dat`, `th06ST.dat` …
2. Open **TH06 Port → Choose game folder** and pick that folder. Files are read in place.
3. **Play.** Stage 1 starts on Normal. Without the 2002 archives the screen says what is missing.

Controls: D-pad left, **SHOT / BOMB / FOCUS** right, **II** pauses. Back pauses; Back again
quits. A Bluetooth/USB gamepad works too (the touch overlay hides itself until you touch the
screen). Advanced: archives placed in `Android/data/io.github.cng605568art.th06/files/gamedata/`
(e.g. with `adb push`) are also loaded.

## Layout

```
.github/workflows/build.yml   CI: tests, APK, signing, releases
scripts/fetch_deps.sh         pinned SDL2 2.32.10, zstd 1.5.7, stb_image, bcdec (SHA-256 checked)
app/build.gradle              AGP config, externalNativeBuild → CMake, signing from env vars
app/src/main/java/...         ImportActivity (folder picker), GameActivity (SDL host + JNI), DataSources
app/src/main/cpp/
  CMakeLists.txt              libSDL2.so + libmain.so (also a Linux desktop dev build)
  cmake/th06core.cmake        GL-free core shared with the tests
  src/io/                     MappedFile, BitReader, Lzss, Pbg3Archive, PkglArchive, Vfs
  src/gfx/                    Image (PNG/DDS), Anm (file tables), AnmManager (ANM VM + drawing),
                              TextureLibrary, SpriteBatch (instanced GLES3, D3D8 blend states), RenderTarget
  src/game/Ecl                ECL file layout (packed structs), opcode tables
  src/game/EnemyManager       timeline + ECL VM, enemies
  src/game/BulletManager      640-slot bullet manager
  src/game/Player             player movement, hitbox, shot
  src/game/Stage              loads a stage from the archives, runs one tick, draws
  src/core/ZunMath, FixedTimestep   ZUN's RNG and angle helpers; the 60 Hz clock
  src/input/                  VirtualController (touch → SDL virtual pad), InputMapper
  src/audio/                  SoundPlayer (SoundIdx table, frame queue), AudioMixer (SDL audio)
  src/engine/App              window, GL context, dual loop, lifecycle
tests/                        host tests run by CI before every APK build
```

Local builds (optional, Linux/macOS): `bash scripts/fetch_deps.sh`, then
`cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests`.
With Gradle 9.4.1 and the Android SDK installed: `gradle :app:assembleRelease`.

## Credits

Format knowledge, not code: the PBG3/LZSS layout follows [thtk](https://github.com/thpatch/thtk);
ECL/ANM/bullet/player semantics and tables follow the CC0
[th06 decompilation](https://github.com/happyhavoc/th06); the PKGL layout follows the
MIT-licensed notes in [th06nc-re](https://github.com/creeperkafasi/th06nc-re). Libraries:
SDL2 (zlib), Zstandard (BSD), stb_image (public domain), bcdec (MIT).

Touhou Project is © Team Shanghai Alice. This is an unofficial fan project; you need your own
legally obtained copy of the game.
