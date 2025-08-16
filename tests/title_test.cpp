#define ROCKTRAINER_NO_MAIN
#include "../src/main.cpp"
#include <cassert>

int main() {
    App app{};
    SDL_Event e{};

    int itemH = 60;
    int menuCount = 5; // number of items in kMenu
    int startY = app.rs.h/2 - menuCount*itemH/2;

    // Mouse motion should update highlighted menu item
    e.type = SDL_MOUSEMOTION;
    e.motion.x = app.rs.w/2;
    e.motion.y = startY + 1*itemH + itemH/2; // hover second item
    updateTitle(app, e);
    assert(app.menuIndex == 1);

    // Mouse click should change state to selected item
    app.state = AppState::Title;
    e.type = SDL_MOUSEBUTTONDOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.x = app.rs.w/2;
    e.button.y = startY + 2*itemH + itemH/2; // click third item (Free Play)
    updateTitle(app, e);
    assert(app.state == AppState::FreePlay);

    // Rendering should draw menu text
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, app.rs.w, app.rs.h, 32, SDL_PIXELFORMAT_RGBA8888);
    app.rs.r = SDL_CreateSoftwareRenderer(surface);

    renderTitle(app);

    Uint32 bg = SDL_MapRGBA(surface->format, 80, 80, 80, 255);
    bool found = false;
    int scale = 4;
    int y = startY + itemH; // second item (unselected)
    int textY = y + (itemH - 8*scale)/2;
    int pitch = surface->pitch / 4;
    Uint32* pixels = static_cast<Uint32*>(surface->pixels);
    for (int py = textY; py < textY + 8*scale && !found; ++py) {
        for (int px = app.rs.w/3; px < 2*app.rs.w/3 && !found; ++px) {
            if (pixels[py*pitch + px] != bg) found = true;
        }
    }
    assert(found);

    SDL_DestroyRenderer(app.rs.r);
    SDL_FreeSurface(surface);
    SDL_Quit();

    return 0;
}

