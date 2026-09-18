// Sound for the console: a tiny chiptune-style synth through the onboard ES8311
// codec and speaker. Games ask for short tones; a mixer task on core 0 renders
// them, so nothing blocks the game loop.
#pragma once

#include <cstdint>

namespace wc::audio {

enum class Wave : uint8_t {
    Square,     // punchy, classic blip
    Triangle,   // softer, rounder
    Noise,      // hits, explosions
};

struct Tone {
    float f0 = 440;         // start frequency (Hz)
    float f1 = 0;           // end frequency; 0 = hold f0
    uint16_t ms = 80;       // length
    Wave wave = Wave::Square;
    float volume = 1.0f;    // 0..1 relative to the console volume
    uint16_t delay_ms = 0;  // wait before it starts (for little melodies)
};

bool init();                       // safe to call twice; false if there's no codec
void play(const Tone &t);
void play(const Tone *tones, int count);   // a short sequence
void stopAll();

// Console volume: 0 = off, 1 = low, 2 = medium, 3 = high (persisted).
constexpr int LEVELS = 4;
constexpr const char *LEVEL_NAMES[LEVELS] = {"OFF", "LOW", "MED", "HIGH"};
int volume();
void setVolume(int level);

}  // namespace wc::audio
