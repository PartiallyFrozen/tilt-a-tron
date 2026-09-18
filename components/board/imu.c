// Minimal QMI8658 driver: accel ±4g, gyro ±1024 dps, ~500 Hz ODR, burst read.
#include "board/board.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu";
static i2c_master_dev_handle_t s_dev;

enum {
    REG_WHO_AM_I = 0x00,
    REG_CTRL1 = 0x02, REG_CTRL2 = 0x03, REG_CTRL3 = 0x04, REG_CTRL5 = 0x06, REG_CTRL7 = 0x08,
    REG_AX_L = 0x35,
    REG_RESET = 0x60,
};

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 50);
}

esp_err_t imu_init(void)
{
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x6B,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c(), &dev, &s_dev), TAG, "add dev");

    wr(REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t reg = REG_WHO_AM_I, id = 0;
    ESP_RETURN_ON_ERROR(i2c_master_transmit_receive(s_dev, &reg, 1, &id, 1, 50), TAG, "whoami");
    if (id != 0x05) {
        ESP_LOGE(TAG, "unexpected WHO_AM_I 0x%02x", id);
        return ESP_ERR_NOT_FOUND;
    }

    wr(REG_CTRL1, 0x40);   // register auto-increment, little endian
    wr(REG_CTRL2, 0x14);   // accel ±4g, ODR ~500 Hz
    wr(REG_CTRL3, 0x54);   // gyro ±1024 dps, ODR ~500 Hz
    wr(REG_CTRL5, 0x11);   // low-pass filters on for accel+gyro
    wr(REG_CTRL7, 0x03);   // enable accel + gyro
    ESP_LOGI(TAG, "QMI8658 ready");
    return ESP_OK;
}

void imu_sleep(bool sleep)
{
    if (s_dev) wr(REG_CTRL7, sleep ? 0x00 : 0x03);
}

esp_err_t imu_read(imu_sample_t *out)
{
    uint8_t reg = REG_AX_L, b[12];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof(b), 20);
    out->t_us = esp_timer_get_time();
    if (err != ESP_OK) return err;

    const float a = 4.0f / 32768.0f, g = 1024.0f / 32768.0f;
    out->ax = (int16_t)(b[0] | b[1] << 8) * a;
    out->ay = (int16_t)(b[2] | b[3] << 8) * a;
    out->az = (int16_t)(b[4] | b[5] << 8) * a;
    out->gx = (int16_t)(b[6] | b[7] << 8) * g;
    out->gy = (int16_t)(b[8] | b[9] << 8) * g;
    out->gz = (int16_t)(b[10] | b[11] << 8) * g;
    return ESP_OK;
}
