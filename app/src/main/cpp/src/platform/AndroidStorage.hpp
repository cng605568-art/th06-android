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

}  // namespace th06
