// AXP2101 power chip: just enough to tell whether USB power is present.
#include "board/board.h"

#include "esp_log.h"

#define AXP2101_ADDR 0x34
#define AXP2101_STATUS1 0x00
#define STATUS1_VBUS_GOOD (1 << 5)

static const char *TAG = "pmu";
static i2c_master_dev_handle_t s_dev;

esp_err_t pmu_init(void)
{
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c(), &dev, &s_dev);
    if (err != ESP_OK) ESP_LOGW(TAG, "AXP2101 not available");
    return err;
}

bool pmu_usb_power(void)
{
    if (!s_dev) return false;
    uint8_t reg = AXP2101_STATUS1, val = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &val, 1, 20) != ESP_OK) return false;
    return val & STATUS1_VBUS_GOOD;
}
