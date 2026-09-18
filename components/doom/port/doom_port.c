// The Tilt-a-tron platform layer for the Doom engine: everything the engine expects
// from i_system / i_video / i_input / a sound module, plus the task it runs in.
#include "doom/doom_port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "config.h"
#include "d_event.h"
#include "d_items.h"
#include "d_main.h"
#include "d_player.h"
#include "doomdef.h"
#include "doomkeys.h"
#include "doomstat.h"
#include "doomtype.h"
#include "g_game.h"
#include "i_sound.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "info.h"
#include "m_argv.h"
#include "m_controls.h"
#include "v_patch.h"
#include "v_video.h"
#include "w_file.h"
#include "w_wad.h"
#include "z_zone.h"

static const char *TAG = "doom";

void D_DoomMain(void);
void doomgeneric_Tick(void);
void M_FindResponseFile(void);
const uint8_t *doom_wad_map(uint32_t *length, const char **name);
void wc_audio_pcm_start(int channel, const uint8_t *data, int len, int rate, float volume);
void wc_audio_pcm_volume(int channel, float volume);
void wc_audio_pcm_stop(int channel);
int wc_audio_pcm_playing(int channel);

// ------------------------------------------------------------------ shared with the console

static volatile bool s_started, s_active, s_main_done;
static volatile int s_new_game = -1;
static volatile int s_weapon_clicks;
static volatile bool s_fire, s_use;
static volatile uint32_t s_frames;
static char s_error[160];
static volatile bool s_failed;
static int s_skill = 2;
static float s_sfx_gain = 1.0f;

// Read by G_BuildTiccmd (g_game.c) once per tic.
volatile int tat_forward, tat_turn;

// The engine draws into I_VideoBuffer; finished frames are copied to one of two
// buffers and the console reads whichever was finished last.
static uint8_t *s_front[2];
static volatile int s_front_i;
static uint8_t s_pal[768];
static volatile uint32_t s_pal_gen;

static int64_t s_paused_us;

// ------------------------------------------------------------------ i_system

typedef struct atexit_listentry_s atexit_listentry_t;

static void park(void)
{
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void I_Error(char *error, ...)
{
    va_list ap;
    va_start(ap, error);
    vsnprintf(s_error, sizeof(s_error), error, ap);
    va_end(ap);
    ESP_LOGE(TAG, "I_Error: %s", s_error);
    s_failed = true;
    park();   // the engine can't carry on; the console shows the message
    for (;;) {}
}

void I_Quit(void)
{
    snprintf(s_error, sizeof(s_error), "DOOM QUIT");
    s_failed = true;
    park();
}

void I_AtExit(atexit_func_t func, boolean run_on_error) {}
void I_Tactile(int on, int off, int total) {}
void I_Init(void) {}
void I_BindVariables(void) {}
boolean I_ConsoleStdout(void) { return false; }
boolean I_GetMemoryValue(unsigned int offset, void *value, int size) { return false; }
void I_PrintBanner(char *msg) { ESP_LOGI(TAG, "%s", msg); }
void I_PrintDivider(void) {}
void I_PrintStartupBanner(char *gamedescription) { ESP_LOGI(TAG, "%s", gamedescription); }

byte *I_ZoneBase(int *size)
{
    // Lumps are used straight from flash, so the zone only holds level data and caches.
    static const int kSizes[] = {3 * 1024 * 1024, 2560 * 1024, 2 * 1024 * 1024, 1536 * 1024};
    for (unsigned i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
        byte *zone = heap_caps_malloc(kSizes[i], MALLOC_CAP_SPIRAM);
        if (zone) {
            *size = kSizes[i];
            ESP_LOGI(TAG, "zone: %d KB (PSRAM free now %u KB)", kSizes[i] / 1024,
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            return zone;
        }
    }
    I_Error("NOT ENOUGH MEMORY FOR DOOM");
    return NULL;
}

// ------------------------------------------------------------------ timer (i_timer.c calls these)

uint32_t DG_GetTicksMs(void) { return (uint32_t)((esp_timer_get_time() - s_paused_us) / 1000); }

void DG_SleepMs(uint32_t ms)
{
    const TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : 1);
}

// ------------------------------------------------------------------ i_video

byte *I_VideoBuffer = NULL;
boolean screensaver_mode = false;
boolean screenvisible = true;
float mouse_acceleration = 2.0;
int mouse_threshold = 10;
int usegamma = 0;
int usemouse = 0;
int vanilla_keyboard_mapping = 1;

void I_InitGraphics(void)
{
    I_VideoBuffer = heap_caps_calloc(1, DOOM_W * DOOM_H, MALLOC_CAP_SPIRAM);
    if (!I_VideoBuffer) I_Error("NO MEMORY FOR THE SCREEN");
}

void I_ShutdownGraphics(void) {}
void I_UpdateNoBlit(void) {}
void I_ReadScreen(byte *scr) { memcpy(scr, I_VideoBuffer, DOOM_W * DOOM_H); }
void I_BeginRead(void) {}
void I_EndRead(void) {}
void I_SetWindowTitle(char *title) {}
void I_GraphicsCheckCommandLine(void) {}
void I_SetGrabMouseCallback(grabmouse_callback_t func) {}
void I_EnableLoadingDisk(void) {}
void I_BindVideoVariables(void) {}
void I_DisplayFPSDots(boolean dots_on) {}
void I_CheckIsScreensaver(void) {}

void I_SetPalette(byte *palette)
{
    for (int i = 0; i < 768; i++) s_pal[i] = gammatable[usegamma][palette[i]];
    s_pal_gen++;
}

int I_GetPaletteIndex(int r, int g, int b)
{
    int best = 0, best_d = 1 << 30;
    for (int i = 0; i < 256; i++) {
        const int dr = r - s_pal[i * 3], dg = g - s_pal[i * 3 + 1], db = b - s_pal[i * 3 + 2];
        const int d = dr * dr + dg * dg + db * db;
        if (d < best_d) best_d = d, best = i;
    }
    return best;
}

// ---- our own status display, drawn on the finished frame (the engine runs with its
// status bar off: the round screen only shows the middle of the 320x200 picture)

static void draw_patch(uint8_t *fb, int x, int y, const patch_t *p)
{
    const int w = SHORT(p->width);
    for (int col = 0; col < w; col++) {
        const int sx = x + col;
        if (sx < 0 || sx >= DOOM_W) continue;
        const byte *post = (const byte *)p + LONG(p->columnofs[col]);
        while (*post != 0xff) {
            const int top = post[0], len = post[1];
            const byte *src = post + 3;
            for (int i = 0; i < len; i++) {
                const int sy = y + top + i;
                if (sy >= 0 && sy < DOOM_H) fb[sy * DOOM_W + sx] = src[i];
            }
            post += len + 4;
        }
    }
}

static const patch_t *s_num[10], *s_percent, *s_keys[6];

static void load_hud(void)
{
    char name[9];
    for (int i = 0; i < 10; i++) {
        snprintf(name, sizeof(name), "STTNUM%d", i);
        s_num[i] = W_CacheLumpName(name, PU_STATIC);
    }
    s_percent = W_CacheLumpName("STTPRCNT", PU_STATIC);
    for (int i = 0; i < 6; i++) {
        snprintf(name, sizeof(name), "STKEYS%d", i);
        s_keys[i] = W_CacheLumpName(name, PU_STATIC);
    }
}

// Right-aligned number ending at x.
static void draw_number(uint8_t *fb, int x, int y, int n)
{
    if (n < 0) n = 0;
    do {
        x -= 14;
        draw_patch(fb, x, y, s_num[n % 10]);
        n /= 10;
    } while (n > 0);
}

static void draw_hud(uint8_t *fb)
{
    if (!s_num[0]) load_hud();
    const player_t *pl = &players[consoleplayer];
    // The visible part of the picture is an ellipse about 240 x 200; near the bottom
    // it is ~120 px wide, centred on x = 160.
    const int y = 170;
    draw_number(fb, 148, y, pl->health);
    draw_patch(fb, 148, y, s_percent);
    const ammotype_t at = weaponinfo[pl->readyweapon].ammo;
    if (at != am_noammo) draw_number(fb, 214, y, pl->ammo[at]);
    int kx = 164;
    for (int i = 0; i < 6; i++) {
        if (!pl->cards[i]) continue;
        draw_patch(fb, kx, y - 8, s_keys[i]);
        kx += 9;
    }
}

void I_StartFrame(void)
{
    // Off screen (another app, or the console menu): stop here, and stop the clock.
    if (!s_active) {
        const int64_t t0 = esp_timer_get_time();
        for (int ch = 0; ch < 8; ch++) wc_audio_pcm_stop(ch);
        while (!s_active) vTaskDelay(pdMS_TO_TICKS(20));
        s_paused_us += esp_timer_get_time() - t0;
    }
}

void I_FinishUpdate(void)
{
    const int next = 1 - s_front_i;
    memcpy(s_front[next], I_VideoBuffer, DOOM_W * DOOM_H);
    if (gamestate == GS_LEVEL && !automapactive && s_main_done) draw_hud(s_front[next]);
    s_front_i = next;
    s_frames++;
    vTaskDelay(1);   // let the idle task in even when Doom can't keep up with 35 Hz
}

// ------------------------------------------------------------------ i_input

static void key(int k, boolean down)
{
    event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = k;
    D_PostEvent(&ev);
}

void I_InitInput(void) {}

void I_GetEvent(void)
{
    static bool fire_sent, use_sent;
    static int weapon_seen, weapon_release;
    if (s_fire != fire_sent) key(key_fire, fire_sent = s_fire);
    if (s_use != use_sent) key(key_use, use_sent = s_use);
    if (weapon_release) {
        key(key_nextweapon, false);
        weapon_release = 0;
    } else if (weapon_seen != s_weapon_clicks) {
        weapon_seen++;
        key(key_nextweapon, true);
        weapon_release = 1;
    }
    if (s_new_game >= 0) {
        G_DeferedInitNew((skill_t)s_new_game, 1, 1);
        s_new_game = -1;
    }
}

void I_StartTic(void) { I_GetEvent(); }

// ------------------------------------------------------------------ WAD access: memory-mapped flash

typedef struct {
    wad_file_t wad;
} tat_wad_file_t;

extern wad_file_class_t tat_wad_file;

static wad_file_t *tat_open(char *path)
{
    if (strncmp(path, "/wad/", 5) != 0) return NULL;
    uint32_t length = 0;
    const uint8_t *map = doom_wad_map(&length, NULL);
    if (!map) return NULL;
    tat_wad_file_t *f = Z_Malloc(sizeof(*f), PU_STATIC, 0);
    f->wad.file_class = &tat_wad_file;
    f->wad.mapped = (byte *)map;
    f->wad.length = length;
    return &f->wad;
}

static void tat_close(wad_file_t *wad) { Z_Free(wad); }

static size_t tat_read(wad_file_t *wad, unsigned int offset, void *buffer, size_t len)
{
    if (offset >= wad->length) return 0;
    if (offset + len > wad->length) len = wad->length - offset;
    memcpy(buffer, wad->mapped + offset, len);
    return len;
}

wad_file_class_t tat_wad_file = {tat_open, tat_close, tat_read};

// ------------------------------------------------------------------ sound: PCM voices in the console mixer

// Config variables the engine binds for the SDL sound code it was written against.
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

static snddevice_t s_sfx_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS, SNDDEVICE_WAVEBLASTER,
                                      SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};
static boolean s_sfx_prefix;

static boolean snd_init(boolean use_sfx_prefix)
{
    s_sfx_prefix = use_sfx_prefix;
    return true;
}
static void snd_shutdown(void) {}

static int snd_lump(sfxinfo_t *sfx)
{
    char name[16];
    if (sfx->link) sfx = sfx->link;
    snprintf(name, sizeof(name), "%s%s", s_sfx_prefix ? "ds" : "", sfx->name);
    return W_CheckNumForName(name);
}

static void snd_update(void) {}
static void snd_params(int channel, int vol, int sep) { wc_audio_pcm_volume(channel, vol / 127.0f * s_sfx_gain); }

static int snd_start(sfxinfo_t *sfx, int channel, int vol, int sep)
{
    if (channel < 0 || channel >= 8 || sfx->lumpnum < 0) return -1;
    const int size = W_LumpLength(sfx->lumpnum);
    const byte *d = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);   // mapped flash: stays put
    // DMX: u16 format (3), u16 rate, u32 count, then 8-bit samples with 16 pad bytes each end.
    if (size < 8 + 32 || d[0] != 3 || d[1] != 0) return -1;
    const int rate = d[2] | (d[3] << 8);
    int count = (int)(d[4] | (d[5] << 8) | (d[6] << 16) | ((uint32_t)d[7] << 24));
    if (count > size - 8) count = size - 8;
    if (count <= 32) return -1;
    wc_audio_pcm_start(channel, d + 8 + 16, count - 32, rate, vol / 127.0f * s_sfx_gain);
    return channel;
}

static void snd_stop(int channel) { wc_audio_pcm_stop(channel); }
static boolean snd_playing(int channel) { return wc_audio_pcm_playing(channel) != 0; }
static void snd_cache(sfxinfo_t *sounds, int num_sounds) {}

sound_module_t DG_sound_module = {
    s_sfx_devices, sizeof(s_sfx_devices) / sizeof(s_sfx_devices[0]),
    snd_init, snd_shutdown, snd_lump, snd_update, snd_params, snd_start, snd_stop, snd_playing, snd_cache,
};

// No music (yet): the speaker is tiny and MUS playback needs a synth.
static boolean mus_init(void) { return true; }
static void mus_void(void) {}
static void mus_volume(int v) {}
static void *mus_register(void *data, int len) { return NULL; }
static void mus_unregister(void *h) {}
static void mus_play(void *h, boolean looping) {}
static boolean mus_playing(void) { return false; }

music_module_t DG_music_module = {
    NULL, 0, mus_init, mus_void, mus_volume, mus_void, mus_void, mus_register, mus_unregister, mus_play, mus_void,
    mus_playing, mus_void,
};

// ------------------------------------------------------------------ the engine task

static void doom_task(void *arg)
{
    static char skill[4];
    static char *argv[] = {"doom", "-iwad", NULL, "-warp", "1", "1", "-skill", skill, "-nomusic", NULL};
    static char iwad[48];
    const char *name = "doom1.wad";
    doom_wad_map(NULL, &name);
    snprintf(iwad, sizeof(iwad), "/wad/%s", name);
    snprintf(skill, sizeof(skill), "%d", s_skill + 1);
    argv[2] = iwad;
    myargc = 9;
    myargv = argv;

    // Full-screen view with no status bar; messages on.
    extern int screenblocks;
    screenblocks = 11;
    key_nextweapon = ']';

    while (!s_active) vTaskDelay(pdMS_TO_TICKS(20));
    s_paused_us = esp_timer_get_time();   // Doom's clock starts at zero
    Info_Init();
    M_FindResponseFile();
    D_DoomMain();
    s_main_done = true;
    ESP_LOGI(TAG, "running (PSRAM free %u KB, stack left %u)", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    for (;;) doomgeneric_Tick();
}

bool doom_port_start(int skill)
{
    if (s_started) return true;
    s_front[0] = heap_caps_calloc(1, DOOM_W * DOOM_H, MALLOC_CAP_SPIRAM);
    s_front[1] = heap_caps_calloc(1, DOOM_W * DOOM_H, MALLOC_CAP_SPIRAM);
    if (!s_front[0] || !s_front[1]) return false;
    s_skill = skill < 0 ? 0 : skill > 4 ? 4 : skill;
    // Map the WAD from here: reading the flash isn't allowed from a task whose stack is
    // in PSRAM, and the engine task's is. After this it only ever touches mapped memory.
    if (!doom_wad_map(NULL, NULL)) return false;
    // The stack lives in PSRAM too: fine, because this task never touches the flash
    // driver (the WAD is memory-mapped and nothing is ever saved from here).
    if (xTaskCreatePinnedToCoreWithCaps(doom_task, "doom", 64 * 1024, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS)
        return false;
    s_started = true;
    return true;
}

bool doom_port_started(void) { return s_started; }
void doom_port_set_active(bool active) { s_active = active; }

void doom_port_new_game(int skill)
{
    s_skill = skill < 0 ? 0 : skill > 4 ? 4 : skill;
    s_new_game = s_skill;
}

void doom_port_controls(int forward, int turn, bool fire, bool use)
{
    tat_forward = forward;
    tat_turn = turn;
    s_fire = fire;
    s_use = use;
}

void doom_port_next_weapon(void) { s_weapon_clicks++; }
void doom_port_set_sfx_volume(int vol_0_15) { s_sfx_gain = vol_0_15 / 15.0f; }

void doom_port_status(doom_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->frames = s_frames;
    if (s_failed) {
        out->state = DOOM_FAILED;
        return;
    }
    if (!s_main_done) {
        out->state = DOOM_LOADING;
        return;
    }
    switch (gamestate) {
    case GS_LEVEL: out->state = DOOM_LEVEL; break;
    case GS_INTERMISSION: out->state = DOOM_INTERMISSION; break;
    case GS_FINALE: out->state = DOOM_FINALE; break;
    default: out->state = DOOM_TITLE; break;
    }
    const player_t *pl = &players[consoleplayer];
    out->dead = pl->playerstate == PST_DEAD;
    out->health = pl->health;
    out->armor = pl->armorpoints;
    const ammotype_t at = weaponinfo[pl->readyweapon].ammo;
    out->ammo = at == am_noammo ? -1 : pl->ammo[at];
    out->episode = gameepisode;
    out->map = gamemap;
}

const char *doom_port_error(void) { return s_failed ? s_error : NULL; }
const uint8_t *doom_port_frame(void) { return s_front[s_front_i]; }

const uint8_t *doom_port_palette(uint32_t *generation)
{
    if (generation) *generation = s_pal_gen;
    return s_pal;
}
