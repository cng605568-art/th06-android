#include "platform/DataMount.hpp"

#include <cstdlib>
#include <string>

#include <SDL.h>

#include "core/Log.hpp"

#if defined(__ANDROID__)
#include "platform/AndroidStorage.hpp"
#endif

namespace th06 {

std::unique_ptr<Vfs> mountGameData(int argc, char** argv) {
    auto vfs = std::make_unique<Vfs>();

#if defined(__ANDROID__)
    (void)argc;
    (void)argv;
    for (const AndroidDataSource& src : listAndroidDataSources()) {
        const int fd = openAndroidDataSource(src.uri);
        if (fd < 0) {
            log::warn("Cannot open {} (permission revoked or file moved?)", src.name);
            continue;
        }
        std::string err;
        auto file = MappedFile::adoptFd(fd, &err);
        if (src.format == "WAV" || src.format == "SCORE") {  // music ("bgm/th06_NN.wav") and score.dat: loose files
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
#else
    std::string dir = "gamedata";
    if (argc > 1 && argv[1] && argv[1][0] != '-') {
        dir = argv[1];
    } else if (const char* env = std::getenv("TH06_DATA")) {
        dir = env;
    }
    vfs->mountDirectory(dir, std::getenv("TH06_PREFER_NC") != nullptr);
#endif

    if (vfs->fileCount() == 0) {
        log::info("No game archives mounted: running with generated stand-in art");
    } else {
        for (const std::string& line : vfs->describe()) log::info("  {}", line);
    }
    return vfs;
}

}  // namespace th06
