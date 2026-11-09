#pragma once

// The app around the game on a phone: Android (the launcher's GameActivity, over JNI) or
// iOS (the Settings app's values and UIKit). The desktop build has none of this and takes
// its options from the command line instead.

#if defined(__ANDROID__) || defined(TOUHOU_IOS)
#define TOUHOU_PHONE 1

#include <memory>
#include <string>
#include <vector>

#include "gfx/TextRenderer.hpp"
#include "host/DisplayOptions.hpp"
#include "input/ControlOptions.hpp"

namespace touhou {

// A game's own options as an int array, named after the Android launcher's methods
// ("getGameOptions", "getControlOptions", "getDisplayOptions"); the same layout on iOS.
// Empty when not available.
std::vector<int> platformIntArray(const char* name);
ControlOptions readPlatformControlOptions();
DisplayOptions readPlatformDisplayOptions();
// A short vibration (a miss, a bomb).
void platformVibrate(int milliseconds);
// The system fonts drawing the game's Shift-JIS text and translations.
std::unique_ptr<TextRenderer> makePlatformTextRenderer();
// The chosen game-text translation's patch folders, lowest priority first (empty: none).
std::vector<std::string> readPlatformGameTextStack();

#if defined(TOUHOU_IOS)
// iOS: the folder holding the player's game files (the app's Documents folder if it has a
// .dat archive, otherwise the first folder inside it, two levels deep at most, that has one;
// ends in '/'), and the app's own storage (Documents/Saves/, so saves, replays and logs
// show in the Files app).
std::string iosGameDataDir();
std::string iosStorageDir();
// iOS: the screen's framebuffer object, which SDL creates (it is not 0 on iOS).
unsigned iosScreenFramebuffer(void* sdlWindow);
#endif

}  // namespace touhou

#endif
