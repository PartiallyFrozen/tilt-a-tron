#include "board/board.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "board";
static i2c_master_bus_handle_t s_i2c;

i2c_master_bus_handle_t board_i2c(void) { return s_i2c; }

esp_err_t board_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &s_i2c), TAG, "i2c bus");

    ESP_RETURN_ON_ERROR(gpio_install_isr_service(ESP_INTR_FLAG_IRAM), TAG, "gpio isr");
    ESP_RETURN_ON_ERROR(buttons_init(), TAG, "buttons");
    if (touch_init() != ESP_OK) ESP_LOGE(TAG, "touch init failed");
    if (imu_init() != ESP_OK)   ESP_LOGE(TAG, "imu init failed");
    pmu_init();
    return ESP_OK;
}
