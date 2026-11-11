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
| Display | Landscape (the whole 640×480 picture, letterboxed) or **portrait**: hold the phone upright and, in play, the 384×448 playfield fills the width (a strip kept clear for the camera cut-out) with the score rows of the side panel below it and the touch controls underneath; title, menus, results and story screens stay whole at the top. Scaling: smooth, sharp pixels, or pixel-perfect whole multiples; optional CRT scanlines. |
| Main loop | `std::chrono` fixed timestep of exactly 1/60 s (rational arithmetic, no drift), rendering once per display refresh with positions interpolated to the present instant (60/90/120/144 Hz), vsync-jitter snapping, max 4 ticks per frame |
| Replays | Every game is recorded in the 2002 game's `.rpy` format (version 1.02h): the state at each stage's start (score, RNG seed, power, lives, bombs, rank, point items) and the buttons whenever they change. After a full game (from Stage 1, or Extra) the result screen asks **"save replay?"** as the original does: 15 slots (`replay/th6_01.rpy` … `th6_15.rpy` in the app's storage), overwrite?, a name on the same key grid; after a continue it says the replay cannot be saved. The title's **Replay** item lists the slots (and any `th6_ud????.rpy` you copy in), then the file's stages with their scores and slowdown; a replay plays from the chosen stage through the rest of the file, skippable dialogue at 3×, Esc pauses. Twelve idle seconds on the title start the **attract demo** (`demo00.rpy` from the archives, Stage 4, "DEMO PLAY", any key or 62 seconds end it). Touch drag-to-move is stored in an extra chunk at the end of the file (the original game still loads such files; replays played with buttons or a D-pad are plain PC replays). PC replays load and play; float math is not bit-identical to the 2002 build, so long PC replays can drift. Saved in Backup. |
| Practice tools (port extra) | Launcher options not in the original: **game speed** 75% or 50% (slow motion: game time runs slower, the music does not), a **hitbox marker** on the ship (red ring, white centre), and **save states**: SAVE / LOAD touch buttons beside the pause button (F5 / F9 on a keyboard, L3 / R3 on a gamepad) keep one copy of the whole stage in play (enemies, bullets, items, effects, the player, the HUD, the script, the RNG) and put it back; the state expires when another stage loads. Slow motion or loading a state makes that game practice: the HUD says so and its scores are not saved. The marker changes nothing. |
| Backup, logs, updates | **Backup** (launcher): one ZIP with your high scores (`score.dat`), game options (`th06.cfg`), replays, launcher settings and, if you like, your replaced music, saved wherever you choose; **Restore** brings it back (after reinstalling, or on another phone). **Help & updates**: the app's version, a daily check for a newer release on GitHub (only the release's name and page are fetched; you install it yourself), **Save a log for a bug report** (the game's session log, the previous run's log and the last crash report, as one text file you can attach to an issue) and a link to the issue tracker. If the game crashed, the launcher says so next time and offers the log. Nothing is ever sent automatically. |
| Mischief (port extra) | Launcher switches not in the original: infinite lives, infinite bombs, invincible, power always MAX, unlimited continues. While any is on, the HUD says so and the game's scores, clear progress and spell history are not saved (the result screen still shows the statistics). |
| Controls | Touch controls registered as an **SDL virtual game controller** (physical gamepads and keyboards use the same path). **Drag anywhere** (default): a finger off the buttons steers like a touchpad, the ship following your finger's travel at its normal speed (the anchor slides along the walls, so dragging back responds at once); the same drag is a floating 8-way stick in menus. Or the classic 8-way **D-pad**. Launcher options: drag speed, button size and opacity, left-handed layout, a latching Focus button, auto-fire, and a short vibration on a miss and on a bomb. |

**Not ported yet** (shown live under *NOT PORTED YET* in the HUD whenever the game data uses
them): Key Config (Android maps controllers itself) and the score viewer's keyboard cheat,
MIDI music (only the WAV music from the `bgm` folder plays, also in MIDI mode).

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
* **A release:** **Releases → Draft a new release → Choose a tag →** type `v` + the version
  in the `VERSION` file (e.g. `v0.9.0`, or `v0.9.0-beta` for a pre-release) → **Publish**.
  The workflow builds, signs and attaches both APKs (TH06 and TH07) with `SHA256SUMS.txt` and
  `SHA256SUMS-th07.txt` (a tag that does not match `VERSION` is refused). Tags with a `-` become pre-releases. The launcher's
  **Check for updates** compares its own version with the newest full release.

Versions: `VERSION` holds the next release's number; branch builds are named
`<VERSION>-dev+<commit>` (shown in the launcher's *Help & updates*). Each run also keeps
`debug-symbols-…` (the unstripped `libmain.so`) for 30 days, to decode the addresses in a
player's crash log.

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

## Perfect Cherry Blossom (th07) app

A second app, **TH07 Port** (`io.github.cng605568art.th07`, module `th07/`), runs on the same
engine and installs next to the th06 app. **Play** opens the game's own title screen: Start,
Extra Start (and Phantasm), Practice Start, Result (high scores per difficulty and shot, the
spell card history, Player Data), Music Room, Option and Quit, with the original's select
screens, unlock rules, name entry, result statistics, endings and staff roll. Scores, clear
progress, spell history and play statistics are kept in `score.dat` (the PC game's format, in
the app's storage) and the settings in `th07.cfg`. Every game is recorded in the PC game's
`.rpy` format; after a game without continues the result screen offers to save it into one of
15 slots (`replay/th7_01.rpy` … `th7_15.rpy` in the app's storage, named on the key grid).
**Replay** on the title lists those and any `th7_ud????.rpy` copied into that folder, then the
file's stages and the three playback modes (normal, the recorded slowdown, fast outside boss
fights; dialogue always plays fast). Fifteen idle seconds on the title start the attract demo
(the three demo replays in `th07.dat`, in turn). Files saved by the PC game load and play;
whether they stay in sync for long depends on the simulation matching the PC's exactly.
The FPS counter sits in the bottom-right corner as in the original (the game's speed in logic
passes per second; replays also show their recorded FPS), and the slowdown on the result
screen and in replays is measured from it. The menu descriptions are read from your
`th07.exe`.

* **Get it:** every CI run also builds it, signed with the same key, as its own artifact
  `th07-android-<version>`, and every GitHub Release from v0.10.0 carries it. Locally, with the
  Android SDK: `gradle :th07:assembleRelease` (APK in `th07/build/outputs/apk/release/`).
* **Files from your own copy (version 1.00b):** `th07.dat` and `th07.exe` are needed (the
  game data, and the program the sound effects and other tables are read from, recognised by
  its SHA-256); `thbgm.dat` is optional and adds the music (its index, `thbgm.fmt`, is inside
  `th07.dat`).
* **Import:** copy your game folder to the phone, then **TH07 Port → Choose game folder** (or
  **Choose files** to pick them one by one). Each file is recognised by its contents and
  copied into the app's own storage (`files/gamedata/`), so the folder can be deleted
  afterwards; the file list shows what is there and whether it is the right file, and
  **Remove imported files** deletes the copies.
* **Start options:** *Skip the title screen* (off by default) starts a stage directly
  instead, with its choices: character and shot type (Reimu A/B, Marisa A/B, Sakuya A/B),
  difficulty (Easy to Lunatic, or Extra and Phantasm with their own stage), stage 1–6, spare
  lives 0–4 and a practice start; touch controls (drag or D-pad, drag speed, button size,
  opacity, auto-fire, latching Focus, left-handed, vibration on a miss and a bomb) and display
  (landscape or portrait, scaling, scanlines) work as in th06. Back opens the pause menu in a
  game and cancels in the menus; on the title with the cursor on Quit it returns to the
  launcher. Without a 1.00b `th07.dat` and `th07.exe` the game shows a status screen saying
  what is missing.
* **The th06 launcher's other settings:** the launcher in English, 日本語, 简体中文, 繁體中文 or
  Bahasa Melayu; **practice tools** (game speed 75 % / 50 %, a hitbox marker, SAVE / LOAD save
  states); **mischief** (infinite lives or bombs, invincible, power always MAX, unlimited
  continues); **replace music** (any song with your own audio file, converted to WAV 44.1 kHz
  stereo and looped, stored as `music/th07_NN.wav`). A game with mischief, slow motion or a
  loaded save state saves no high scores, clears, spell history, practice bests or replay (the
  side panel says so). **Help & updates:** an automatic or manual check for a newer release,
  **Report an issue**, **Save a log**, and a banner after a crash.
* **Backup:** **Back up to a file** saves `score.dat` (high scores, spell card history, play
  records), `th07.cfg`, the replays and the launcher's settings as one ZIP wherever you choose;
  **Restore from a backup** brings them back (same-name replays are replaced, other replays
  kept), with your replaced music if you include it. The game files and the game text are
  not in a backup: import or download them again.
* **Game text in other languages (optional):** pick a language under **Language → Game text**
  and **Download** it: the Touhou Patch Center translation's th07 files (and the patches it
  depends on) are fetched into the app's storage. If you play with the English patch on the PC
  (thcrap), keeping its `thcrap` folder in the game folder when you import it gives English
  without a download (the patch's `.js` files and `th07` folders are copied). The game then reads the patch the way thcrap applies it: dialogue and the boss
  introductions, spell card names (score.dat keeps the original names), endings and staff
  roll text, Music Room titles and comments, bomb names, translators' notes beside the
  playfield, and the patch's pictures over the game's (title and select menus, pause and
  continue menus, side panel, stage titles, result screens, Music Room, staff roll). Nothing
  of the patch ships with the app.
* Desktop development build: `cmake -S th07/src/main/cpp -B build-th07 && cmake --build build-th07`,
  then `./build-th07/th07_desktop /path/to/game/folder` (the title), or add `--play 1 --shot 0
  --difficulty 1 [--practice] [--lives N]` to start a stage, `--ending SHOT [--continued]
  [--score N]` to watch an ending, `--unlock-all` to unlock Extra, Phantasm and every practice
  stage, `--replay FILE [--replay-stage 1-7] [--replay-mode 0-2]` to play a replay or `--demo
  0-2` an attract demo; `--replay-check` compares every played frame's event flags with the
  recorded ones and logs where they first differ, `--replay-log N` logs the state every N
  frames. `--english` shows the English patch from the game folder's `thcrap` folder
  (`--thcrap DIR` names another); `--test-msg N` / `--test-spell N` show dialogue script N or
  spell card N's banner at the stage's frame 30.

## Perfect Cherry Blossom on iPhone and iPad (unsigned IPA)

Every CI run also builds TH07 for iOS (`th07-ios-<version>.ipa`, the **iOS app** job; tag
releases carry it too). It is the same game code on the same engine (SDL2, OpenGL ES 3), built
ahead of time for arm64: no JIT, no emulation. It needs iOS 16.3 or later (iPhone 8 and newer;
Apple's C++ library has the number formatting the game uses from 16.3 on).

* **Install:** the IPA is unsigned. Sideload it with your own Apple ID through AltStore,
  SideStore or Sideloadly (free accounts re-sign every 7 days; AltStore and SideStore can do
  that automatically), or sign it with a developer account for TestFlight.
* **Game files:** open the **Files** app → **On My iPhone** → **TH07 Port** and copy your own
  game folder there (th07.dat and th07.exe 1.00b, thbgm.dat for music; the folder may sit up to
  four levels down, but not inside a .zip: tap a zip in Files to unpack it), then close the app
  (swipe it away) and open it again: the files are looked for at start-up. Keep its `thcrap`
  folder for English text. Nothing of the game ships with the app.
* **Settings:** iOS Settings → **TH07 Port** has the launcher's options: English text, skip the
  title (shot, difficulty, stage, lives, practice), touch controls (drag or D-pad, drag speed,
  button size, opacity, auto-fire, latching Focus, left-handed, vibration), display (landscape
  or portrait, scaling, scanlines), practice tools and mischief. They apply at the next start.
* **Saves:** `Saves/` in the same Files folder holds score.dat, th07.cfg, replays and the logs
  (`Saves/logs/session.log` for bug reports); copy it out to back up.
* Not on iOS yet: the game-text downloads, replace music and the launcher's backup button
  (copy the `Saves` folder instead).

## Imperishable Night (th08) app: in progress

`:th08` (app id `io.github.cng605568art.th08`, "TH08 Port") is the next game on the same
engine. CI builds it as **th08-android-<version>** (releases do not carry it yet). The
launcher imports your files (th08.dat, th08.exe **1.00d**, thbgm.dat). Ported so far: the
title and its menus (Game Start, Extra Start, Spell Card Practice, Practice Start, Replay,
Player Data, Music Room, Option), every stage with its dialogue, bosses, spell cards and Last
Spells, the human/youkai gauge and time orbs, the endings and staff roll, the result screen
with high-score names, score.dat in the PC format, and replays in the PC `.rpy` format (saved
from the result screen, watched from the Replay menu with its three playback modes). Not yet:
the attract demo and Key Config (Android maps game pads itself). Replays recorded on the PC,
and the demos inside th08.dat, do not stay in sync with the port yet.

TH08 is written from clean-room behaviour specs taken from your own th08.exe and public
format documentation (thtk, thcrap). It does not use the GensokyoClub/th08 decompilation:
its authors have asked that AI ports not draw on their work.

## Languages

The launcher speaks English, 日本語, 简体中文, 繁體中文 and Bahasa Melayu (the **Language**
card at the top; "Phone setting" follows Android's language). All its text lives in
`th06/src/main/res/values*/strings.xml`, one file per language with the same keys. Chinese
uses the system's CJK fonts; the other languages use the bundled brush and Mincho faces,
subset to exactly the characters the launcher needs.

**Game text** (same card): the game's own Japanese, or a community translation from the
[Touhou Patch Center](https://www.thpatch.net/) (English, Simplified or Traditional Chinese,
Malay when one exists). **Download translation** fetches only that patch's th06 text files
(and the patches it falls back on) from thpatch's servers into the app's storage, checking
each file's CRC32; nothing is bundled with the app and nothing is copied into the game
folder. The engine applies them itself, as thcrap would: dialogue and boss title cards
(`msgN.dat.jdiff`, thcrap's box-numbering rules, checked line for line against a port of
thcrap's matcher), endings and the staff roll (`*.end.jdiff`), spell card names in play and
in the score viewer (`spells.js`), stage titles (`stages.js`), music titles in the "♪"
caption and the Music Room plus its comments (`themes.js`, `musiccmt.js`) and the bomb names
(`stringdefs.js`). Translated lines are drawn with a proportional font (condensed Latin, the
system CJK face) and narrowed to fit the original text sprites. Pictures with text (the
title menu, HUD labels) stay as in your files. Translations: Touhou Patch Center
contributors, CC BY-SA 3.0.

## Launcher look

Shrine red (primary) and sakura pink (accent) on warm paper: a red header with the app's
picture and 東方紅魔郷 in brush script, a big Play button, and white cards for Game files,
Stage select, Controls, Display, Practice tools, Mischief, Replace music, Backup and Help &
updates. Titles and buttons use **Yuji Syuku** (brush) and
text uses **Zen Antique** (antique Mincho), both SIL Open Font License 1.1, bundled as
subsets with their licences in `th06/src/main/assets/fonts/`. The app icon is an adaptive
icon (the picture over shrine red) with round and square fallbacks for older launchers.

## Layout

The code is split so more games can share it: a shared engine (`engine/`) and one module per
game (`th06/`, and `th07/` in progress). Each game is its own Android app; a fix in the engine
reaches every game.

```
.github/workflows/build.yml   CI: tests, APK, signing, releases
scripts/fetch_deps.sh         pinned SDL2 2.32.10, zstd 1.5.7, stb_image, bcdec (SHA-256 checked)
engine/native/                the shared engine (C++, namespace touhou)
  engine.cmake                thengine_core (no SDL/GL, unit-tested) and thengine_host (SDL host)
  src/host/Game               the interface a game implements (start, tick, render, ...)
  src/host/App                window, GL context, dual loop, letterbox/portrait layout, lifecycle
  src/io/                     MappedFile, BitReader, Lzss, Pbg3Archive, PkglArchive, Vfs
  src/gfx/                    Image (PNG/DDS), Anm (file tables), AnmManager (ANM VM + drawing),
                              TextureLibrary, TextRenderer, SpriteBatch (instanced GLES3, D3D8
                              blend states), RenderTarget, ProceduralArt (touch control art)
  src/audio/                  MusicPlayer (WAV + .pos looping, fades), AudioMixer (SDL audio)
  src/input/                  InputState, VirtualController (touch → SDL virtual pad), DragPad, InputMapper
  src/text/PatchStack         Touhou Patch Center translation patches: stacking, variants, markup
  src/core/                   ZunMath (ZUN's RNG, angles), FixedTimestep (60 Hz), Json, Sha256, Log
  src/platform/               data mounting, crash handler, Android JNI glue
th06/                         Embodiment of Scarlet Devil: the Android app module
  build.gradle                AGP config, externalNativeBuild → CMake, signing from env vars
  src/main/java/...           ImportActivity (launcher), GameActivity (SDL host + JNI), DataSources
  src/main/cpp/
    CMakeLists.txt            libSDL2.so + libmain.so (also a Linux desktop dev build)
    th06.cmake                th06_game: the game code, shared with the tests
    src/th06/Th06Game         the game as the host runs it: options, saves, haptics
    src/th06/Ecl              ECL file layout (packed structs), opcode tables
    src/th06/EnemyManager     timeline + ECL VM, enemies
    src/th06/BulletManager    640-slot bullet manager and the 64 lasers
    src/th06/ItemManager      512-slot items, pickups, drops
    src/th06/Resources        graze, rolling score, extends, stage-clear bonus
    src/th06/Player           player movement, hitbox, orbs, shots
    src/th06/ShotData         reads the shot tables from your 東方紅魔郷.exe (PE32, SHA-256 checked)
    src/th06/Bomb, ScreenShake  the four player bombs; screen shake
    src/th06/EffectManager    512-slot particle effects, spell circles
    src/th06/Gui              side panel, boss HUD, names, messages, score popups, ASCII text, dialogue
    src/th06/StageBackground  STD stage backgrounds: script, camera, fog, culling, spell-card fade
    src/th06/StageMenu        pause menu and continue menu
    src/th06/Title, MusicRoom title screen and menus, replay list, attract demo; Music Room
    src/th06/ScoreFile, ResultScreen   score.dat; the result screen (name entry, statistics)
    src/th06/Replay           .rpy files: recording, saving, playback
    src/th06/Ending           the ending / staff roll script interpreter
    src/th06/SoundPlayer      SoundIdx table, frame queue
    src/th06/Translation      which patch files th06 reads and how (dialogue box numbering)
    src/th06/Stage            loads a stage from the archives, runs one tick, draws
th07/                         Perfect Cherry Blossom: the second Android app module
  build.gradle                same SDK, NDK, ABIs and signing as th06; app id ...th07
  src/main/java/...           LauncherActivity (import, start options), GameFiles (copies and
                              checks th07.dat / th07.exe / thbgm.dat), GameActivity (SDL host + JNI)
  src/main/cpp/               CMakeLists.txt (libSDL2.so + libmain.so, desktop build), th07.cmake,
                              src/th07/ (Th07Game hosts the Supervisor: title and select
                              screens, games, results, ending, Music Room, score.dat,
                              replays and the attract demo)
tests/                        host tests run by CI before every APK build (engine core, th06, th07)
```

Local builds (optional, Linux/macOS): `bash scripts/fetch_deps.sh`, then
`cmake -S tests -B build-tests && cmake --build build-tests && ctest --test-dir build-tests`.
With Gradle 9.4.1 and the Android SDK installed: `gradle :th06:assembleRelease`. A Linux desktop
build for development: `cmake -S th06/src/main/cpp -B build-desktop && cmake --build build-desktop`,
then `./build-desktop/th06_desktop /path/to/game/folder` (`--seed N --lockstep` make runs
repeatable; see `engine/native/src/host/App.hpp` for the other developer options).

## Credits

Format knowledge, not code: the PBG3/LZSS layout follows [thtk](https://github.com/thpatch/thtk);
ECL/ANM/bullet/player semantics and tables follow the CC0
[th06 decompilation](https://github.com/happyhavoc/th06); the PKGL layout follows the
MIT-licensed notes in [th06nc-re](https://github.com/creeperkafasi/th06nc-re). Libraries:
SDL2 (zlib), Zstandard (BSD), stb_image (public domain), bcdec (MIT).

Touhou Project is © Team Shanghai Alice. This is an unofficial fan project; you need your own
legally obtained copy of the game.
