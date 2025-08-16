#include "../src/chart.hpp"
#include <fstream>
#include <cassert>

int main() {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "simple.mss";
    std::ofstream f(tmp);
    f << R"({
  "meta": {"bpm": 120, "title": "Test", "tuning": [40,45,50,55,59,64]},
  "measures": [
    {"notes": [
      {"beat": 0.0, "string": 1, "fret": 0, "sustain": 1.0}
    ]}
  ]
})";
    f.close();
    auto chartOpt = loadChartMss(tmp);
    fs::remove(tmp);
    assert(chartOpt);
    const Chart& c = *chartOpt;
    assert(c.notes.size() == 1);
    const NoteEvent& n = c.notes[0];
    assert(n.t_ms == 0);
    assert(n.str == 1);
    assert(n.fret == 0);
    assert(n.len_ms == 500); // 1 beat at 120 BPM
    assert(c.tuning[0] == 40);
    return 0;
}
