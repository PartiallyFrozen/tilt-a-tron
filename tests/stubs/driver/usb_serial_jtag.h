#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef struct { int tx_buffer_size, rx_buffer_size; } usb_serial_jtag_driver_config_t;
#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() (usb_serial_jtag_driver_config_t){256, 256}
static inline esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t *c) { (void)c; return ESP_OK; }
static inline int usb_serial_jtag_read_bytes(void *b, uint32_t n, TickType_t t) { (void)b; (void)n; (void)t; return 0; }
static inline int usb_serial_jtag_write_bytes(const void *b, size_t n, TickType_t t) { (void)b; (void)t; return (int)n; }
