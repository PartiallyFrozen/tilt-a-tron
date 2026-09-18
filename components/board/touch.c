#include "board/board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_cst9217.h"

static const char *TAG = "touch";
static esp_lcd_touch_handle_t s_tp;
static volatile bool s_irq;

static void IRAM_ATTR tp_isr(void *arg) { s_irq = true; }

esp_err_t touch_init(void)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(board_i2c(), &io_cfg, &io), TAG, "io");

    const esp_lcd_touch_config_t cfg = {
        .x_max = LCD_W,
        .y_max = LCD_H,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = GPIO_NUM_NC,     // we own the INT pin ourselves (edge flag below)
        .levels = { .reset = 0, .interrupt = 0 },
        .flags = { .swap_xy = 0, .mirror_x = 1, .mirror_y = 1 },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst9217(io, &cfg, &s_tp), TAG, "cst9217");

    const gpio_config_t irq = {
        .pin_bit_mask = 1ULL << PIN_TP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&irq);
    gpio_isr_handler_add(PIN_TP_INT, tp_isr, NULL);
    return ESP_OK;
}

bool touch_irq_pending(void) { return s_irq; }

esp_err_t touch_read(touch_point_t *out)
{
    s_irq = false;
    esp_err_t err = esp_lcd_touch_read_data(s_tp);
    out->t_us = esp_timer_get_time();
    if (err != ESP_OK) return err;

    esp_lcd_touch_point_data_t pt;
    uint8_t n = 0;
    out->down = esp_lcd_touch_get_data(s_tp, &pt, &n, 1) == ESP_OK && n > 0;
    if (out->down) {
        out->x = pt.x;
        out->y = pt.y;
    }
    return ESP_OK;
}
