#pragma once

// What Imperishable Night's screens outside a stage share (claude/th08-specs/
// spec_flow_menus.md): the scene numbers, the menu input with its pressed and auto-repeat
// rules (same as TH07), the flow context, and small drawing helpers: picture backgrounds
// ("surfaces"), ASCII strings from ascii.anm, Shift-JIS text rendered into text.anm sprites
// and full-screen fades.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/TextRenderer.hpp"
#include "th08/Anm.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
struct UiArt;
}  // namespace touhou

namespace touhou::th08 {

struct ScoreData;

class BgmPlayer;
class SoundPlayer;
struct ExeTables;
struct GameConfig;

// The game's button word.
enum Button : std::uint16_t {
    BtnShoot = 0x0001, BtnBomb = 0x0002, BtnFocus = 0x0004, BtnMenu = 0x0008,
    BtnUp = 0x0010, BtnDown = 0x0020, BtnLeft = 0x0040, BtnRight = 0x0080,
    BtnSkip = 0x0100, BtnQ = 0x0200, BtnS = 0x0400, BtnHome = 0x0800, BtnEnter = 0x1000, BtnD = 0x2000, BtnR = 0x4000,
};
inline constexpr std::uint16_t kSelect = BtnShoot | BtnEnter;  // 0x1001
inline constexpr std::uint16_t kCancel = BtnBomb | BtnMenu;    // 0x000A

// The Supervisor's scenes (spec 1.2; TH07's numbers).
enum class Scene : int {
    Exit = -1, Init = 0, MainMenu = 1, Game = 2, NextStage = 3, ResultViewer = 5, ResultAfterGame = 6,
    ReplayEnd = 7, MusicRoom = 8, Ending = 9, RestartGame = 10, RetryStage = 11,
};

// Menu sound ids (spec: SE table 0x4C8040).
inline constexpr int kSndPlst = 0, kSndSelect = 10, kSndBack = 11, kSndMove = 12, kSndTimeout = 29, kSndPause = 34,
                     kSndInvalid = 41;

// ANM slots (spec_anm_text §1.7).
inline constexpr int kSlotText = 0, kSlotAscii = 1, kSlotNowLoading = 2, kSlotCapture = 3, kSlotTitle = 20, kSlotResultText = 22;

// The raw button word as menus read it: held, pressed (any bit of the mask newly set), and the
// auto-repeat flag (the whole word unchanged for 33 passes, then every 8) - as TH07.
class MenuInput {
  public:
    void sample(std::uint16_t raw);
    void clear() {
        current_ = previous_ = 0;
        repeat_ = false;
    }
    std::uint16_t current() const { return current_; }
    bool repeating() const { return repeat_; }
    bool held(std::uint16_t mask) const { return (current_ & mask) != 0; }
    bool pressed(std::uint16_t mask) const { return (current_ & mask) != 0 && (current_ & mask) != (previous_ & mask); }
    bool pressedOrRepeat(std::uint16_t mask) const { return pressed(mask) || (held(mask) && repeat_); }
    // Passes the whole word has been held unchanged (it stays at 30 or more once reached).
    int holdCount() const { return counter_; }

  private:
    std::uint16_t current_ = 0, previous_ = 0;
    int counter_ = 0;
    bool repeat_ = false;
};

// Everything the flow screens share; owned by the game.
struct FlowContext {
    const Vfs* vfs = nullptr;
    TextureLibrary* textures = nullptr;
    TextRenderer* text = nullptr;
    const UiArt* ui = nullptr;
    AnmLibrary* anm = nullptr;
    ZunRng* rng = nullptr;
    SoundPlayer* sound = nullptr;
    BgmPlayer* music = nullptr;
    const ExeTables* tables = nullptr;
    GameConfig* config = nullptr;

    std::function<void(Scene)> request;  // acted on at the start of the next pass
    std::function<void()> saveConfig;    // writes th08.cfg
    // Called whenever the game plays a song (the music-play wrappers): marks its Music Room
    // track heard (spec §4.9). Not called for the Music Room's own playback.
    std::function<void(std::string_view path)> musicPlayed;
    const std::array<std::uint8_t, 32>* musicHeard = nullptr;  // score.dat's PLST flags
    const ScoreData* scores = nullptr;   // score.dat as read at boot
    // The same records, kept and changed by the game (GameManager), and written by the
    // result screen's write-through (mode 2); `gameRng`: the two header bytes come from the
    // game's RNG (the write at a fresh set-up).
    ScoreData* records = nullptr;
    std::function<void(bool gameRng)> saveScores;
    Scene previous = Scene::Init;        // the Supervisor's previous scene

    // Unlocks (spec 5.1), from score.dat once it is ported.
    bool extraUnlocked = false, spellPracticeUnlocked = false, solosUnlocked = false;
    int totalPlays = 0;  // PLST's total play count (the Option menu's lives range)

    // Remembered selections (spec 1.5, 6.1): the difficulty lives in the configuration.
    int character = 0;   // 0-3 teams, 4-11 solo characters
    int stage = 0;       // stage index 0-8 (the spell practice menus: 0-9, 9 = Last Word)
    bool practice = false, spellPractice = false;
    // Spell Card Practice: the card last chosen (GM+0x3DBB0, 0-based) and its difficulty (the
    // game's, not the remembered one; Last Words play on Normal).
    int card = 0;
    int spellDifficulty = 1;

    void playSound(int id) const;
    void notePlayed(std::string_view path) const {
        if (musicPlayed) musicPlayed(path);
    }
};

// A picture from the archive (JPEG or PNG) as an opaque background, drawn 1:1 at integer
// pixels.
class Surface {
  public:
    bool load(const Vfs& vfs, TextureLibrary& textures, const std::string& path);
    void release() { texture_ = -1; }
    bool loaded() const { return texture_ >= 0; }
    const std::string& path() const { return path_; }
    void blit(SpriteSink& sink, float x, float y) const;
    // The w x h window at (left, top) of the picture, drawn at (x, y).
    void blitRect(SpriteSink& sink, float x, float y, float left, float top, float w, float h) const;
    int height() const { return height_; }

  private:
    int texture_ = -1;
    int width_ = 0, height_ = 0;
    std::string path_;
};

// Strings in the ASCII font (ascii.anm, file-local: byte c is sprite c - 1), queued during a
// frame and drawn in order. Glyph advance 13 (TH07: 14).
class AsciiText {
  public:
    // `xScale` < 0: the same as `scale` (the glyphs' horizontal scale; the advance uses `scale`).
    // `selected`: the highlighted glyph set (sprite c + 138), drawn white.
    void add(float x, float y, std::string_view text, std::uint32_t color = 0xFFFFFFFFu, float scale = 1.0f, float advance = 13.0f,
             float xScale = -1.0f, bool selected = false);
    void draw(SpriteSink& sink, AnmLibrary& anm);
    void clear() { items_.clear(); }

  private:
    struct Item {
        float x, y;
        std::string text;
        std::uint32_t color;
        float scale, advance, xScale;
        bool selected;
    };
    std::vector<Item> items_;
};

// Renders Shift-JIS text into the band of the VM's sprite (the whole texture width) and makes
// the VM visible (spec_anm_text §6.2). `colorRef` is a GDI colour (0x00BBGGRR).
void renderSpriteText(TextureLibrary* textures, TextRenderer* text, AnmVm& vm, std::string_view sjis, std::uint32_t colorRef,
                      TextAlign align = TextAlign::Left);

// printf into a string with a format read from the exe (Shift-JIS trail bytes are never '%').
std::string sjisPrintf(const char* format, ...);

// A full-screen colour overlay (fades), colour 0xAARRGGBB.
void fillScreen(SpriteSink& sink, const UiArt* ui, std::uint32_t argb);
// An untextured rectangle with a vertical gradient (0x45B490): `top` and `bottom` 0xAARRGGBB.
void gradientRect(SpriteSink& sink, const UiArt* ui, float x0, float y0, float x1, float y1, std::uint32_t top, std::uint32_t bottom);

}  // namespace touhou::th08
