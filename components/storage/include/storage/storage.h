// The Tilt-a-tron drive: a 16 MB FAT partition mounted at /data for apps, and
// (when DEBUG MODE is off) shown to a computer as a USB flash drive. While a
// computer has it open the app can't read it; on eject it comes back and the
// generation counter bumps so themes reload.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STORAGE_ROOT "/data"
#define STORAGE_THEMES STORAGE_ROOT "/Theme"

esp_err_t storage_init(bool usb_drive);
// Call from the app task now and then: does deferred work (restoring missing
// Default files) that must not run on the USB driver's task.
void storage_service(void);
bool storage_ready(void);            // mounted for the app right now
// The built-in (Default theme) icon PNG for an app id, embedded in the firmware.
bool storage_builtin_icon(const char *id, const uint8_t **png, size_t *len);
bool storage_on_computer(void);      // a computer currently has the drive open
bool storage_free_bytes(uint32_t *total, uint32_t *free_bytes);   // of the storage area
uint32_t storage_generation(void);   // bumps every time the drive returns to the app

// DEBUG MODE (persisted, default on for now): the USB port stays the flashing /
// log port. Off: the USB port is the Tilt-a-tron drive. Takes effect after restart.
bool storage_debug_mode(void);           // = !storage_usb_drive_enabled()
bool storage_usb_drive_enabled(void);    // Settings > USB DRIVE (default off)
void storage_set_usb_drive_enabled(bool on);
void storage_set_debug_mode(bool on);

#ifdef __cplusplus
}
#endif
