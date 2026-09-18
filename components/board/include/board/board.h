// Thin, allocation-free hardware layer. Everything here is plain C so the vendor
// component macros (designated initialisers) compile as-is.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "board/pins.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------- board
esp_err_t board_init(void);                 // I2C bus + all peripherals below
i2c_master_bus_handle_t board_i2c(void);

// ---------------------------------------------------------------- display
// Pixel data is RGB565 *big-endian* (byte-swapped), which is what the panel wants,
// so frames go out without per-pixel conversion.
esp_err_t display_init(uint32_t pclk_hz);
// Write a w*h rectangle; returns only once every byte is on the wire. x/w must be even-aligned (panel requirement).
// `px` must be DMA-capable internal memory for zero-copy transfer.
esp_err_t display_write(int x, int y, int w, int h, const uint16_t *px);
// Full-screen streaming: the window is set once (first = true) and every later
// chunk continues where the last one ended, so there's no per-chunk addressing.
// Chunks must be whole rows of LCD_W pixels, top to bottom.
esp_err_t display_stream(const uint16_t *px, int pixels, bool first);
void      display_set_brightness(uint8_t level);      // 0..255
// Panel sleep: blank + sleep-in; waking restores the last brightness.
void      display_sleep(bool sleep);
// Temporarily dim (idle warning) without changing the saved brightness.
void      display_dim(bool dim);
// Block until the start of the next vertical blank (TE rising edge) or timeout.
// Returns false on timeout.
bool      display_wait_vsync(uint32_t timeout_ms);
int64_t   display_last_vsync_us(void);
uint32_t  display_vsync_count(void);

// ---------------------------------------------------------------- touch
typedef struct {
    bool     down;
    uint16_t x, y;
    int64_t  t_us;   // when this sample was read
} touch_point_t;

esp_err_t touch_init(void);
bool      touch_irq_pending(void);          // INT line asserted since last read
esp_err_t touch_read(touch_point_t *out);

// ---------------------------------------------------------------- imu
typedef struct {
    float ax, ay, az;   // g
    float gx, gy, gz;   // deg/s
    int64_t t_us;
} imu_sample_t;

esp_err_t imu_init(void);
esp_err_t imu_read(imu_sample_t *out);
void      imu_sleep(bool sleep);

// ---------------------------------------------------------------- power
esp_err_t pmu_init(void);
bool      pmu_usb_power(void);   // USB cable supplying power (charger or computer)
int       pmu_battery_percent(void);   // 0..100, or -1 if unknown
bool      pmu_charging(void);

// ---------------------------------------------------------------- buttons
enum { BTN_BOOT = 1 << 0, BTN_PWR = 1 << 1 };
esp_err_t buttons_init(void);
uint32_t  buttons_read(void);               // bitmask of currently held buttons

#ifdef __cplusplus
}
#endif
