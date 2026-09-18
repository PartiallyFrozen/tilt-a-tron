#include "storage/storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "wear_levelling.h"

static const char *TAG = "storage";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static volatile bool s_ready;
static volatile uint32_t s_generation;
static volatile bool s_seed_pending;

// Default theme + README, embedded from themes/ in the repo.
#define EMBED(sym) \
    extern const uint8_t _binary_##sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t _binary_##sym##_end[] asm("_binary_" #sym "_end");
EMBED(README_txt)
EMBED(theme_json)
EMBED(background_png)
EMBED(breakout_png)
EMBED(settings_png)
EMBED(maze_png)
EMBED(racer_png)
EMBED(jump_png)
EMBED(tiltatris_png)
EMBED(clock_png)
EMBED(star_png)
EMBED(guide_home_png)
EMBED(guide_menus_png)
EMBED(guide_icon_png)

static void write_file(const char *path, const uint8_t *start, const uint8_t *end, bool only_if_missing)
{
    struct stat st;
    if (stat(path, &st) == 0 && (only_if_missing || st.st_size == end - start)) return;
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "can't create %s (%d)", path, errno);
        return;
    }
    fwrite(start, 1, end - start, f);
    fclose(f);
}

static void write_if_missing(const char *path, const uint8_t *start, const uint8_t *end)
{
    write_file(path, start, end, true);
}

bool storage_builtin_icon(const char *id, const uint8_t **png, size_t *len)
{
    static const struct {
        const char *id;
        const uint8_t *start, *end;
    } icons[] = {
        {"breakout", _binary_breakout_png_start, _binary_breakout_png_end},
        {"settings", _binary_settings_png_start, _binary_settings_png_end},
        {"maze", _binary_maze_png_start, _binary_maze_png_end},
        {"racer", _binary_racer_png_start, _binary_racer_png_end},
        {"jump", _binary_jump_png_start, _binary_jump_png_end},
        {"tiltatris", _binary_tiltatris_png_start, _binary_tiltatris_png_end},
        {"clock", _binary_clock_png_start, _binary_clock_png_end},
        {"star", _binary_star_png_start, _binary_star_png_end},
    };
    for (size_t i = 0; i < sizeof(icons) / sizeof(icons[0]); i++) {
        if (strcmp(icons[i].id, id) != 0) continue;
        *png = icons[i].start;
        *len = icons[i].end - icons[i].start;
        return true;
    }
    return false;
}

// Make sure Theme/Default exists with the built-in files, so there's always a
// working theme to copy. A new firmware may ship different Default files (new
// icons, say). Those are refreshed *only* if the copy on the drive is still the
// one an earlier firmware wrote: anything the user changed or replaced stays.
// The record of what the firmware wrote lives in Theme/Default/.firmware.
#define SEED_RECORD STORAGE_THEMES "/Default/.firmware"

static uint32_t fnv1a(uint32_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static bool file_hash(const char *path, uint32_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "can't read %s (%d)", path, errno);
        return false;
    }
    uint32_t h = 2166136261u;
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) h = fnv1a(h, buf, n);
    fclose(f);
    *out = h;
    return true;
}

typedef struct {
    char name[40];
    uint32_t hash;
} seed_rec_t;
static seed_rec_t s_recs[16];
static int s_nrecs;

static uint32_t *record_for(const char *name)
{
    for (int i = 0; i < s_nrecs; i++)
        if (strcmp(s_recs[i].name, name) == 0) return &s_recs[i].hash;
    return NULL;
}

static void record_set(const char *name, uint32_t hash)
{
    uint32_t *h = record_for(name);
    if (h) {
        *h = hash;
    } else if (s_nrecs < (int)(sizeof(s_recs) / sizeof(s_recs[0]))) {
        strlcpy(s_recs[s_nrecs].name, name, sizeof(s_recs[s_nrecs].name));
        s_recs[s_nrecs++].hash = hash;
    }
}

static void records_load(void)
{
    s_nrecs = 0;
    FILE *f = fopen(SEED_RECORD, "r");
    if (!f) return;
    char line[64];
    while (fgets(line, sizeof(line), f) && s_nrecs < (int)(sizeof(s_recs) / sizeof(s_recs[0]))) {
        char name[40];
        unsigned hash;
        if (sscanf(line, "%39s %x", name, &hash) == 2) record_set(name, hash);
    }
    fclose(f);
}

static void records_save(void)
{
    FILE *f = fopen(SEED_RECORD, "w");
    if (!f) return;
    for (int i = 0; i < s_nrecs; i++) fprintf(f, "%s %08x\n", s_recs[i].name, (unsigned)s_recs[i].hash);
    fclose(f);
}

// `name` is the path under Theme/Default. Writes the firmware's copy when the
// file is missing, or when it is unchanged since the firmware last wrote it and
// the firmware now ships something different.
static void seed_default_file(const char *name, const uint8_t *start, const uint8_t *end)
{
    char path[96];
    snprintf(path, sizeof(path), STORAGE_THEMES "/Default/%s", name);
    const uint32_t fw = fnv1a(2166136261u, start, end - start);
    uint32_t cur = 0;
    const bool exists = file_hash(path, &cur);
    const uint32_t *rec = record_for(name);
    bool write = !exists;
    if (exists) {
        if (rec) write = (cur == *rec) && (fw != cur);   // ours and out of date
        else if (cur == fw) record_set(name, fw);         // pre-tracking file that matches: adopt it
        // Otherwise the user made it: leave it, and don't record it as ours.
    }
    ESP_LOGD(TAG, "Default/%s: exists %d cur %08x rec %08x fw %08x -> %s", name, exists, (unsigned)cur,
             rec ? (unsigned)*rec : 0u, (unsigned)fw, write ? "write" : "keep");
    if (write) {
        write_file(path, start, end, false);
        record_set(name, fw);
        ESP_LOGI(TAG, "Default/%s refreshed", name);
    }
}

static void seed_defaults(void)
{
    mkdir(STORAGE_THEMES, 0775);
    mkdir(STORAGE_THEMES "/Default", 0775);
    mkdir(STORAGE_THEMES "/Default/icons", 0775);
    // The README is ours, not the user's: keep it current with the firmware.
    write_file(STORAGE_ROOT "/README.txt", _binary_README_txt_start, _binary_README_txt_end, false);

    records_load();
    seed_default_file("theme.json", _binary_theme_json_start, _binary_theme_json_end);
    seed_default_file("background.png", _binary_background_png_start, _binary_background_png_end);
    seed_default_file("icons/breakout.png", _binary_breakout_png_start, _binary_breakout_png_end);
    seed_default_file("icons/settings.png", _binary_settings_png_start, _binary_settings_png_end);
    seed_default_file("icons/maze.png", _binary_maze_png_start, _binary_maze_png_end);
    seed_default_file("icons/racer.png", _binary_racer_png_start, _binary_racer_png_end);
    seed_default_file("icons/jump.png", _binary_jump_png_start, _binary_jump_png_end);
    seed_default_file("icons/tiltatris.png", _binary_tiltatris_png_start, _binary_tiltatris_png_end);
    seed_default_file("icons/clock.png", _binary_clock_png_start, _binary_clock_png_end);
    seed_default_file("icons/star.png", _binary_star_png_start, _binary_star_png_end);
    records_save();
    unlink(STORAGE_THEMES "/Default/icons/ringdrop.png");   // the game was renamed

    // Design templates: where icons, titles and menu rows land on the round screen.
    mkdir(STORAGE_ROOT "/Guide", 0775);
    write_if_missing(STORAGE_ROOT "/Guide/guide-home.png", _binary_guide_home_png_start,
                     _binary_guide_home_png_end);
    write_if_missing(STORAGE_ROOT "/Guide/guide-menus.png", _binary_guide_menus_png_start,
                     _binary_guide_menus_png_end);
    write_if_missing(STORAGE_ROOT "/Guide/guide-icon.png", _binary_guide_icon_png_start,
                     _binary_guide_icon_png_end);
}

// A never-used partition reads as all 0xFF. Only such a partition may be
// formatted automatically; a damaged one keeps its data for a computer to repair.
static bool partition_blank(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!part) return false;
    uint8_t buf[256];
    for (size_t off = 0; off < 8192; off += sizeof(buf)) {
        if (esp_partition_read(part, off, buf, sizeof(buf)) != ESP_OK) return false;
        for (size_t i = 0; i < sizeof(buf); i++)
            if (buf[i] != 0xFF) return false;
    }
    return true;
}

esp_err_t storage_init(void)
{
    const bool blank = partition_blank();
    if (blank) ESP_LOGI(TAG, "blank drive: formatting");
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = blank,
        .max_files = 8,
        .allocation_unit_size = 0,
    };

    {
        esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_ROOT, "storage", &mount, &s_wl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
            return err;
        }
        uint64_t total = 0, free_bytes = 0;
        esp_vfs_fat_info(STORAGE_ROOT, &total, &free_bytes);
        ESP_LOGI(TAG, "drive: %llu KB, %llu KB free", total / 1024, free_bytes / 1024);
        if (total == 0 || (mkdir(STORAGE_THEMES, 0775) != 0 && errno != EEXIST)) {
            if (!blank) {
                // Damaged, but it has data: back it up with tools/tatlink.py --backup and
                // then --format, rather than wiping somebody's themes here.
                ESP_LOGE(TAG, "storage damaged (errno %d): not formatting, back it up and format it", errno);
                return ESP_FAIL;
            }
            ESP_LOGW(TAG, "drive unusable (errno %d), formatting", errno);
            if ((err = esp_vfs_fat_spiflash_format_rw_wl(STORAGE_ROOT, "storage")) != ESP_OK) {
                ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
                return err;
            }
            esp_vfs_fat_info(STORAGE_ROOT, &total, &free_bytes);
            ESP_LOGI(TAG, "formatted: %llu KB, %llu KB free", total / 1024, free_bytes / 1024);
        }
        seed_defaults();
        s_ready = true;
        s_generation++;
        return ESP_OK;
    }
}

void storage_changed(void) { s_generation++; }
esp_err_t storage_format(void)
{
    const esp_err_t err = esp_vfs_fat_spiflash_format_rw_wl(STORAGE_ROOT, "storage");
    ESP_LOGW(TAG, "format: %s", esp_err_to_name(err));
    return err;
}

bool storage_free_bytes(uint32_t *total, uint32_t *free_bytes)
{
    uint64_t t = 0, f = 0;
    if (esp_vfs_fat_info(STORAGE_ROOT, &t, &f) != ESP_OK) return false;
    *total = (uint32_t)t;
    *free_bytes = (uint32_t)f;
    return true;
}

bool storage_ready(void) { return s_ready; }
uint32_t storage_generation(void) { return s_generation; }


