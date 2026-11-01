#pragma once

#include <string>
#include <vector>

namespace th06 {

struct AndroidDataSource {
    std::string name;    // display name, e.g. "紅魔郷CM.DAT" or "th06CM.dat"
    std::string uri;     // content:// document URI
    std::string format;  // "PBG3" or "PKGL" (sniffed by the launcher)
};

// Calls GameActivity.getDataSources() over JNI (archives in mount order).
std::vector<AndroidDataSource> listAndroidDataSources();
// Calls GameActivity.openDataSource(uri); returns an owned fd or -1.
int openAndroidDataSource(const std::string& uri);

struct AndroidGameOptions {
    int shotType = 0;    // 0 Reimu A, 1 Reimu B, 2 Marisa A, 3 Marisa B
    int difficulty = 1;  // 0 Easy .. 3 Lunatic
    int stage = 1;       // 1..6, 7 = Extra
};
// Calls GameActivity.getGameOptions() (the launcher's choices).
AndroidGameOptions readAndroidGameOptions();

}  // namespace th06
