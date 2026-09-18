#include "board/board.h"

// PWR key polarity on GPIO3 isn't documented; the bench app prints raw levels so
// we can confirm it on hardware. Assumed active-high (SYS_OUT via BSS138).
#define PWR_ACTIVE_LEVEL 1

esp_err_t buttons_init(void)
{
    const gpio_config_t boot = {
        .pin_bit_mask = 1ULL << PIN_KEY_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    const gpio_config_t pwr = {
        .pin_bit_mask = 1ULL << PIN_KEY_PWR,
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&boot);
    return gpio_config(&pwr);
}

uint32_t buttons_read(void)
{
    uint32_t m = 0;
    if (gpio_get_level(PIN_KEY_BOOT) == 0) m |= BTN_BOOT;
    if (gpio_get_level(PIN_KEY_PWR) == PWR_ACTIVE_LEVEL) m |= BTN_PWR;
    return m;
}
