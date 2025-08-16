#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <optional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <iostream>
#include <fstream>

#include <portaudio.h>
#include <aubio/aubio.h>
#include <SDL.h>

// nlohmann json (header-only). Install via package manager.
// If CMake can't find it automatically, ensure its include dir is visible.
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// --------- Config ---------
static constexpr double kSampleRate = 48000.0;
static constexpr uint_t kHopSize = 512;   // buffer size per callback
static constexpr uint_t kWinSize = 2048;  // analysis window
static constexpr float  kSilenceDb = -50.0f;
static constexpr int    kMaxFrets = 24;

// --------- Globals (simple starter) ---------
struct NoteEvent {
  int64_t t_ms;   // start time
  int      str;   // 1..6 (1 = high E)
  int      fret;  // 0..24
  int64_t len_ms; // duration
};
struct Chart {
  std::vector<NoteEvent> notes;
  double bpm = 120.0;
  std::string title = "Example";
};

static std::atomic<float> g_detectedHz{0.0f};
static std::atomic<int>   g_latencyOffsetMs{0};  // visual offset

// Standard tuning MIDI numbers for open strings (low→high): E2 A2 D3 G3 B3 E4
static const int kStringOpenMidi[6] = {40, 45, 50, 55, 59, 64};

// --------- Utils ---------
inline double hzToMidi(double hz) {
  return 69.0 + 12.0 * std::log2(hz / 440.0);
}
inline double midiToHz(double midi) {
  return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}
inline std::pair<std::string,int> midiToName(int midi) {
  static const char* names[12] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
  };
  int n = (midi + 1200) % 12;
  int octave = (midi/12) - 1;
  return {names[n], octave};
}
struct DetectedNote {
  int midi;          // nearest midi
  double cents;      // deviation from nearest
  int stringIdx;     // 0..5 (low E..high E) best match
  int fret;          // 0..24 if in range, else -1
};
inline std::optional<DetectedNote> analyzeFrequency(double hz) {
  if (hz <= 0.0) return std::nullopt;
  double midiF = hzToMidi(hz);
  int midi = (int)std::round(midiF);
  double refHz = midiToHz(midi);
  double cents = 1200.0 * std::log2(hz / refHz);

  // pick string/fret by proximity
  int bestStr = -1, bestFret = -1;
  double bestDiff = 1e9;
  for (int s = 0; s < 6; ++s) {
    int fret = midi - kStringOpenMidi[s];
    if (fret < 0 || fret > kMaxFrets) continue;
    double diff = std::abs((double)fret - (midi - kStringOpenMidi[s]));
    if (diff < bestDiff) {
      bestDiff = diff;
      bestStr = s;
      bestFret = fret;
    }
  }
  return DetectedNote{midi, cents, bestStr, bestFret};
}

// --------- Chart loader ---------
std::optional<Chart> loadChart(const std::string& path) {
  std::ifstream f(path);
  if (!f) return std::nullopt;
  json j; f >> j;
  Chart c;
  if (j.contains("meta")) {
    auto m = j["meta"];
    if (m.contains("bpm"))   c.bpm = m["bpm"].get<double>();
    if (m.contains("title")) c.title = m["title"].get<std::string>();
  }
  if (j.contains("notes") && j["notes"].is_array()) {
    for (auto& n : j["notes"]) {
      NoteEvent e{};
      e.t_ms   = n.value("t", 0);
      e.str    = n.value("str", 1);
      e.fret   = n.value("fret", 0);
      e.len_ms = n.value("len", 240);
      c.notes.push_back(e);
    }
    std::sort(c.notes.begin(), c.notes.end(),
      [](const NoteEvent& a, const NoteEvent& b){return a.t_ms < b.t_ms;});
  }
  return c;
}

// --------- PortAudio + aubio ---------
struct AudioState {
  fvec_t* inputFrame = nullptr;
  aubio_pitch_t* pitch = nullptr;
  uint_t hop = kHopSize;
};

static int audioCb(const void* input, void*, unsigned long frameCount,
                   const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags, void* userData) {
  auto* st = reinterpret_cast<AudioState*>(userData);
  if (!input) return paContinue;
  const float* in = static_cast<const float*>(input);
  // Feed aubio in hop-sized chunks
  for (unsigned long i = 0; i < frameCount; i += st->hop) {
    unsigned long chunk = std::min<unsigned long>(st->hop, frameCount - i);
    for (unsigned long j = 0; j < chunk; ++j)
      fvec_set_sample(st->inputFrame, in[i+j], j);
    for (unsigned long j = chunk; j < st->hop; ++j)
      fvec_set_sample(st->inputFrame, 0.f, j);
    // aubio outputs pitch (Hz) into an fvec
    fvec_t* out = new_fvec(1);
    aubio_pitch_do(st->pitch, st->inputFrame, out);
    float hz = fvec_get_sample(out, 0);
    if (hz > 20.f && hz < 2000.f) g_detectedHz.store(hz, std::memory_order_relaxed);
    del_fvec(out);
  }
  return paContinue;
}

struct DevicePick {
  int index = -1;
  std::string name;
};

DevicePick pickInputDevice() {
  DevicePick pick{};
  int num = Pa_GetDeviceCount();
  for (int i = 0; i < num; ++i) {
    const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
    if (!info || info->maxInputChannels < 1) continue;
    std::string name = info->name ? info->name : "";
    // loosen matching if needed
    if (name.find("Rocksmith") != std::string::npos ||
        name.find("Real Tone") != std::string::npos) {
      pick.index = i; pick.name = name; return pick;
    }
  }
  // fallback default
  int def = Pa_GetDefaultInputDevice();
  const PaDeviceInfo* info = Pa_GetDeviceInfo(def);
  if (info) { pick.index = def; pick.name = info->name; }
  return pick;
}

// --------- SDL2 Render ---------
struct RenderState {
  SDL_Window* window = nullptr;
  SDL_Renderer* r = nullptr;
  int w = 1280, h = 720;
};

// --------- App State Machine ---------
enum class AppState { Title, Library, Tuner, FreePlay, Settings, Play };

struct App {
  RenderState rs;
  Chart chart;
  AppState state = AppState::Title;
  int menuIndex = 0; // index into title menu
  bool running = true;
  bool playing = true; // used in Play state
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

bool initSDL(RenderState& rs) {
  if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS|SDL_INIT_TIMER) != 0) {
    std::cerr << "SDL_Init: " << SDL_GetError() << "\n"; return false;
  }
  rs.window = SDL_CreateWindow("RockTrainer (Starter)",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               rs.w, rs.h, SDL_WINDOW_SHOWN);
  if (!rs.window) { std::cerr << "SDL_CreateWindow failed\n"; return false; }
  rs.r = SDL_CreateRenderer(rs.window, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if (!rs.r) { std::cerr << "SDL_CreateRenderer failed\n"; return false; }
  return true;
}

// Render the play state (chart + tuner overlay)
void renderPlayScreen(RenderState& rs, const Chart* chart, int64_t now_ms) {
  // Clear bg
  SDL_SetRenderDrawColor(rs.r, 12,12,16,255);
  SDL_RenderClear(rs.r);

  // Draw 6 string lanes horizontal bands (low E at bottom)
  int laneH = rs.h / 8;
  int topOffset = laneH; // top margin
  for (int s = 0; s < 6; ++s) {
    int y = rs.h - topOffset - s*laneH;
    SDL_SetRenderDrawColor(rs.r, 30,30,40,255);
    SDL_Rect lane{ 0, y - laneH/2, rs.w, laneH-2 };
    SDL_RenderFillRect(rs.r, &lane);
  }

  if (chart) {
    // time → x position (simple scroll): notes move left to right
    // Visible window ~ 4 seconds
    const double windowMs = 4000.0;
    for (const auto& n : chart->notes) {
      double dt = (double)(n.t_ms - now_ms);
      if (dt < -2000 || dt > windowMs) continue; // cull
      double x = (dt / windowMs) * rs.w * 0.9 + rs.w*0.5;
      int sIdx = std::clamp(6 - n.str, 0, 5); // chart str: 1=high E → map to top; draw bottom→top
      int y = rs.h - topOffset - sIdx*laneH;
      int h = laneH/2;
      int w = std::max(12, (int)( (n.len_ms/windowMs) * rs.w * 0.9 ));
      SDL_SetRenderDrawColor(rs.r, 80,180,250,255);
      SDL_Rect rect{ (int)x, y - h/2, w, h };
      SDL_RenderFillRect(rs.r, &rect);
    }
  }

  // Detected note overlay
  float hz = g_detectedHz.load(std::memory_order_relaxed);
  if (hz > 0.0f) {
    auto dn = analyzeFrequency(hz);
    if (dn) {
      auto [name, octave] = midiToName(dn->midi);
      char buf[128];
      snprintf(buf, sizeof(buf), "Hz: %.1f  %s%d  %+0.1f cents  %s",
               hz, name.c_str(), octave, dn->cents,
               (dn->fret >= 0 && dn->stringIdx >= 0) ? ("S" + std::to_string(6-dn->stringIdx) + " F" + std::to_string(dn->fret)).c_str()
                                                     : "—");
      // crude text: draw as rectangles for now (placeholder)
      // You can replace with SDL_ttf later. For now, draw a small bar proportional to pitch.
      int bar = std::clamp((int)((hz/1000.0)*rs.w), 0, rs.w);
      SDL_SetRenderDrawColor(rs.r, 200,200,220,255);
      SDL_Rect rect{20, 20, bar, 8};
      SDL_RenderFillRect(rs.r, &rect);
      // draw a simple vertical cent indicator
      int cx = rs.w/2 + (int)(dn->cents * 2); // scale cents
      SDL_SetRenderDrawColor(rs.r, 255,120,120,255);
      SDL_RenderDrawLine(rs.r, cx, 40, cx, 80);
      // center line
      SDL_SetRenderDrawColor(rs.r, 150,150,150,255);
      SDL_RenderDrawLine(rs.r, rs.w/2, 40, rs.w/2, 80);
    }
  }

  SDL_RenderPresent(rs.r);
}

// --------- Render helpers for other states ---------
static const std::vector<std::pair<const char*, AppState>> kMenu = {
  {"Library", AppState::Library},
  {"Tuner", AppState::Tuner},
  {"Free Play", AppState::FreePlay},
  {"Settings", AppState::Settings},
  {"Play", AppState::Play},
};

void renderTitle(App& app) {
  SDL_SetRenderDrawColor(app.rs.r, 0, 0, 40, 255);
  SDL_RenderClear(app.rs.r);
  int itemH = 60;
  int startY = app.rs.h/2 - (int)kMenu.size()*itemH/2;
  for (size_t i = 0; i < kMenu.size(); ++i) {
    int y = startY + (int)i*itemH;
    SDL_Rect rect{ app.rs.w/3, y, app.rs.w/3, itemH-10 };
    SDL_SetRenderDrawColor(app.rs.r, 80,80,80,255);
    SDL_RenderFillRect(app.rs.r, &rect);
    if ((int)i == app.menuIndex) {
      SDL_SetRenderDrawColor(app.rs.r, 0,255,200,100);
      SDL_Rect bar{ rect.x-10, y-5, rect.w+20, itemH };
      SDL_RenderFillRect(app.rs.r, &bar);
    }
  }
  SDL_RenderPresent(app.rs.r);
}

void updateTitle(App& app, const SDL_Event& e) {
  if (e.type != SDL_KEYDOWN) return;
  if (e.key.keysym.sym == SDLK_UP) {
    app.menuIndex = (app.menuIndex + (int)kMenu.size() - 1) % (int)kMenu.size();
  } else if (e.key.keysym.sym == SDLK_DOWN) {
    app.menuIndex = (app.menuIndex + 1) % (int)kMenu.size();
  } else if (e.key.keysym.sym == SDLK_RETURN) {
    app.state = kMenu[app.menuIndex].second;
    app.t0 = std::chrono::steady_clock::now();
  } else if (e.key.keysym.sym == SDLK_ESCAPE) {
    app.running = false;
  }
}

void renderStub(App& app) {
  SDL_SetRenderDrawColor(app.rs.r, 20,20,25,255);
  SDL_RenderClear(app.rs.r);
  SDL_RenderPresent(app.rs.r);
}

void updateReturnToTitle(App& app, const SDL_Event& e) {
  if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
    app.state = AppState::Title;
}

void renderLibrary(App& app){ renderStub(app); }
void updateLibrary(App& app, const SDL_Event& e){ updateReturnToTitle(app,e); }

void renderFreePlay(App& app){ renderStub(app); }
void updateFreePlay(App& app, const SDL_Event& e){ updateReturnToTitle(app,e); }

void renderSettings(App& app){ renderStub(app); }
void updateSettings(App& app, const SDL_Event& e){ updateReturnToTitle(app,e); }

void renderTuner(App& app) {
  SDL_SetRenderDrawColor(app.rs.r, 12,12,16,255);
  SDL_RenderClear(app.rs.r);
  float hz = g_detectedHz.load(std::memory_order_relaxed);
  if (hz > 0.0f) {
    auto dn = analyzeFrequency(hz);
    if (dn) {
      int bar = std::clamp((int)((hz/1000.0)*app.rs.w), 0, app.rs.w);
      SDL_SetRenderDrawColor(app.rs.r, 200,200,220,255);
      SDL_Rect rect{20,20,bar,8};
      SDL_RenderFillRect(app.rs.r, &rect);
      int cx = app.rs.w/2 + (int)(dn->cents * 2);
      SDL_SetRenderDrawColor(app.rs.r, 255,120,120,255);
      SDL_RenderDrawLine(app.rs.r, cx, 40, cx, 80);
      SDL_SetRenderDrawColor(app.rs.r, 150,150,150,255);
      SDL_RenderDrawLine(app.rs.r, app.rs.w/2, 40, app.rs.w/2, 80);
    }
  }
  SDL_RenderPresent(app.rs.r);
}

void updateTuner(App& app, const SDL_Event& e){ updateReturnToTitle(app,e); }

void renderPlay(App& app, int64_t now_ms){
  renderPlayScreen(app.rs, app.chart.notes.empty()?nullptr:&app.chart, now_ms);
}

void updatePlay(App& app, const SDL_Event& e){
  if (e.type != SDL_KEYDOWN) return;
  if (e.key.keysym.sym == SDLK_ESCAPE) app.state = AppState::Title;
  if (e.key.keysym.sym == SDLK_SPACE) app.playing = !app.playing;
  if (e.key.keysym.sym == SDLK_EQUALS || e.key.keysym.sym == SDLK_PLUS) g_latencyOffsetMs.fetch_add(5);
  if (e.key.keysym.sym == SDLK_MINUS) g_latencyOffsetMs.fetch_add(-5);
}

// --------- Main ---------
int main(int argc, char** argv) {
  std::string chartPath = (argc > 1) ? argv[1] : "charts/example.json";
  App app{};
  app.chart = loadChart(chartPath).value_or(Chart{});

  // Init PortAudio
  Pa_Initialize();
  auto pick = pickInputDevice();
  if (pick.index < 0) {
    std::cerr << "No input device found.\n";
    return 1;
  }
  std::cout << "Using input: " << pick.name << "\n";

  PaStreamParameters in{};
  in.device = pick.index;
  const PaDeviceInfo* info = Pa_GetDeviceInfo(in.device);
  in.channelCount = 1;
  in.sampleFormat = paFloat32;
  in.suggestedLatency = info ? info->defaultLowInputLatency : 0.0;
  in.hostApiSpecificStreamInfo = nullptr;

  AudioState st{};
  st.hop = kHopSize;
  st.inputFrame = new_fvec(st.hop);
  st.pitch = new_aubio_pitch("yinfast", kWinSize, st.hop, (uint_t)kSampleRate);
  aubio_pitch_set_unit(st.pitch, "Hz");
  aubio_pitch_set_silence(st.pitch, kSilenceDb);

  PaStream* stream = nullptr;
  PaError err = Pa_OpenStream(&stream, &in, nullptr, kSampleRate, st.hop, paNoFlag, audioCb, &st);
  if (err != paNoError) { std::cerr << "Pa_OpenStream: " << Pa_GetErrorText(err) << "\n"; return 1; }
  Pa_StartStream(stream);

  // Init SDL
  if (!initSDL(app.rs)) { std::cerr << "SDL init failed\n"; return 1; }

  // Main loop
  while (app.running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) app.running = false;
      else {
        switch (app.state) {
          case AppState::Title:   updateTitle(app, e); break;
          case AppState::Library: updateLibrary(app, e); break;
          case AppState::Tuner:   updateTuner(app, e); break;
          case AppState::FreePlay:updateFreePlay(app, e); break;
          case AppState::Settings:updateSettings(app, e); break;
          case AppState::Play:    updatePlay(app, e); break;
        }
      }
    }

    int64_t now_ms = 0;
    if (app.state == AppState::Play) {
      now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - app.t0).count();
      if (!app.playing) { app.t0 = std::chrono::steady_clock::now(); }
      now_ms += g_latencyOffsetMs.load();
    }

    switch (app.state) {
      case AppState::Title:   renderTitle(app); break;
      case AppState::Library: renderLibrary(app); break;
      case AppState::Tuner:   renderTuner(app); break;
      case AppState::FreePlay:renderFreePlay(app); break;
      case AppState::Settings:renderSettings(app); break;
      case AppState::Play:    renderPlay(app, now_ms); break;
    }

    SDL_Delay(16); // ~60fps
  }

  // Cleanup
  if (stream) { Pa_StopStream(stream); Pa_CloseStream(stream); }
  del_aubio_pitch(st.pitch);
  del_fvec(st.inputFrame);
  Pa_Terminate();

  if (app.rs.r) SDL_DestroyRenderer(app.rs.r);
  if (app.rs.window) SDL_DestroyWindow(app.rs.window);
  SDL_Quit();

  return 0;
}