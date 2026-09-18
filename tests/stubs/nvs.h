// A fake NVS that remembers a value and its width, so the migration path in wc::Store
// (an old u8 being read back as an i32) can actually be exercised.
#pragma once
#include <stdint.h>
#include <string.h>
#include "esp_err.h"

typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;

enum { FAKE_NONE = 0, FAKE_I32, FAKE_U8, FAKE_U16 };
typedef struct { char key[16]; int kind; int32_t value; } fake_entry_t;
extern fake_entry_t fake_nvs[16];

static inline fake_entry_t *fake_find(const char *key)
{
    for (int i = 0; i < 16; i++)
        if (fake_nvs[i].kind && !strcmp(fake_nvs[i].key, key)) return &fake_nvs[i];
    return 0;
}
static inline fake_entry_t *fake_slot(const char *key)
{
    fake_entry_t *e = fake_find(key);
    if (e) return e;
    for (int i = 0; i < 16; i++)
        if (!fake_nvs[i].kind) { snprintf(fake_nvs[i].key, 16, "%s", key); return &fake_nvs[i]; }
    return 0;
}

static inline esp_err_t nvs_open(const char *ns, nvs_open_mode_t m, nvs_handle_t *h) { (void)ns; (void)m; *h = 1; return ESP_OK; }
static inline void nvs_close(nvs_handle_t h) { (void)h; }
static inline esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

static inline esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t *v)
{ (void)h; fake_entry_t *e = fake_find(k); if (!e) return ESP_ERR_NOT_FOUND;
  if (e->kind != FAKE_I32) return ESP_ERR_NVS_TYPE_MISMATCH; *v = e->value; return ESP_OK; }
static inline esp_err_t nvs_get_u8(nvs_handle_t h, const char *k, uint8_t *v)
{ (void)h; fake_entry_t *e = fake_find(k); if (!e) return ESP_ERR_NOT_FOUND;
  if (e->kind != FAKE_U8) return ESP_ERR_NVS_TYPE_MISMATCH; *v = (uint8_t)e->value; return ESP_OK; }
static inline esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *v)
{ (void)h; fake_entry_t *e = fake_find(k); if (!e) return ESP_ERR_NOT_FOUND;
  if (e->kind != FAKE_U16) return ESP_ERR_NVS_TYPE_MISMATCH; *v = (uint16_t)e->value; return ESP_OK; }
static inline esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t v)
{ (void)h; fake_entry_t *e = fake_slot(k); if (!e) return ESP_FAIL;
  if (e->kind && e->kind != FAKE_I32) return ESP_ERR_NVS_TYPE_MISMATCH;   // NVS refuses a width change
  e->kind = FAKE_I32; e->value = v; return ESP_OK; }
static inline esp_err_t nvs_erase_key(nvs_handle_t h, const char *k)
{ (void)h; fake_entry_t *e = fake_find(k); if (!e) return ESP_ERR_NOT_FOUND; e->kind = FAKE_NONE; return ESP_OK; }
static inline esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *len)
{ (void)h; (void)k; (void)v; (void)len; return ESP_ERR_NOT_FOUND; }
static inline esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t len)
{ (void)h; (void)k; (void)v; (void)len; return ESP_OK; }
