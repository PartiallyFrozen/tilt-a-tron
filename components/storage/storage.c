#include "storage/storage.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"
#include "wear_levelling.h"

static const char *TAG = "storage";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static volatile bool s_ready;
static volatile bool s_on_computer;
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

// Make sure Theme/Default exists with the built-in files, so there's always a
// working theme to copy. Files the user already changed are left alone.
static void seed_defaults(void)
{
    mkdir(STORAGE_THEMES, 0775);
    mkdir(STORAGE_THEMES "/Default", 0775);
    mkdir(STORAGE_THEMES "/Default/icons", 0775);
    // The README is ours, not the user's: keep it current with the firmware.
    write_file(STORAGE_ROOT "/README.txt", _binary_README_txt_start, _binary_README_txt_end, false);
    write_if_missing(STORAGE_THEMES "/Default/theme.json", _binary_theme_json_start, _binary_theme_json_end);
    write_if_missing(STORAGE_THEMES "/Default/background.png", _binary_background_png_start,
                     _binary_background_png_end);
    write_if_missing(STORAGE_THEMES "/Default/icons/breakout.png", _binary_breakout_png_start,
                     _binary_breakout_png_end);
    write_if_missing(STORAGE_THEMES "/Default/icons/settings.png", _binary_settings_png_start,
                     _binary_settings_png_end);
    write_if_missing(STORAGE_THEMES "/Default/icons/maze.png", _binary_maze_png_start, _binary_maze_png_end);
    write_if_missing(STORAGE_THEMES "/Default/icons/racer.png", _binary_racer_png_start, _binary_racer_png_end);
    write_if_missing(STORAGE_THEMES "/Default/icons/jump.png", _binary_jump_png_start, _binary_jump_png_end);

    // Design templates: where icons, titles and menu rows land on the round screen.
    mkdir(STORAGE_ROOT "/Guide", 0775);
    write_if_missing(STORAGE_ROOT "/Guide/guide-home.png", _binary_guide_home_png_start,
                     _binary_guide_home_png_end);
    write_if_missing(STORAGE_ROOT "/Guide/guide-menus.png", _binary_guide_menus_png_start,
                     _binary_guide_menus_png_end);
    write_if_missing(STORAGE_ROOT "/Guide/guide-icon.png", _binary_guide_icon_png_start,
                     _binary_guide_icon_png_end);
}

static void on_msc_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    switch (event->id) {
    case TINYUSB_MSC_EVENT_MOUNT_START:
        s_ready = false;   // switching owner: nobody should touch files right now
        break;
    case TINYUSB_MSC_EVENT_MOUNT_COMPLETE:
        if (event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP) {
            // File writes happen in storage_service() on the app task: this callback
            // runs on the USB driver's small stack.
            s_seed_pending = true;
            s_on_computer = false;
            s_ready = true;
            s_generation++;
            ESP_LOGI(TAG, "drive back with the console");
        } else {
            s_on_computer = true;
            ESP_LOGI(TAG, "drive opened on a computer");
        }
        break;
    case TINYUSB_MSC_EVENT_MOUNT_FAILED:
    case TINYUSB_MSC_EVENT_FORMAT_FAILED:
        ESP_LOGE(TAG, "drive mount failed (event %d)", event->id);
        break;
    default: break;
    }
}

esp_err_t storage_init(bool usb_drive)
{
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files = 8,
        .allocation_unit_size = 0,
    };

    if (!usb_drive) {
        esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_ROOT, "storage", &mount, &s_wl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(err));
            return err;
        }
        uint64_t total = 0, free_bytes = 0;
        esp_vfs_fat_info(STORAGE_ROOT, &total, &free_bytes);
        ESP_LOGI(TAG, "drive: %llu KB, %llu KB free", total / 1024, free_bytes / 1024);
        if (total == 0 || (mkdir(STORAGE_THEMES, 0775) != 0 && errno != EEXIST)) {
            // Leftover data from an older partition layout: start with a clean drive.
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

    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, "storage");
    if (!part) return ESP_ERR_NOT_FOUND;
    esp_err_t err = wl_mount(part, &s_wl);
    if (err != ESP_OK) return err;

    const tinyusb_msc_driver_config_t driver = {.callback = on_msc_event};
    if ((err = tinyusb_msc_install_driver(&driver)) != ESP_OK) return err;

    tinyusb_msc_storage_handle_t handle;
    const tinyusb_msc_storage_config_t cfg = {
        .medium.wl_handle = s_wl,
        .fat_fs = {.base_path = (char *)STORAGE_ROOT, .config = mount},
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    if ((err = tinyusb_msc_new_storage_spiflash(&cfg, &handle)) != ESP_OK) return err;

    tinyusb_msc_mount_point_t mp;
    if (tinyusb_msc_get_storage_mount_point(handle, &mp) == ESP_OK && mp == TINYUSB_MSC_STORAGE_MOUNT_APP) {
        seed_defaults();
        s_ready = true;
        s_generation++;
    }

    const tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG();
    if ((err = tinyusb_driver_install(&usb)) != ESP_OK) return err;
    ESP_LOGI(TAG, "USB drive ready");
    return ESP_OK;
}

void storage_service(void)
{
    if (!s_seed_pending || !s_ready) return;
    s_seed_pending = false;
    seed_defaults();
}

bool storage_ready(void) { return s_ready; }
bool storage_on_computer(void) { return s_on_computer; }
uint32_t storage_generation(void) { return s_generation; }

bool storage_debug_mode(void)
{
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open("console", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "debug", &v);
        nvs_close(h);
    }
    return v;
}

void storage_set_debug_mode(bool on)
{
    nvs_handle_t h;
    if (nvs_open("console", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "debug", on);
    nvs_commit(h);
    nvs_close(h);
}
