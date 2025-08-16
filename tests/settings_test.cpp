#define ROCKTRAINER_NO_MAIN
#include "../src/main.cpp"
#include <cassert>
#include <cstdio>

int main() {
    SettingsState s{};
    s.audioDeviceIndex = 3;
    s.bufferSize = 256;
    s.latencyOffset = 42;
    s.vsync = false;
    s.width = 800;
    s.height = 600;
    s.stringColors[0] = SDL_Color{1,2,3,255};
    s.stringColors[5] = SDL_Color{4,5,6,255};

    const char* path = "test_config.json";
    assert(saveConfig(path, s));

    SettingsState loaded{};
    loaded.vsync = true;
    loaded.width = 1;
    loaded.height = 1;
    loaded.audioDeviceIndex = -1;
    loaded.bufferSize = 0;
    loaded.latencyOffset = 0;

    assert(loadConfig(path, loaded));

    assert(loaded.audioDeviceIndex == s.audioDeviceIndex);
    assert(loaded.bufferSize == s.bufferSize);
    assert(loaded.latencyOffset == s.latencyOffset);
    assert(loaded.vsync == s.vsync);
    assert(loaded.width == s.width);
    assert(loaded.height == s.height);
    assert(loaded.stringColors[0].r == s.stringColors[0].r);
    assert(loaded.stringColors[0].g == s.stringColors[0].g);
    assert(loaded.stringColors[0].b == s.stringColors[0].b);
    assert(loaded.stringColors[5].r == s.stringColors[5].r);
    assert(loaded.stringColors[5].g == s.stringColors[5].g);
    assert(loaded.stringColors[5].b == s.stringColors[5].b);

    std::remove(path);
    return 0;
}

