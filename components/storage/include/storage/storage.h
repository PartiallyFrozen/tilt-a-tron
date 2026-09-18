// Where the console keeps files: a 16 MB FAT partition mounted at /data. Only the watch
// ever touches it - files arrive from a computer over the USB link (components/link), not
// by handing the filesystem to Windows, which is how a set of themes was lost once.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_ROOT "/data"
#define STORAGE_THEMES STORAGE_ROOT "/Theme"

esp_err_t storage_init(void);
bool storage_ready(void);            // mounted and usable
// The built-in (Default theme) icon PNG for an app id, embedded in the firmware.
bool storage_builtin_icon(const char *id, const uint8_t **png, size_t *len);
bool storage_free_bytes(uint32_t *total, uint32_t *free_bytes);   // of the storage area
// Wipes the storage area and lays down a fresh, empty filesystem. Everything on it is
// lost; the caller restarts so the Default theme and Guide are seeded again.
esp_err_t storage_format(void);
// Bumps whenever the files change under the console's feet - a theme arriving over the
// link, say - so the theme reloads without a restart. Call storage_changed() after writing.
uint32_t storage_generation(void);
void storage_changed(void);

#ifdef __cplusplus
}
#endif
