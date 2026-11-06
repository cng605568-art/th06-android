// Entry point. On Android, SDL.h renames main() to SDL_main(), which SDLActivity calls
// on its native thread after loading libSDL2.so and libmain.so.
#include <memory>

#include <SDL.h>

#include "host/App.hpp"
#include "th06/Th06Game.hpp"

int main(int argc, char* argv[]) {
    touhou::App app(std::make_unique<touhou::th06::Th06Game>());
    return app.run(argc, argv);
}
