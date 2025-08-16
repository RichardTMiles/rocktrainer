#define ROCKTRAINER_NO_MAIN
#include "../src/main.cpp"
#include <cassert>

int main() {
    // Hit scenario
    App app{};
    NoteEvent n{};
    n.t_ms = 0; n.str = 6; n.fret = 24; n.len_ms = 100;
    app.chart.notes.push_back(n);
    g_detectedHz.store(midiToHz(kStringOpenMidi[5]), std::memory_order_relaxed); // high E open
    updateGameplay(app, 0);
    assert(app.stats.hits == 1);
    assert(app.stats.combo == 1);
    assert(app.stats.misses == 0);
    assert(app.stats.accuracy > 99.0f);

    // Miss scenario
    App app2{};
    app2.chart.notes.push_back(n);
    g_detectedHz.store(0.0f, std::memory_order_relaxed);
    updateGameplay(app2, 200); // past hit window
    assert(app2.stats.misses == 1);
    assert(app2.stats.combo == 0);
    assert(app2.stats.accuracy == 0.0f);
    return 0;
}
