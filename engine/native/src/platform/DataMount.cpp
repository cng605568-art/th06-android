#include "platform/DataMount.hpp"

#include <cstdlib>
#include <string>

#include <SDL.h>

#include "core/Log.hpp"

#if defined(__ANDROID__)
#include "platform/AndroidStorage.hpp"
#endif
#include "platform/Platform.hpp"

namespace touhou {

std::unique_ptr<Vfs> mountGameData(int argc, char** argv, const char* envPrefix) {
    auto vfs = std::make_unique<Vfs>();

#if defined(__ANDROID__)
    (void)argc;
    (void)argv;
    (void)envPrefix;
    for (const AndroidDataSource& src : listAndroidDataSources()) {
        const int fd = openAndroidDataSource(src.uri);
        if (fd < 0) {
            log::warn("Cannot open {} (permission revoked or file moved?)", src.name);
            continue;
        }
        std::string err;
        auto file = MappedFile::adoptFd(fd, &err);
        // Loose files: th06's music ("bgm/th06_NN.wav"), th07's (thbgm.dat) and score.dat.
        if (src.format == "WAV" || src.format == "ZWAV" || src.format == "SCORE") {
            if (file) vfs->addLooseFile(src.name, std::move(file));
            continue;
        }
        if (src.format == "EXE") {
            if (file && Vfs::looksLikeGameExecutable(file->bytes())) vfs->addExecutable(src.name, std::move(file));
            continue;
        }
        auto archive = file ? openArchive(src.name, std::move(file), &err) : nullptr;
        if (!archive) {
            log::warn("Skipping {}: {}", src.name, err);
            continue;
        }
        vfs->mount(std::move(archive));
    }
    if (const char* ext = SDL_AndroidGetExternalStoragePath()) {
        vfs->mountDirectory(std::string(ext) + "/gamedata", false);
    }
#elif defined(TOUHOU_IOS)
    // iOS: the player copies the game folder into the app's Documents (the Files app shows
    // it as "On My iPhone > TH07 Port"); the folder with the game's archive is mounted.
    (void)argc;
    (void)argv;
    (void)envPrefix;
    vfs->mountDirectory(iosGameDataDir(), false);
#else
    const std::string prefix = envPrefix ? envPrefix : "";
    std::string dir = "gamedata";
    if (argc > 1 && argv[1] && argv[1][0] != '-') {
        dir = argv[1];
    } else if (const char* env = std::getenv((prefix + "_DATA").c_str())) {
        dir = env;
    }
    vfs->mountDirectory(dir, std::getenv((prefix + "_PREFER_NC").c_str()) != nullptr);
#endif

    if (vfs->fileCount() == 0) {
        log::info("No game archives mounted: running with generated stand-in art");
    } else {
        for (const std::string& line : vfs->describe()) log::info("  {}", line);
    }
    return vfs;
}

}  // namespace touhou
