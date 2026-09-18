#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NVS_TYPE_MISMATCH 0x1102
static inline const char *esp_err_to_name(esp_err_t e) { (void)e; return "stub"; }
