#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "host/DisplayOptions.hpp"
#include "input/ControlOptions.hpp"

#include "gfx/TextRenderer.hpp"

namespace touhou {

// The game's half of the Android app: static methods on its GameActivity, called over JNI
// from SDL's main thread.

struct AndroidDataSource {
    std::string name;    // display name, e.g. "紅魔郷CM.DAT" or "th06CM.dat"
    std::string uri;     // content:// document URI
    std::string format;  // "PBG3" or "PKGL" (sniffed by the launcher)
};

// Calls GameActivity.getDataSources() over JNI (archives in mount order).
std::vector<AndroidDataSource> listAndroidDataSources();
// Calls GameActivity.openDataSource(uri); returns an owned fd or -1.
int openAndroidDataSource(const std::string& uri);

// Calls a static GameActivity method returning int[] (no arguments); empty if it is
// missing or throws. For a game's own launcher options (th06: getGameOptions()).
std::vector<int> androidIntArray(const char* method);

// Calls GameActivity.getControlOptions() (the launcher's touch-control settings).
ControlOptions readAndroidControlOptions();
// Calls GameActivity.getDisplayOptions() (portrait, scaling, scanlines).
DisplayOptions readAndroidDisplayOptions();
// A short vibration (GameActivity.vibrate).
void androidVibrate(int milliseconds);

// Renders the game's Shift-JIS strings with the system's CJK font through
// GameActivity.renderText().
std::unique_ptr<TextRenderer> makeAndroidTextRenderer();
// The launcher's game-text translation: patch folders, lowest priority first (empty: the
// original Japanese text).
std::vector<std::string> readAndroidGameTextStack();

}  // namespace touhou
