#include "board/board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_co5300.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_vsync_sem;
static volatile int64_t s_vsync_us;
static volatile uint32_t s_vsync_count;

// Waveshare's vendor init for this panel. 0x35 0x00 enables TE (V-blank only),
// which we use to start frame pushes right as the panel finishes a scan.
static const co5300_lcd_init_cmd_t k_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},       // RGB565
    {0x35, (uint8_t[]){0x00}, 1, 0},       // TE on
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 120},
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 0},
};

static SemaphoreHandle_t s_xfer_done;

// esp_lcd queues color data and returns before it's on the wire; this fires when
// the last chunk has actually been sent.
static bool IRAM_ATTR color_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_xfer_done, &woken);
    return woken == pdTRUE;
}

static void IRAM_ATTR te_isr(void *arg)
{
    s_vsync_us = esp_timer_get_time();
    s_vsync_count++;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

esp_err_t display_init(uint32_t pclk_hz)
{
    // Largest single transfer we ever do is one band from the present pipeline;
    // size generously so a full-width 64-row band fits.
    const int max_xfer = LCD_W * 64 * 2 + 64;
    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_SCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3, max_xfer);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    s_xfer_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, color_done_cb, NULL);
    io_cfg.pclk_hz = pclk_hz;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    co5300_vendor_config_t vendor = {
        .init_cmds = k_init_cmds,
        .init_cmds_size = sizeof(k_init_cmds) / sizeof(k_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel), TAG, "panel");
    esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_disp_on_off(s_panel, true);

    // Tearing-effect line -> vsync semaphore
    s_vsync_sem = xSemaphoreCreateBinary();
    const gpio_config_t te = {
        .pin_bit_mask = 1ULL << PIN_LCD_TE,
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    gpio_config(&te);
    gpio_isr_handler_add(PIN_LCD_TE, te_isr, NULL);

    ESP_LOGI(TAG, "CO5300 up, QSPI %lu Hz", (unsigned long)pclk_hz);
    return ESP_OK;
}

esp_err_t display_write(int x, int y, int w, int h, const uint16_t *px)
{
    xSemaphoreTake(s_xfer_done, 0);
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, x, y, x + w, y + h, px);
    if (err != ESP_OK) return err;
    // Caller recycles `px` right after we return, so it must be fully sent first.
    return xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(500)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t display_stream(const uint16_t *px, int pixels, bool first)
{
    if (first) {
        const int x0 = LCD_X_GAP, x1 = LCD_X_GAP + LCD_W - 1, y1 = LCD_H - 1;
        const uint8_t cols[4] = {(x0 >> 8) & 0xFF, x0 & 0xFF, (x1 >> 8) & 0xFF, x1 & 0xFF};
        const uint8_t rows[4] = {0, 0, (y1 >> 8) & 0xFF, y1 & 0xFF};
        esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x2A << 8), cols, 4);
        esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x2B << 8), rows, 4);
    }
    xSemaphoreTake(s_xfer_done, 0);
    // 0x2C starts a memory write, 0x3C continues it from where the last one stopped.
    esp_err_t err = esp_lcd_panel_io_tx_color(s_io, (0x32 << 24) | ((first ? 0x2C : 0x3C) << 8), px, pixels * 2);
    if (err != ESP_OK) return err;
    return xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(500)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static uint8_t s_brightness = 0xFF;

static void send_brightness(uint8_t level)
{
    esp_lcd_panel_io_tx_param(s_io, (0x02 << 24) | (0x51 << 8), &level, 1);
}

void display_set_brightness(uint8_t level)
{
    s_brightness = level;
    send_brightness(level);
}

void display_dim(bool dim)
{
    send_brightness(dim ? (s_brightness < 30 ? s_brightness : 30) : s_brightness);
}

void display_sleep(bool sleep)
{
    if (sleep) {
        send_brightness(0);
        esp_lcd_panel_disp_sleep(s_panel, true);
    } else {
        esp_lcd_panel_disp_sleep(s_panel, false);
        send_brightness(s_brightness);
    }
}

bool display_wait_vsync(uint32_t timeout_ms)
{
    xSemaphoreTake(s_vsync_sem, 0);   // drop a stale edge so we sync to the *next* blank
    return xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

int64_t display_last_vsync_us(void) { return s_vsync_us; }
uint32_t display_vsync_count(void) { return s_vsync_count; }
