#pragma once

// What the screens outside a stage share (the supervisor's world): the supervisor states,
// the menu input with its pressed and auto-repeat rules, the remembered selections and the
// game that just ended, and small drawing helpers for menu screens: JPEG backgrounds
// ("surface 0"), ASCII strings from ascii.anm, and Shift-JIS text rendered into text.anm
// sprites.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gfx/TextRenderer.hpp"
#include "th07/Anm.hpp"
#include "th07/ScoreFile.hpp"
#include "th07/World.hpp"

namespace touhou {
class SpriteSink;
class TextureLibrary;
class Vfs;
struct UiArt;
}  // namespace touhou

namespace touhou::th07 {

class BgmPlayer;
class English;
class SoundPlayer;
struct ExeTables;
struct GameConfig;
struct ReplayFile;

// The supervisor's states (spec 1.4.1).
enum class FlowState : int {
    Exit = -1,
    Init = 0,
    MainMenu = 1,
    Game = 2,
    NextStage = 3,
    ExitError = 4,
    ResultViewer = 5,
    ResultAfterGame = 6,
    ReplayEnd = 7,
    MusicRoom = 8,
    Ending = 9,
    RestartGame = 10,
    RestartStage = 11,      // never issued by 1.00b
    NextStageVariant = 12,  // never issued by 1.00b
};

// Menu sound ids.
inline constexpr int kSndSelect = 10, kSndBack = 11, kSndMove = 12, kSndExtend = 28;

// The raw button word as menus read it (spec 1.5.4): held, pressed (any bit of the mask newly
// set: the masked word is non-zero and differs from the previous pass's), and the auto-repeat
// flag (the whole word unchanged for 33 passes, then every 8).
class MenuInput {
  public:
    void sample(std::uint16_t raw);
    // After a supervisor transition: nothing is held for the rest of the pass.
    void clear() {
        current_ = previous_ = 0;
        repeat_ = false;
    }
    // The pass after a slow-mode skip: the new sample is ORed in, nothing shifts (spec 1.5.4).
    void merge(std::uint16_t raw) { current_ = static_cast<std::uint16_t>(current_ | raw); }
    std::uint16_t current() const { return current_; }
    std::uint16_t previous() const { return previous_; }
    bool repeating() const { return repeat_; }
    bool held(std::uint16_t mask) const { return (current_ & mask) != 0; }
    bool pressed(std::uint16_t mask) const { return (current_ & mask) != 0 && (current_ & mask) != (previous_ & mask); }
    bool pressedOrRepeat(std::uint16_t mask) const { return pressed(mask) || (held(mask) && repeat_); }

  private:
    std::uint16_t current_ = 0, previous_ = 0;
    int counter_ = 0;
    bool repeat_ = false;
};

// Everything the flow screens share; owned by the supervisor.
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
    ScoreData* scores = nullptr;
    const English* english = nullptr;  // the English text mode (null: the original text)

    // Requests a supervisor state (acted on at the start of the next pass).
    std::function<void(FlowState)> request;
    FlowState previous = FlowState::Init;  // the supervisor's previous state
    // score.dat: the current image (nullopt: missing or invalid) and a full write.
    std::function<std::optional<ScoreImage>()> readScores;
    std::function<void(const std::array<ScoreList, kDifficulties * kShots>&, const std::array<char, 12>&,
                       const std::array<std::uint8_t, 0x1C>*)>
        writeScores;
    std::function<void()> saveConfig;
    std::function<void()> refreshRunningTime;  // brings PLST's running time up to date

    // Remembered selections (spec 3.8.5): the difficulty lives in the configuration.
    int character = 0, shotType = 0;
    bool practice = false;
    int stage = 0;  // the game state's stage number (the practice stage to come back to)
    bool replay = false, demo = false;
    // Replays (spec 7): their folder (the app's storage + "replay/"; empty: none),
    // th07_0100b.ver for their check, the replay to play (chosen in the replay menu, or the
    // demo) with its playback mode, the demo index (the first demo of a session is 0), and the
    // result screen's save of the game's recording into a file with a name.
    std::string replayDir;
    std::span<const std::uint8_t> verFile;
    std::shared_ptr<const ReplayFile> replayFile;
    int playbackMode = 0;
    int demoIndex = 2;
    std::function<bool(const std::string& path, const std::string& name)> saveReplay;
    std::function<bool()> replayRecorded;  // the game left a recording (nothing to save otherwise)
    int maxContinues = 3;
    bool phantasmSeen = true;  // "unlocked" when the splash check last ran
    bool timingBad = false;    // never set in the port

    // The game that just ended (for the result screen and the title cursor).
    GameState lastGame;
    float lastSlow = 0.0f;
    bool lastValid = false;
    // The spell records at the start of the game (slow mode discards the game's history).
    std::array<SpellRecord, kSpellCards> spellSnapshot{};

    int shotIndex() const { return character * 2 + shotType; }
    void playSound(int id) const;
};

// A JPEG (or any decodable picture) from the archive as an opaque background, drawn 1:1 at
// integer pixels (spec 1.11).
class Surface {
  public:
    bool load(const Vfs& vfs, TextureLibrary& textures, const std::string& path);
    void release() { texture_ = -1; }
    bool loaded() const { return texture_ >= 0; }
    int width() const { return width_; }
    int height() const { return height_; }
    // Copies the picture from (left, top) to its right and bottom edges to (x, y).
    void blit(SpriteSink& sink, float x, float y, float left = 0, float top = 0) const;
    // The ending's copy: the rectangle (left, top, w, h) to (x, y); nothing if it does not fit.
    void blitRect(SpriteSink& sink, float x, float y, float left, float top, float w, float h) const;
    const std::string& path() const { return path_; }

  private:
    int texture_ = -1;
    int width_ = 0, height_ = 0;
    std::string path_;
};

// Strings in the ASCII font (ascii.anm: byte c is absolute sprite c - 1), queued during a
// frame and drawn in order.
class AsciiText {
  public:
    void add(float x, float y, std::string_view text, std::uint32_t color = 0xFFFFFFFFu, float scale = 1.0f, float advance = 14.0f);
    void draw(SpriteSink& sink, const AnmLibrary& anm);
    void clear() { items_.clear(); }

  private:
    struct Item {
        float x, y;
        std::string text;
        std::uint32_t color;
        float scale, advance;
    };
    std::vector<Item> items_;
};

// Renders Shift-JIS (or ASCII) text into the band of the VM's sprite (the whole texture width,
// spec 8 of the ANM spec) and makes the VM visible. `colorRef` is a GDI colour (0x00BBGGRR).
// Translated text (kUtf8TextMarker first) is laid out within the sprite's own rectangle,
// with the English patch's markup; a translator's note after U+0014 is not drawn.
void renderSpriteText(TextureLibrary* textures, TextRenderer* text, AnmVm& vm, std::string_view sjis, std::uint32_t colorRef,
                      TextAlign align = TextAlign::Left);

// A full-screen colour overlay (fades), colour 0xAARRGGBB.
void fillScreen(SpriteSink& sink, const UiArt* ui, std::uint32_t argb);

// The shot names as menus print them ("ReimuA ", 7 characters) and the difficulty names.
std::string_view shotName7(int shot);
std::string_view difficultyName(int difficulty);

}  // namespace touhou::th07
