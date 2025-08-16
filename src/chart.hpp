#pragma once
#include <vector>
#include <string>
#include <array>
#include <optional>
#include <filesystem>
#include <cstdint>

struct NoteEvent {
  int64_t t_ms;   // start time in ms
  int      str;   // 1..6 (1 = high E)
  int      fret;  // 0..24
  int64_t len_ms; // duration in ms
  int      slideTo = -1;
  std::vector<std::string> techs;
};

struct Chart {
  std::vector<NoteEvent> notes;
  double bpm = 120.0;
  std::string title = "Example";
  // MIDI numbers for open strings, low (string 6) to high (string 1)
  std::array<int,6> tuning{40,45,50,55,59,64};
};

// Loaders for different chart formats
std::optional<Chart> loadChartJson(const std::filesystem::path& path);
std::optional<Chart> loadChartMss(const std::filesystem::path& path);
