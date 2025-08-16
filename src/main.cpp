#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <utility>
#include <optional>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <string_view>
#include <filesystem>

#ifdef RT_ENABLE_AUDIO
#include <portaudio.h>
#include <aubio/aubio.h>
#endif
#include <SDL.h>
#include "font8x8_basic.h"

// nlohmann json (header-only). Install via package manager.
// If CMake can't find it automatically, ensure its include dir is visible.
#include <nlohmann/json.hpp>
using json = nlohmann::json;
namespace fs = std::filesystem;

// --------- Config ---------
static constexpr double kSampleRate = 48000.0;
static constexpr unsigned kHopSize = 512;   // buffer size per callback
static constexpr unsigned kWinSize = 2048;  // analysis window
static constexpr float  kSilenceDb = -50.0f;
static constexpr int    kMaxFrets = 24;
static constexpr int    kFrameHistory = 120;

// --------- Globals (simple starter) ---------
struct NoteEvent {
  int64_t t_ms;   // start time
  int      str;   // 1..6 (1 = high E)
  int      fret;  // 0..24
  int64_t len_ms; // duration
  int      slideTo = -1;               // target fret for slide, -1 if none
  std::vector<std::string> techs;      // technique tags
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

static const char* kStringNames[6] = {"E2","A2","D3","G3","B3","E4"};

inline void drawChar(SDL_Renderer* r, char c, int x, int y, int scale, SDL_Color col) {
  unsigned uc = static_cast<unsigned char>(c);
  if (uc >= 128) return;
  const uint8_t* bmp = font8x8_basic[uc];
  SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
  for (int row = 0; row < 8; ++row) {
    for (int bit = 0; bit < 8; ++bit) {
      if ((bmp[row] >> bit) & 1) {
        SDL_Rect px{ x + bit*scale, y + row*scale, scale, scale };
        SDL_RenderFillRect(r, &px);
      }
    }
  }
}

inline void drawText(SDL_Renderer* r, std::string_view text, int x, int y, int scale, SDL_Color col) {
  int cx = x;
  for (char c : text) {
    if (c != ' ') drawChar(r, c, cx, y, scale, col);
    cx += 8 * scale;
  }
}


// --------- Chart loader ---------
std::optional<Chart> loadChart(const fs::path& path) {
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
      if (n.contains("slide")) e.slideTo = n["slide"].get<int>();
      if (n.contains("techs") && n["techs"].is_array()) {
        for (auto& t : n["techs"]) e.techs.push_back(t.get<std::string>());
      }
      c.notes.push_back(e);
    }
    std::sort(c.notes.begin(), c.notes.end(),
      [](const NoteEvent& a, const NoteEvent& b){return a.t_ms < b.t_ms;});
  }
  return c;
}

#ifdef RT_ENABLE_AUDIO
// --------- PortAudio + aubio ---------
struct AudioState {
  fvec_t* inputFrame = nullptr;
  aubio_pitch_t* pitch = nullptr;
  unsigned hop = kHopSize;
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
#endif

// --------- SDL2 Render ---------
struct RenderState {
  SDL_Window* window = nullptr;
  SDL_Renderer* r = nullptr;
  int w = 1280, h = 720;
  SDL_Texture* laneTex = nullptr;  // full-res offscreen
  SDL_Texture* bloomTex = nullptr; // downsampled bright areas
  SDL_Texture* blurTex = nullptr;  // blurred result
};

inline void drawTextCentered(RenderState& rs, std::string_view text, int y, int scale, SDL_Color col) {
  int w = (int)text.size() * 8 * scale;
  int x = rs.w / 2 - w / 2;
  drawText(rs.r, text, x, y, scale, col);
}

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
  std::array<float, kFrameHistory> frameTimes{};
  int frameTimeIdx = 0;
  bool frameTimesFull = false;
  bool showFrameGraph = false;
};

bool initSDL(RenderState& rs) {
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_EVENTS|SDL_INIT_TIMER) != 0) {
    std::cerr << "SDL_Init: " << SDL_GetError() << "\n"; return false;
  }
  rs.window = SDL_CreateWindow("RockTrainer (Starter)",
                               SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               rs.w, rs.h, SDL_WINDOW_SHOWN);
  if (!rs.window) { std::cerr << "SDL_CreateWindow failed\n"; return false; }
  rs.r = SDL_CreateRenderer(rs.window, -1, SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
  if (!rs.r) { std::cerr << "SDL_CreateRenderer failed\n"; return false; }
  // Create render targets
  rs.laneTex = SDL_CreateTexture(rs.r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, rs.w, rs.h);
  int bw = rs.w/2, bh = rs.h/2;
  rs.bloomTex = SDL_CreateTexture(rs.r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, bw, bh);
  rs.blurTex  = SDL_CreateTexture(rs.r, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, bw, bh);
  if (!rs.laneTex || !rs.bloomTex || !rs.blurTex) {
    std::cerr << "Texture creation failed\n"; return false;
  }
  return true;
}

void renderFrameGraph(App& app) {
  SDL_Renderer* r = app.rs.r;
  const int w = kFrameHistory;
  const int h = 60;
  const int x0 = 10;
  const int y0 = 10;

  SDL_SetRenderDrawColor(r, 0, 0, 0, 160);
  SDL_Rect bg{ x0-1, y0-1, w+2, h+2 };
  SDL_RenderFillRect(r, &bg);

  SDL_SetRenderDrawColor(r, 100, 100, 100, 255);
  SDL_RenderDrawLine(r, x0, y0 + h/2, x0 + w, y0 + h/2);

  SDL_SetRenderDrawColor(r, 0, 255, 0, 255);
  float scale = h / (16.7f * 2.f);
  int count = app.frameTimesFull ? kFrameHistory : app.frameTimeIdx;
  int start = app.frameTimesFull ? app.frameTimeIdx : 0;
  for (int i = 1; i < count; ++i) {
    int idx0 = (start + i - 1) % kFrameHistory;
    int idx1 = (start + i) % kFrameHistory;
    float t0 = std::min(app.frameTimes[idx0], 33.4f);
    float t1 = std::min(app.frameTimes[idx1], 33.4f);
    int x1 = x0 + i - 1;
    int x2 = x0 + i;
    int y1 = y0 + h - int(t0 * scale);
    int y2 = y0 + h - int(t1 * scale);
    SDL_RenderDrawLine(r, x1, y1, x2, y2);
  }
}

// Render the play state (chart + tuner overlay)
void drawChart(App& app, const Chart* chart, int64_t now_ms) {
  RenderState& rs = app.rs;
  // First pass: render chart to offscreen texture
  SDL_SetRenderTarget(rs.r, rs.laneTex);
  SDL_SetRenderDrawColor(rs.r, 12,12,16,255);
  SDL_RenderClear(rs.r);

  // Colored lanes (low E bottom → purple, high E top → red)
  int laneH = rs.h / 8;
  int topOffset = laneH; // top margin
  static const SDL_Color kStrColors[6] = {
    {128,0,255,255}, {0,0,255,255}, {0,255,0,255},
    {255,255,0,255}, {255,128,0,255}, {255,0,0,255}
  };
  for (int s = 0; s < 6; ++s) {
    int y = rs.h - topOffset - s*laneH;
    SDL_Color c = kStrColors[s];
    SDL_SetRenderDrawColor(rs.r, c.r/4, c.g/4, c.b/4, 255);
    SDL_Rect lane{ 0, y - laneH/2, rs.w, laneH-2 };
    SDL_RenderFillRect(rs.r, &lane);
  }

  // Hit line at center
  SDL_SetRenderDrawColor(rs.r, 255,255,255,120);
  SDL_RenderDrawLine(rs.r, rs.w/2, topOffset/2, rs.w/2, rs.h-topOffset/2);

  if (chart) {
    const double windowMs = 4000.0;
    double beatMs = 60000.0 / chart->bpm;
    double measureMs = beatMs * 4.0;
    int64_t startBeat = (int64_t)std::floor((now_ms - windowMs) / beatMs) * (int64_t)beatMs;
    int64_t endTime = now_ms + (int64_t)windowMs;
    for (double t = startBeat; t <= endTime; t += beatMs) {
      double dtb = t - now_ms;
      double x = (dtb / windowMs) * rs.w * 0.9 + rs.w*0.5;
      if (x < 0 || x > rs.w) continue;
      bool isMeasure = std::fmod(t, measureMs) < 1.0;
      SDL_SetRenderDrawColor(rs.r, 255,255,255, isMeasure ? 100 : 40);
      SDL_RenderDrawLine(rs.r, (int)x, topOffset/2, (int)x, rs.h-topOffset/2);
    }

    for (const auto& n : chart->notes) {
      double dt = (double)(n.t_ms - now_ms);
      if (dt < -2000 || dt > windowMs) continue;
      double x = (dt / windowMs) * rs.w * 0.9 + rs.w*0.5;
      int sIdx = std::clamp(6 - n.str, 0, 5);
      int y = rs.h - topOffset - sIdx*laneH;
      double depth = std::clamp(1.0 - std::abs(dt)/windowMs, 0.0, 1.0);
      double scale = 0.5 + 0.5*depth;
      Uint8 alpha = (Uint8)(255 * depth);
      int h = (int)(laneH/2 * scale);
      int w = std::max(12, (int)((n.len_ms/windowMs) * rs.w * 0.9));
      int headW = std::max(12, (int)(12 * scale));
      int sustainW = w - headW;

      SDL_Color c = kStrColors[sIdx];
      SDL_SetRenderDrawColor(rs.r, c.r, c.g, c.b, alpha);
      SDL_Rect head{ (int)x - headW/2, y - h/2, headW, h };
      SDL_RenderFillRect(rs.r, &head);
      if (sustainW > 0) {
        SDL_Rect sus{ head.x + headW, y - h/4, sustainW, h/2 };
        SDL_RenderFillRect(rs.r, &sus);
        if (n.slideTo >= 0 && n.slideTo != n.fret) {
          double dy = (n.slideTo - n.fret) * (laneH/24.0);
          SDL_RenderDrawLine(rs.r, head.x + headW, y, head.x + headW + sustainW, (int)(y + dy));
        }
      }
      if (!n.techs.empty()) {
        SDL_SetRenderDrawColor(rs.r, 255,255,255, alpha);
        SDL_Rect tag{ head.x - 6, head.y - 10, 12, 8 };
        SDL_RenderFillRect(rs.r, &tag);
      }
    }
  }

  // Bloom: extract bright areas to downsampled texture
  auto extractBright = [&](Uint8 threshold){
    void* srcPixels; int srcPitch;
    void* dstPixels; int dstPitch;
    SDL_LockTexture(rs.laneTex, nullptr, &srcPixels, &srcPitch);
    SDL_LockTexture(rs.bloomTex, nullptr, &dstPixels, &dstPitch);
    SDL_PixelFormat* fmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    int sw = rs.w, sh = rs.h;
    int dw = sw/2, dh = sh/2;
    Uint32* src = static_cast<Uint32*>(srcPixels);
    Uint32* dst = static_cast<Uint32*>(dstPixels);
    int sStride = srcPitch/4;
    int dStride = dstPitch/4;
    for (int y=0;y<dh;++y){
      for (int x=0;x<dw;++x){
        int r=0,g=0,b=0;
        for(int oy=0;oy<2;++oy) for(int ox=0;ox<2;++ox){
          Uint8 pr,pg,pb,pa;
          SDL_GetRGBA(src[(y*2+oy)*sStride + (x*2+ox)], fmt, &pr,&pg,&pb,&pa);
          r+=pr; g+=pg; b+=pb;
        }
        r/=4; g/=4; b/=4;
        Uint8 bright = (Uint8)((r+g+b)/3);
        if (bright < threshold) r=g=b=0;
        dst[y*dStride+x] = SDL_MapRGBA(fmt,(Uint8)r,(Uint8)g,(Uint8)b,255);
      }
    }
    SDL_UnlockTexture(rs.laneTex);
    SDL_UnlockTexture(rs.bloomTex);
    SDL_FreeFormat(fmt);
  };

  auto blur = [&](){
    int w = rs.w/2, h = rs.h/2;
    const int k[5] = {1,4,6,4,1};
    SDL_PixelFormat* fmt = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    // horizontal
    void* srcPix; int srcPitch; void* dstPix; int dstPitch;
    SDL_LockTexture(rs.bloomTex, nullptr, &srcPix, &srcPitch);
    SDL_LockTexture(rs.blurTex,  nullptr, &dstPix, &dstPitch);
    Uint32* src = static_cast<Uint32*>(srcPix); int sStride = srcPitch/4;
    Uint32* dst = static_cast<Uint32*>(dstPix); int dStride = dstPitch/4;
    for(int y=0;y<h;++y){
      for(int x=0;x<w;++x){
        int sr=0,sg=0,sb=0;
        for(int i=-2;i<=2;++i){
          int sx = std::clamp(x+i,0,w-1);
          Uint8 r,g,b,a; SDL_GetRGBA(src[y*sStride+sx], fmt,&r,&g,&b,&a);
          int wgt = k[i+2]; sr+=r*wgt; sg+=g*wgt; sb+=b*wgt;
        }
        dst[y*dStride+x] = SDL_MapRGBA(fmt, (Uint8)(sr/16), (Uint8)(sg/16), (Uint8)(sb/16), 255);
      }
    }
    SDL_UnlockTexture(rs.bloomTex);
    SDL_UnlockTexture(rs.blurTex);
    // vertical back into bloomTex
    SDL_LockTexture(rs.blurTex,  nullptr, &srcPix, &srcPitch);
    SDL_LockTexture(rs.bloomTex, nullptr, &dstPix, &dstPitch);
    src = static_cast<Uint32*>(srcPix); sStride = srcPitch/4;
    dst = static_cast<Uint32*>(dstPix); dStride = dstPitch/4;
    for(int y=0;y<h;++y){
      for(int x=0;x<w;++x){
        int sr=0,sg=0,sb=0;
        for(int i=-2;i<=2;++i){
          int sy = std::clamp(y+i,0,h-1);
          Uint8 r,g,b,a; SDL_GetRGBA(src[sy*sStride+x], fmt,&r,&g,&b,&a);
          int wgt = k[i+2]; sr+=r*wgt; sg+=g*wgt; sb+=b*wgt;
        }
        dst[y*dStride+x] = SDL_MapRGBA(fmt, (Uint8)(sr/16), (Uint8)(sg/16), (Uint8)(sb/16), 255);
      }
    }
    SDL_UnlockTexture(rs.blurTex);
    SDL_UnlockTexture(rs.bloomTex);
    SDL_FreeFormat(fmt);
  };

  extractBright(200);
  blur();

  SDL_SetRenderTarget(rs.r, nullptr);
  SDL_SetRenderDrawColor(rs.r,0,0,0,255);
  SDL_RenderClear(rs.r);
  SDL_RenderCopy(rs.r, rs.laneTex, nullptr, nullptr);
  SDL_SetTextureBlendMode(rs.bloomTex, SDL_BLENDMODE_ADD);
  SDL_RenderCopy(rs.r, rs.bloomTex, nullptr, nullptr);

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

  if (app.showFrameGraph) renderFrameGraph(app);
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
    int scale = 4;
    int textY = y + (itemH - 8*scale) / 2;
    drawTextCentered(app.rs, kMenu[i].first, textY, scale, SDL_Color{20,20,20,255});
  }
  if (app.showFrameGraph) renderFrameGraph(app);
  SDL_RenderPresent(app.rs.r);
}

void updateTitle(App& app, const SDL_Event& e) {
  int itemH = 60;
  int startY = app.rs.h/2 - (int)kMenu.size()*itemH/2;
  if (e.type == SDL_KEYDOWN) {
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
  } else if (e.type == SDL_MOUSEMOTION) {
    int mx = e.motion.x;
    int my = e.motion.y;
    if (mx >= app.rs.w/3 && mx < 2*app.rs.w/3 &&
        my >= startY && my < startY + (int)kMenu.size()*itemH) {
      app.menuIndex = (my - startY) / itemH;
    }
  } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
    int mx = e.button.x;
    int my = e.button.y;
    if (mx >= app.rs.w/3 && mx < 2*app.rs.w/3 &&
        my >= startY && my < startY + (int)kMenu.size()*itemH) {
      app.menuIndex = (my - startY) / itemH;
      app.state = kMenu[app.menuIndex].second;
      app.t0 = std::chrono::steady_clock::now();
    }
  }
}

void renderStub(App& app) {
  SDL_SetRenderDrawColor(app.rs.r, 20,20,25,255);
  SDL_RenderClear(app.rs.r);
  if (app.showFrameGraph) renderFrameGraph(app);
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
    auto dnOpt = analyzeFrequency(hz);
    if (dnOpt) {
      auto& dn = *dnOpt;
      int cx = app.rs.w/2;
      int cy = app.rs.h/2;
      int meterW = app.rs.w * 3 / 4;
      int left = cx - meterW/2;
      int right = cx + meterW/2;

      // baseline
      SDL_SetRenderDrawColor(app.rs.r, 60,60,70,255);
      SDL_RenderDrawLine(app.rs.r, left, cy, right, cy);
      // center tick
      SDL_RenderDrawLine(app.rs.r, cx, cy-40, cx, cy+40);

      // pointer for cents (-50..+50)
      double cents = std::clamp(dn.cents, -50.0, 50.0);
      int px = cx + int(cents/50.0 * (meterW/2));
      SDL_SetRenderDrawColor(app.rs.r, 255,120,120,255);
      SDL_RenderDrawLine(app.rs.r, px, cy-60, px, cy+60);

      // string name
      const char* sname = (dn.stringIdx >=0 && dn.stringIdx < 6) ? kStringNames[dn.stringIdx] : "--";
      drawTextCentered(app.rs, sname, cy-120, 8, SDL_Color{200,200,220,255});

      // Hz value
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.1f Hz", hz);
      drawTextCentered(app.rs, buf, cy+80, 4, SDL_Color{200,200,220,255});
    }
  }
  if (app.showFrameGraph) renderFrameGraph(app);
  SDL_RenderPresent(app.rs.r);
}

void updateTuner(App& app, const SDL_Event& e){ updateReturnToTitle(app,e); }

void renderPlay(App& app, int64_t now_ms){
  drawChart(app, app.chart.notes.empty()?nullptr:&app.chart, now_ms);
}

void updatePlay(App& app, const SDL_Event& e){
  if (e.type != SDL_KEYDOWN) return;
  if (e.key.keysym.sym == SDLK_ESCAPE) app.state = AppState::Title;
  if (e.key.keysym.sym == SDLK_SPACE) app.playing = !app.playing;
  if (e.key.keysym.sym == SDLK_EQUALS || e.key.keysym.sym == SDLK_PLUS) g_latencyOffsetMs.fetch_add(5);
  if (e.key.keysym.sym == SDLK_MINUS) g_latencyOffsetMs.fetch_add(-5);
}

// --------- Main ---------
#ifndef ROCKTRAINER_NO_MAIN
int main(int argc, char** argv) {
  fs::path exeDir;
  try {
    exeDir = fs::canonical(fs::path(argv[0])).parent_path();
  } catch (const fs::filesystem_error&) {
    exeDir = fs::current_path();
  }
  fs::path dataRoot = exeDir;
  if (!fs::exists(dataRoot / "charts")) {
    dataRoot = exeDir.parent_path();
  }

  fs::path chartPath = (argc > 1) ? fs::path(argv[1]) : fs::path("charts") / "example.json";
  if (!chartPath.is_absolute()) {
    chartPath = dataRoot / chartPath;
  }
  if (!fs::exists(chartPath)) {
    std::cerr << "Chart file not found: " << chartPath << "\n";
    return 1;
  }

  fs::path assetsDir = dataRoot / "assets";
  if (!fs::exists(assetsDir)) {
    std::cerr << "Assets directory not found: " << assetsDir << "\n";
    return 1;
  }

  App app{};
  app.chart = loadChart(chartPath).value_or(Chart{});
#ifdef RT_ENABLE_AUDIO
  AudioState st{};
  PaStream* stream = nullptr;

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

  st.hop = kHopSize;
  st.inputFrame = new_fvec(st.hop);
  st.pitch = new_aubio_pitch("yinfast", kWinSize, st.hop, (unsigned)kSampleRate);
  aubio_pitch_set_unit(st.pitch, "Hz");
  aubio_pitch_set_silence(st.pitch, kSilenceDb);

  PaError err = Pa_OpenStream(&stream, &in, nullptr, kSampleRate, st.hop, paNoFlag, audioCb, &st);
  if (err != paNoError) { std::cerr << "Pa_OpenStream: " << Pa_GetErrorText(err) << "\n"; return 1; }
  Pa_StartStream(stream);
#endif

  // Init SDL
  if (!initSDL(app.rs)) { std::cerr << "SDL init failed\n"; return 1; }

  const double freq = (double)SDL_GetPerformanceFrequency();
  Uint64 lastCounter = SDL_GetPerformanceCounter();

  // Main loop
  while (app.running) {
    Uint64 nowCounter = SDL_GetPerformanceCounter();
    float dt_ms = float((nowCounter - lastCounter) * 1000.0 / freq);
    lastCounter = nowCounter;
    app.frameTimes[app.frameTimeIdx] = dt_ms;
    app.frameTimeIdx = (app.frameTimeIdx + 1) % kFrameHistory;
    if (app.frameTimeIdx == 0) app.frameTimesFull = true;

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) app.running = false;
      else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_F3) {
        app.showFrameGraph = !app.showFrameGraph;
      } else {
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
#ifdef RT_ENABLE_AUDIO
  if (stream) { Pa_StopStream(stream); Pa_CloseStream(stream); }
  del_aubio_pitch(st.pitch);
  del_fvec(st.inputFrame);
  Pa_Terminate();
#endif

  if (app.rs.blurTex) SDL_DestroyTexture(app.rs.blurTex);
  if (app.rs.bloomTex) SDL_DestroyTexture(app.rs.bloomTex);
  if (app.rs.laneTex) SDL_DestroyTexture(app.rs.laneTex);
  if (app.rs.r) SDL_DestroyRenderer(app.rs.r);
  if (app.rs.window) SDL_DestroyWindow(app.rs.window);
  SDL_Quit();

  return 0;
}
#endif // ROCKTRAINER_NO_MAIN
