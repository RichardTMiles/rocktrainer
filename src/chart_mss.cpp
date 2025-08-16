#include "chart.hpp"
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

std::optional<Chart> loadChartMss(const fs::path& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  json j; f >> j;
  Chart c;
  if (j.contains("meta")) {
    auto m = j["meta"];
    if (m.contains("bpm"))   c.bpm = m["bpm"].get<double>();
    if (m.contains("title")) c.title = m["title"].get<std::string>();
    if (m.contains("tuning") && m["tuning"].is_array() && m["tuning"].size()==6) {
      for (int i=0;i<6;++i) {
        if (m["tuning"][i].is_number_integer())
          c.tuning[i] = m["tuning"][i].get<int>();
      }
    }
  }
  double beatMs = 60000.0 / c.bpm;
  if (j.contains("measures") && j["measures"].is_array()) {
    int measureIdx = 0;
    for (auto& mj : j["measures"]) {
      double measureStartBeats = measureIdx * 4.0; // assume 4/4
      if (mj.contains("notes") && mj["notes"].is_array()) {
        for (auto& n : mj["notes"]) {
          NoteEvent e{};
          e.str  = n.value("string", 1);
          e.fret = n.value("fret", 0);
          double beat = n.value("beat", 0.0) + measureStartBeats;
          e.t_ms = static_cast<int64_t>(std::llround(beat * beatMs));
          double sus = n.value("sustain", 0.0);
          e.len_ms = static_cast<int64_t>(std::llround(sus * beatMs));
          c.notes.push_back(e);
        }
      }
      ++measureIdx;
    }
  }
  std::sort(c.notes.begin(), c.notes.end(),
            [](const NoteEvent& a, const NoteEvent& b){ return a.t_ms < b.t_ms; });
  return c;
}
