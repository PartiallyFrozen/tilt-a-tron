#pragma once
typedef struct { const char *version; } esp_app_desc_t;
static const esp_app_desc_t tat_desc = {"test"};
static inline const esp_app_desc_t *esp_app_get_description(void) { return &tat_desc; }
