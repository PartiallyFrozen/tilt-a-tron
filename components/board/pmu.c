// AXP2101 power chip: USB power present, battery level, charging.
#include "board/board.h"

#include "esp_log.h"

#define AXP2101_ADDR 0x34
#define AXP2101_STATUS1 0x00
#define STATUS1_VBUS_GOOD (1 << 5)
#define AXP2101_STATUS2 0x01      // bits 6:5 = 01 charging, 10 discharging
#define AXP2101_BAT_PERCENT 0xA4  // fuel gauge, 0..100
#define AXP2101_COMMON_CFG 0x10   // bit 0: switch everything off, now
#define AXP2101_PWROFF_EN 0x22    // bit 1: a long hold of the key cuts the power; bit 0: ...and restarts instead
#define AXP2101_KEY_LEVELS 0x27   // bits 3:2: how long that hold is - 4, 6, 8 or 10 s
#define KEY_OFF_LEVEL_MASK (3 << 2)
#define KEY_OFF_LEVEL_10S (3 << 2)

static const char *TAG = "pmu";
static i2c_master_dev_handle_t s_dev;

static bool rd(uint8_t reg, uint8_t *val)
{
    if (!s_dev) return false;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 20) == ESP_OK;
}

static bool wr(uint8_t reg, uint8_t val)
{
    if (!s_dev) return false;
    const uint8_t out[2] = {reg, val};
    return i2c_master_transmit(s_dev, out, 2, 20) == ESP_OK;
}

esp_err_t pmu_init(void)
{
    const i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(board_i2c(), &dev, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not available");
        return err;
    }
    // The power chip has a long hold of PWR of its own: it cuts the power, whatever the
    // firmware is doing, which is the way out of a watch that has hung. It is worth keeping,
    // but out of the box it can be as short as the hold that opens the console's power menu,
    // and then the menu never appears - the watch just dies under the thumb. Ten seconds is
    // the longest it goes, and leaves the menu's hold well clear of it.
    uint8_t off_en = 0, levels = 0;
    if (rd(AXP2101_PWROFF_EN, &off_en) && rd(AXP2101_KEY_LEVELS, &levels)) {
        ESP_LOGI(TAG, "power key: PWROFF_EN 0x%02x, levels 0x%02x", off_en, levels);
        if ((levels & KEY_OFF_LEVEL_MASK) != KEY_OFF_LEVEL_10S)
            wr(AXP2101_KEY_LEVELS, (uint8_t)((levels & ~KEY_OFF_LEVEL_MASK) | KEY_OFF_LEVEL_10S));
    }
    return ESP_OK;
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

bool pmu_power_off(void)
{
    uint8_t val = 0;
    if (!rd(AXP2101_COMMON_CFG, &val)) return false;
    ESP_LOGI(TAG, "power off");
    return wr(AXP2101_COMMON_CFG, val | 1);
}
