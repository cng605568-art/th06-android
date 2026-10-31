// Entry point. On Android, SDL.h renames main() to SDL_main(), which SDLActivity calls
// on its native thread after loading libSDL2.so and libmain.so.
#include <SDL.h>

#include "engine/App.hpp"

int main(int argc, char* argv[]) {
    th06::App app;
    return app.run(argc, argv);
}
