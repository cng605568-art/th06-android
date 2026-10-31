# TH06 Android Port (engine foundation)

Native Android engine for *Touhou Koumakyou ~ the Embodiment of Scarlet Devil*, written in
C++20 on SDL2 + OpenGL ES 3. It reads **your own copy** of the game's archives on the phone
at runtime. No game file is in this repository or in the APK.

Built entirely by GitHub Actions: no Android Studio, no local SDK.

## What works today

| Area | Status |
|---|---|
| CI | Host unit tests → release APK (Gradle 9.4.1, AGP 9.2.1, NDK r28c, CMake) → signed with your keystore → GitHub Release on tags |
| 2002 archives (`紅魔郷CM.DAT` / `KOUMAKYO_CM.dat` …, **PBG3**) | Full reader: bit-packed table + ZUN's LZSS. No encryption exists in this format. |
| New Classic archives (`th06CM.dat` …, **PKGL**, Steam 2026) | Reader: XOR keystream (key from the file name) + Zstandard. DDS textures incl. BC7 are decoded on the CPU. |
| Assets | ANM sprite tables, PNG/JPEG/BMP/TGA, DDS (BC1/2/3/7, 32-bit), alpha-mask images, colour keys, WAV sound effects |
| Engine | Fixed 60 Hz simulation, vsync-snapped accumulator, 640x480 offscreen render letterboxed to the screen, GL context-loss recovery |
| Bullets | `ObjectPool<T, N>`: one allocation, O(1) spawn/kill, dense iteration, generational handles. Instanced renderer: thousands of bullets in one draw call |
| Controls | Touch D-pad + Shot/Bomb/Focus/Pause registered as an **SDL virtual game controller**; physical gamepads and keyboards use the same path |
| Game | A test stage (player, shots, bombs, graze/hit, four bullet patterns peaking at ~2,700 live bullets) that uses your bullet and player sprites when the archives are present |

**Not yet:** the original stages, enemies, bosses, dialogue and menus. Those run on ZUN's
script formats (ECL, STD, ANM scripts, MSG), which need interpreters on top of this engine.
Background music is also not played yet (the 2002 game keeps it as WAV files in `bgm/`,
outside the archives; New Classic uses Opus).

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
3. **Play.** Without game data the app still runs the test stage with generated art.

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
  src/gfx/                    Image (PNG/DDS), Anm, TextureLibrary, SpriteBatch (instanced GLES3), RenderTarget
  src/core/ObjectPool.hpp     the bullet pool
  src/game/                   BulletManager, Game (test stage), GameArt
  src/input/                  VirtualController (touch → SDL virtual pad), InputMapper
  src/audio/AudioMixer        SDL audio, WAV sound effects
  src/engine/App              window, GL context, fixed-step loop, lifecycle
tests/                        host tests run by CI before every APK build
```

Local builds (optional, Linux/macOS): `bash scripts/fetch_deps.sh`, then
`cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests`.
With Gradle 9.4.1 and the Android SDK installed: `gradle :app:assembleRelease`.

## Credits

Format knowledge, not code: the PBG3/LZSS layout follows [thtk](https://github.com/thpatch/thtk)
and the [th06 decompilation](https://github.com/happyhavoc/th06); the PKGL layout follows the
MIT-licensed notes in [th06nc-re](https://github.com/creeperkafasi/th06nc-re). Libraries:
SDL2 (zlib), Zstandard (BSD), stb_image (public domain), bcdec (MIT).

Touhou Project is © Team Shanghai Alice. This is an unofficial fan project; you need your own
legally obtained copy of the game.
