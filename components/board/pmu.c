// AXP2101 power chip: USB power present, battery level, charging.
#include "board/board.h"

#include "esp_log.h"

#define AXP2101_ADDR 0x34
#define AXP2101_STATUS1 0x00
#define STATUS1_VBUS_GOOD (1 << 5)
#define AXP2101_STATUS2 0x01      // bits 6:5 = 01 charging, 10 discharging
#define AXP2101_BAT_PERCENT 0xA4  // fuel gauge, 0..100

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

static bool rd(uint8_t reg, uint8_t *val)
{
    if (!s_dev) return false;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 20) == ESP_OK;
}

bool pmu_usb_power(void)
{
    uint8_t val = 0;
    return rd(AXP2101_STATUS1, &val) && (val & STATUS1_VBUS_GOOD);
}

int pmu_battery_percent(void)
{
    uint8_t val = 0;
    if (!rd(AXP2101_BAT_PERCENT, &val) || val > 100) return -1;
    return val;
}

bool pmu_charging(void)
{
    uint8_t val = 0;
    return rd(AXP2101_STATUS2, &val) && ((val >> 5) & 3) == 1;
}
