// Console-wide settings and app registry.
#pragma once

#include <cstdint>

#include "engine/engine.h"

namespace console {

// Draws an app's icon into a (2r x 2r) RGB565 buffer; pixels outside the icon
// circle must be left at 0 (black).
using IconFn = void (*)(wc::Color *buf, int r);

struct App {
    const char *id;     // theme icon file name: Theme/<theme>/icons/<id>.png
    const char *name;
    wc::Color accent;
    IconFn icon;
    wc::Game *game;
    // Games that arrived as a package do not get to wear a theme's icons. Those belong to
    // the apps the theme was drawn for, and a game from a stranger picking one up means
    // the home screen can show it as something it is not.
    bool packaged = false;
    // A package's own icon, as the PNG it shipped. Owned by whoever built the table.
    const uint8_t *icon_png = nullptr;
    size_t icon_len = 0;
};

// ---- brightness (persisted)
constexpr int BRIGHTNESS_LEVELS = 4;
constexpr const char *BRIGHTNESS_NAMES[BRIGHTNESS_LEVELS] = {"LOW", "MED", "HIGH", "MAX"};
int brightnessLevel();
void setBrightnessLevel(int level);   // applies and saves
void applyBrightness();

// ---- apps taken off the home screen (Settings > GAMES; persisted). Settings can't be.
bool appHidden(const char *id);
void setAppHidden(const char *id, bool hidden);

// ---- motion sensor calibration (persisted; measured by Settings > CALIBRATE)
void loadTiltCalibration();   // at boot: hands the saved correction to the input layer
void saveTiltCalibration(const wc::TiltCal &cal);
void clearTiltCalibration();
bool tiltCalibrated();

// ---- auto off: minutes asleep before the console powers down (persisted)
constexpr int AUTO_OFF_OPTIONS = 5;
constexpr const char *AUTO_OFF_NAMES[AUTO_OFF_OPTIONS] = {"1 MIN", "2 MIN", "5 MIN", "10 MIN", "NEVER"};
int autoOffIndex();
int autoOffSeconds();                 // 0 = never
void setAutoOffIndex(int index);      // saves; caller applies to the engine

const char *firmwareVersion();

// ---- crash breadcrumbs
// A crash takes the serial log with it, so the console remembers what it was
// doing (this survives a crash), shows it on screen after a restart, and appends it
// to last_crash.txt on the drive.
void crumb(const char *what);
bool crashedLastBoot();
const char *lastCrashText();          // "reason - what it was doing"
void reportCrashIfAny(wc::Engine &e); // show it for a few seconds, save it to the drive
const char *previousCrumb();          // what the last boot was doing when it died ("" if clean)

// ---- safe mode
// Counts boots that died before running for 20 s. Three in a row = safe mode: skip
// everything optional and just bring up Wi-Fi so a fixed build can be sent.
int noteBootStarted();                // call first: returns consecutive failed boots before this one
void noteBootStable();
bool safeMode();
void setSafeMode(bool on);

}  // namespace console
