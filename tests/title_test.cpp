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

    return 0;
}

