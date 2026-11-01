#pragma once

// The 2002 game's title screen and the screens before a game, from the TL archive:
//   title00.jpg behind title01..04.anm (80 animations: the menu items, the Option rows and
//   values, decorations), "press a key", the main menu (Start, Extra Start, Practice Start,
//   Replay, Result, Music Room, Option, Quit), the Option menu (lives, bombs, music, sound
//   effects, Default), then select00.jpg behind select01..05 / slpl00a..01b.anm for the
//   difficulty, character, shot type and practice stage choices.
// Animations are driven by interrupts as in the original; the code only tints and nudges
// the item under the cursor. Result (the score viewer) and Music Room are screens of their
// own (the title hands over to them). The original's demo play, replays and Key Config are
// not in the port: Replay plays the cancel sound.

#include <array>
#include <cstdint>
#include <string>

#include "game/GameConfig.hpp"
#include "game/Input.hpp"
#include "gfx/AnmManager.hpp"
#include "gfx/SpriteSink.hpp"

namespace th06 {

class Gui;
class ScoreFile;
class TextureLibrary;
class Vfs;
struct World;

// What the title remembers between visits (the original keeps it in the game state).
struct TitleSession {
    int difficulty = 1;     // of the last game (4 = Extra)
    int character = 0;      // 0 Reimu, 1 Marisa: the character select starts here
    int shot = 0;           // 0 A, 1 B: the shot select starts here
    int practiceStage = 0;  // the practice select starts here (0-based)
    bool practice = false;  // Practice Start was chosen (cleared when the next title opens)
};

class Title {
  public:
    static constexpr int kVms = 122;
    enum class Sub {
        Startup, PreInput, MainMenu, DifficultyLoad, DifficultySelect, BackToTitle, CharacterSelect,
        ShotSelect, PracticeSelect, Options, Quit, Score, Music,
    };
    enum class Result { None, StartGame, Quit, Failed, OpenScores, OpenMusicRoom };

    // The game the player chose.
    struct Choice {
        int difficulty = 1, shotType = 0;
        int stage = 1;  // 1..7
        int lives = 2, bombs = 3;
        bool practice = false;
    };

    // Opens the title (the original's title object creation): the initial cursor from where
    // the player came from, the title theme (`firstVisit`: a 200-pass white fade-in, else
    // 120), and the title graphics. False if the title files are missing.
    bool start(World& world, const Vfs& vfs, TextureLibrary& textures, const ScoreFile& scores, GameConfig& config,
               TitleSession& session, int cursor, bool firstVisit);
    // One logic pass. StartGame: choice() holds the game (the title is gone). Quit: the
    // player left. OpenScores / OpenMusicRoom: Result or Music Room was chosen (60 passes
    // ago). Failed: the select-screen files could not be loaded.
    Result update(World& world, const InputState& in);
    void draw(World& world, Gui& gui, SpriteSink& sink, const SpriteRegion& white);
    // Releases the title and select files (when the game starts).
    void release(World& world);

    const Choice& choice() const { return choice_; }
    // The Option menu (or a choice) changed the settings: write th06.cfg.
    bool takeConfigChanged() {
        const bool c = configChanged_;
        configChanged_ = false;
        return c;
    }

    // Inspection (tests).
    Sub sub() const { return sub_; }
    int timer() const { return timer_; }
    int cursor() const { return cursor_; }
    int optionCursor() const { return optCursor_; }
    int selectCursor() const { return selCursor_; }
    int practiceStages() const { return practiceStages_; }
    bool extraUnlocked() const { return extraUnlocked_; }
    bool extraUnlocked(int shotType) const;
    const AnmVm& vm(int i) const { return vms_[static_cast<std::size_t>(i)]; }
    int overlayAlpha() const;
    int whiteFadeAlpha() const { return fadeOn_ ? fadeAlpha_ : 0; }
    bool drawsBackground() const { return haveBackground_; }

  private:
    struct Edges {
        bool up = false, down = false, left = false, right = false;  // pressed or repeating
        bool confirm = false, cancel = false, q = false;              // pressed
        bool anyKey = false;  // Shoot, Bomb, Esc or Q pressed or repeating (pre-input)
    };
    Edges readInput(const InputState& in);
    bool loadTitleGraphics(World& world);
    bool loadSelectGraphics(World& world);
    void releaseFiles(World& world, int first, int last, int vmFirst, int vmLast);
    void setBackground(const char* path);
    void interruptAll(std::int16_t k);
    void cancel(int vm) { vms_[static_cast<std::size_t>(vm)].pendingInterrupt = 0; }
    void tint(int vm, std::uint32_t color, float dx, float dy);
    void playTitleTheme(World& world);

    void openMainMenu();
    Result mainMenu(World& world, const Edges& e);
    void choose(World& world);
    void difficultySelect(World& world, const Edges& e);
    void enterCharacterSelect(World& world);
    void characterSelect(World& world, const Edges& e);
    void enterShotSelect(World& world);
    Result shotSelect(World& world, const Edges& e);
    void enterPracticeSelect(World& world);
    Result practiceSelect(World& world, const Edges& e);
    void options(World& world, const Edges& e);
    void colourOptions();
    void optionStep(World& world, int step);
    Result startGame(World& world);
    void dropRiders(int keepCharacter);
    int difficultyCard() const;
    void stepOverlay();

    const Vfs* vfs_ = nullptr;
    TextureLibrary* textures_ = nullptr;
    const ScoreFile* scores_ = nullptr;
    GameConfig* config_ = nullptr;
    TitleSession* session_ = nullptr;
    std::array<AnmVm, kVms> vms_{};
    Sub sub_ = Sub::Startup;
    int timer_ = 0;
    int cursor_ = 0;      // main menu
    int optCursor_ = 0;   // Option menu row
    int selCursor_ = 0;   // the select screen's cursor (difficulty, character, shot or stage)
    int character_ = 0;   // during the shot / practice select
    bool extraUnlocked_ = false;
    int practiceStages_ = 1;
    bool titleLoaded_ = false;
    Choice choice_;
    bool configChanged_ = false;
    // Background surface 0.
    std::uint16_t background_ = 0;
    bool haveBackground_ = false;
    float bgWidth_ = 640.0f, bgHeight_ = 480.0f;
    // The dark rectangle over the background (target colour, minimum alpha, counter, ramps).
    std::uint32_t overlayColor_ = 0x40000000u;
    int overlayMin_ = 0, overlayN_ = 0, overlayOn_ = 0, overlayOff_ = 0;
    // The white fade-in when the title opens.
    bool fadeOn_ = false;
    int fadeT_ = 0, fadeN_ = 0, fadeAlpha_ = 0;
    // Input: previous buttons and the auto-repeat counter (whole input unchanged).
    std::uint32_t prevButtons_ = 0;
    int repeatCounter_ = 0;
};

}  // namespace th06
