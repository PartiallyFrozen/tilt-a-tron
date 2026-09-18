// Waveshare ESP32-S3-Touch-AMOLED-1.75C pin map (verified against the board schematic).
#pragma once

#include "driver/gpio.h"

// CO5300 AMOLED, QSPI, 466x466
#define PIN_LCD_CS      GPIO_NUM_12
#define PIN_LCD_SCLK    GPIO_NUM_38
#define PIN_LCD_D0      GPIO_NUM_4
#define PIN_LCD_D1      GPIO_NUM_5
#define PIN_LCD_D2      GPIO_NUM_6
#define PIN_LCD_D3      GPIO_NUM_7
#define PIN_LCD_RST     GPIO_NUM_1
#define PIN_LCD_TE      GPIO_NUM_13

// Shared I2C bus: CST9217 touch (0x5A), QMI8658 IMU (0x6B), AXP2101 PMU (0x34),
// ES8311 codec (0x18), ES7210 mic ADC (0x40)
#define PIN_I2C_SDA     GPIO_NUM_15
#define PIN_I2C_SCL     GPIO_NUM_14

#define PIN_TP_INT      GPIO_NUM_11
#define PIN_TP_RST      GPIO_NUM_2
#define PIN_IMU_INT1    GPIO_NUM_21

// Keys
#define PIN_KEY_BOOT    GPIO_NUM_0   // active low
#define PIN_KEY_PWR     GPIO_NUM_3   // SYS_OUT from AXP2101 PWRON key

// I2S audio (ES8311 out / ES7210 in) + speaker amp enable
#define PIN_I2S_MCLK    GPIO_NUM_16
#define PIN_I2S_BCLK    GPIO_NUM_9
#define PIN_I2S_WS      GPIO_NUM_45
#define PIN_I2S_DOUT    GPIO_NUM_8
#define PIN_I2S_DIN     GPIO_NUM_10
#define PIN_PA_EN       GPIO_NUM_46

// Free GPIOs on test pads: 17, 18, 39, 40, 41, 42 (+43/44 UART0)

#define LCD_W           466
#define LCD_H           466
#define LCD_X_GAP       6    // panel RAM is offset 6 columns
