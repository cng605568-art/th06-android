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
| Lasers | 64 laser slots, warning line → active → shrinking phases, the original's hitbox quirks (half-width deadly core, speck-sized hitbox while warning/shrinking), graze every 12 frames, all laser ECL opcodes incl. stale-reference behaviour, the laser ex-instructions of stages 4, 6 and Extra |
| Items | 512-slot item pool, rise/gravity physics, homing at full power above the collection line, pickup rules for all 7 item types, point-item value by height and difficulty, full-power chain scoring, random drop table, `DROP_ITEMS`, death scatter, bullets/lasers turning into star items |
| Bombs | All four player bombs: Reimu A's homing orbs that explode, Reimu B's four sealing beams, Marisa A's stardust stars, Marisa B's Master Spark (0.3x movement, no shots). Damage regions and bullet-cancel boxes on the original's frame patterns, bullets turned into homing point items, enemy-contact shield, homing drops from bomb kills, invulnerability timings, the darkening overlay, the deathbomb window (6 frames after a hit) with its shrinking-window quirk, rank -2 per bomb, spell-card capture failure and the bomb-damage rules during cards. Screen shake with its random offsets and playfield shrink. |
| Effects | The 512-slot particle ring with its rolling cursor and the 20 effect types from `etama4.anm` (bubble explosions, splash and big-splash sparks, flashes, converging glows, drifting bat-wing particles) plus the stage's `eff0N.anm`. Each type moves and draws random numbers exactly as the original does, so bullet patterns stay on the same random sequence. Spawned from shot hits, grazes, enemy deaths (ECL 100 death effects), player deaths, bombs, ECL 118 and the ex-instructions. Spell circles (ECL 102) orbit their enemy in 3D, grow, spin and fade when released. Effects are drawn with the original's perspective camera. |
| GUI | The game's own side panel from `front.anm` (frame, labels, life and bomb stars, power bar and MAX, hi-score/score/power/graze/point in `ascii.anm`'s font), the boss HUD (label enter/leave, life bar filling at 1 %/frame and draining at 2 %, life count, spell timer coloured by seconds left with its countdown tick), spell-card portraits and names, the bomb portrait and name, sliding *BONUS* / *Full Power Mode!!* / *Spell Card Bonus!* messages, the 512 + 3 score-popup rings, and the stage title/song banner from `stageN.std`. Names and titles are Shift-JIS text rendered at run time into `text.anm`'s blank texture, using the system's Japanese font on Android. Capture bonus = base + base × seconds ÷ 10. |
| Dialogue | `msgN.dat` scripts started by the stage timeline: portraits from the face files, two dialogue lines and two name/title lines rendered into `text.anm`, the opening box, waits that pass on a fresh Shot press (or time out), skip (Ctrl, gamepad Y, or BOMB on the touch pad), the timeline waiting for the conversation, no shooting or bombing while it runs, the boss HUD hidden, song-name changes, the stage-clear tally (computed one frame after the results opcode, with the original's text layout) and the jump to the next stage. Stage 6 and Extra load their final-boss art when the first conversation starts. |
| Boss ex-instructions | All 17 stage-specific ECL routines (opcodes 121/122): Cirno's freezing bullets, the random-area shot, the pentagram that drifts towards you, Patchouli's spell selection by shot type, Sakuya's **time stop** and the green redirection of her knives, the stage 5 dagger arcs, Remilia's bat-wing trail, laser rings, dark red balls seeded on large bullets, resting bullets set moving with a curl or at random, and her **bat transformation** while you bomb (no damage, no contact, back 60 frames after the bomb), the Extra stage's rings around the centre, the large bullets that fling nearby resting bullets outwards, and Flandre's life-dependent last attack. Each scans bullets in slot order and draws random numbers in the original's order. Time stop freezes the player, bullets, items, lasers, the GUI, the background (tinted dark red) and screen shake, while enemies, effects and the score keep running and spell timers stand still. |
| Stage backgrounds | `stageN.std` + `stgNbg.anm`: objects of animated quads placed by the instance list, the background script (camera keyframes with one key per frame, aim easing with its one-frame lag, fog fades, pause until ECL 125), the original's perspective camera (30° FOV, eye distance from the playfield height), per-pixel linear fog, depth-sorted quads in their own depth range, camera-facing sprites, the original's height-only culling, the 61-frame fade to the spell-card background during cards, and the loading-screen halt between stages. Drawn on the GPU with perspective-correct texturing. |
| Resources | Power (0–128), bombs, lives, graze, rolling score counter, score extends (10M/20M/40M/60M), stage-clear bonus, death drops and life loss |
| Player | Movement and clamping, 1.25 px hitbox, graze box, `player00/01.anm` scripts, option orbs that slide in when focusing (with the original's mid-transition reversal). Death as in the original: the 6-frame deathbomb window, then the ship stretches and fades out where it was hit, moves to the respawn point 31 frames after the hit, grows back in over 30 frames while enemy bullets are cleared (120 frames in all), then blinks through 239 frames of invulnerability; no spawn-in at a stage start. |
| Ending and staff roll | Clearing stage 6 (or stage 5 on Easy) plays the game's own ending from the ED archive: the good ending per shot type (`end00/01/10/11.end`) or the bad ending per character on Easy or after a continue (`end00b/10b.end`), then its chained staff roll. The script interpreter covers every command: text lines in up to 16 `text.anm` slots (8 lines, rendered with the system's Japanese font in the script's colour), waits with minimums, screen clears, `staff01..03.anm` pictures, background pictures with vertical scrolling, fades to/from black and white, music and fades. Shoot ends a wait once its minimum has passed; holding Ctrl (touch: BOMB) fast-forwards 5× once you have cleared before, and always during the staff roll. Then the result screen in "cleared" mode. Extra goes straight to the results, as in the original. |
| Results and high scores | `score.dat` in the original format (obfuscation, checksum, the high-score tables per difficulty and shot type, clear progress, spell-card history, practice bests). Your PC `score.dat` in the game folder is read until the port has saved its own (in the app's storage). The HUD's hi-score starts from the table. After a game (game over, giving up, or clearing the last stage) the **result screen** from `result00..03.anm` over `result.jpg`: name entry for a top-10 score on the original 6 × 16 key grid (auto-repeat, END, SP, backspace) with both shot types' tables, then the statistics page (score, difficulty, completion rate, continues, misses, bombs, spell cards captured, slowdown), then the scores are saved. Spell-card attempts and captures are recorded. A practice run saves its best score for the stage (and its clear progress and spell history) without the result screen. |
| Title screen | The game opens at its own title from the TL archive (`title00.jpg`, `title01..04.anm`), with the original's 200-frame white fade-in and the title theme: "press a key", then the main menu (**Start**, **Extra Start**, **Practice Start**, **Result**, **Music Room**, **Option**, **Quit**; Replay plays the cancel sound) with the original's 20-frame lock, auto-repeat, red highlight and darkening backdrop. Start leads through the select screens (`select00.jpg`, `select01..05.anm`, `slpl00a..01b.anm`): difficulty (the cursor remembers your last choice), character (Left/Right) and shot type, fading in from black, every card driven by the original's animation interrupts. **Extra Start** opens once a shot type has cleared Normal, Hard or Lunatic without continuing, and then only that character and shot type can be chosen. **Practice Start** offers the stages your `score.dat` has reached for that shot type and difficulty, each with its best practice score, starts with 2 lives, 3 bombs and the stage's power (0 / 64 / MAX), ends after the one stage and saves its best. **Option**: starting lives (0–4) and bombs (0–3), music (off / WAV / MIDI, played as WAV), sound effects on/off and Default, saved in `th06.cfg` (the original's format) in the app's storage. **Result** is the original's score viewer (`result00..03.anm` over `result.jpg`): the top-10 tables of each difficulty for Reimu or Marisa (Left/Right) and your spell-card history (7 pages of 10: captures / attempts, unseen cards as ？？？？？). **Music Room** (`music.jpg`, `music00..02.anm`): the tracks and comments from `musiccmt.txt`, scrolling ten at a time; SHOT plays a track (from your `bgm` folder) and shows its comment. Every game (game over, results, ending, quitting from the pause menu) returns to the title with the cursor where the original puts it. A game started with 3 or 4 lives has the original's stage-clear Player Penalty (× 0.5 / × 0.2). |
| Menus | The in-game **pause menu** (Resume / Quit, then "really quit?" Yes / No) and the **continue menu** after the last life (Yes / No with the credits left, cursor on No and locked for half a second), both drawn from `ascii.anm` over a snapshot of the frozen playfield (`capture.anm`), with the original's input locks, highlight colours and close timings. Continuing restarts the score from the number of continues (its last digit), refills lives and bombs, keeps rank and the stage; 3 credits on Easy–Lunatic, none on Extra. Menu (Esc, gamepad Start, the touch pause button or Android Back) opens and resumes; Q quits at once on a keyboard. Music keeps playing; nothing else moves. |
| Shots | All four shot types (Reimu A/B, Marisa A/B) at all nine power tiers: 80-slot pool, the 30-frame fire cycle (a tap fires a whole cycle), homing amulets, accelerating missiles with blast radius and decaying damage, Marisa B's beams hanging from the orbs with their period timers, one-hit normal shots with hit animations, the 70-per-frame enemy damage cap. The shot tables are read at runtime from **your** `東方紅魔郷.exe` (version 1.02h, recognised by SHA-256); without it a basic forward shot is used. |
| Music | The 2002 game's WAV mode: the stage file's song path becomes your `bgm/th06_NN.wav`, with loop points from `th06_NN.pos` in the archives; the intro plays once, then the loop repeats gaplessly. Stage theme at stage start, boss theme from the dialogue, 4-second fade after the boss (linear in decibels to −50 dB), every request restarts its song, music keeps playing through the pause menu. Mono or other sample rates are played at the device rate. **Replace music** (launcher): pick one of the 17 songs and an audio file of your own (MP3, AAC, OGG, FLAC, WAV …); it is decoded with Android's decoders, converted to the game's format (WAV, 16-bit PCM, 44,100 Hz, stereo, silence trimmed at both ends) and kept in the app's storage, then played in that song's place everywhere (stages, bosses, title, ending, Music Room), looping like the game's music: the whole song, or from a loop point you give (seconds) to the end. *Restore* brings the original back; the game folder is never changed. |
| Sound | SoundIdx → wav + DirectSound volume table, 3-entry per-frame queue with de-duplication, played once per frame |
| Main loop | `std::chrono` fixed timestep of exactly 1/60 s (rational arithmetic, no drift), rendering once per display refresh with positions interpolated to the present instant (60/90/120/144 Hz), vsync-jitter snapping, max 4 ticks per frame |
| Mischief (port extra) | Launcher switches not in the original: infinite lives, infinite bombs, invincible, power always MAX, unlimited continues. While any is on, the HUD says so and the game's scores, clear progress and spell history are not saved (the result screen still shows the statistics). |
| Controls | Touch controls registered as an **SDL virtual game controller** (physical gamepads and keyboards use the same path). **Drag anywhere** (default): a finger off the buttons steers like a touchpad, the ship following your finger's travel at its normal speed (the anchor slides along the walls, so dragging back responds at once); the same drag is a floating 8-way stick in menus. Or the classic 8-way **D-pad**. Launcher options: drag speed, button size and opacity, left-handed layout, a latching Focus button, auto-fire, and a short vibration on a miss and on a bomb. |

**Not ported yet** (shown live under *NOT PORTED YET* in the HUD whenever the game data uses
them): replays and the attract demo (the result screen skips the save-replay prompt), Key
Config (Android maps controllers itself) and the score viewer's keyboard cheat, MIDI music
(only the WAV music from the `bgm` folder plays, also in MIDI mode).

Accuracy notes: game logic runs in the original order on the original data, but float math
is not bit-identical to the x87 code of 2002, so long runs can drift from real replays.

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
   Keep `東方紅魔郷.exe` (1.02h) in the folder: the shot types are read from it. Copy the
   `bgm` folder too (`th06_01.wav` …) for music.
3. Tap **Play**: the game's title screen opens (SHOT confirms, BOMB or Back cancels). The
   launcher's **Stage select** skips the title and starts at any stage (Extra too) with the
   shot type and difficulty picked there, as practice: you play on from that stage (with the
   power a practice run of it gives), and nothing from that game is saved, not even with a
   top-10 score. Without the 2002 archives the screen says what is missing.
4. Optional: **Replace music** (below Play) swaps any of the 17 songs for an audio file of your
   own: choose the song, optionally a loop point in seconds, then the file.

Controls: D-pad left, **SHOT / BOMB / FOCUS** right, **II** opens the pause menu (as does
Back); choose with the D-pad and SHOT, and **Quit → Yes** returns to the title (its **Quit** returns
to the launcher). Leaving the
app also pauses. A Bluetooth/USB gamepad works too (the touch overlay hides itself until you touch the
screen). Advanced: archives placed in `Android/data/io.github.cng605568art.th06/files/gamedata/`
(e.g. with `adb push`) are also loaded.

## Launcher look

Shrine red (primary) and sakura pink (accent) on warm paper: a red header with the app's
picture and 東方紅魔郷 in brush script, a big Play button, and white cards for Game files,
Stage select, Mischief and Replace music. Titles and buttons use **Yuji Syuku** (brush) and
text uses **Zen Antique** (antique Mincho), both SIL Open Font License 1.1, bundled as
subsets with their licences in `app/src/main/assets/fonts/`. The app icon is an adaptive
icon (the picture over shrine red) with round and square fallbacks for older launchers.

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
  src/game/BulletManager      640-slot bullet manager and the 64 lasers
  src/game/ItemManager        512-slot items, pickups, drops
  src/game/Resources          graze, rolling score, extends, stage-clear bonus
  src/game/Player             player movement, hitbox, orbs, shots
  src/game/ShotData           reads the shot tables from your 東方紅魔郷.exe (PE32, SHA-256 checked)
  src/game/Bomb, ScreenShake  the four player bombs; screen shake
  src/game/EffectManager      512-slot particle effects, spell circles
  src/game/Gui                side panel, boss HUD, names, messages, score popups, ASCII text, dialogue
  src/game/StageBackground    STD stage backgrounds: script, camera, fog, culling, spell-card fade
  src/game/StageMenu          pause menu and continue menu
  src/game/ScoreFile, ResultScreen   score.dat; the result screen (name entry, statistics)
  src/game/Ending             the ending / staff roll script interpreter
  src/gfx/TextRenderer        Shift-JIS text into textures (Android: system font via JNI)
  src/game/Stage              loads a stage from the archives, runs one tick, draws
  src/core/ZunMath, FixedTimestep, Sha256   ZUN's RNG and angle helpers; the 60 Hz clock; SHA-256
  src/input/                  VirtualController (touch → SDL virtual pad), InputMapper
  src/audio/                  SoundPlayer (SoundIdx table, frame queue), MusicPlayer (WAV + .pos looping, fades), AudioMixer (SDL audio)
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
