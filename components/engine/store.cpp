#include "engine/store.h"

#include <cstring>

#include "nvs.h"

namespace wc {

namespace {

// Everything is written as i32, so a setting can grow from a flag to a 0..3 choice
// without the saved value becoming unreadable. Reading tries the older widths too: the
// games used to store these as u8 and u16, and nobody should lose a high score to a
// refactor.
bool readInt(uint32_t h, const char *key, int32_t &out)
{
    if (nvs_get_i32(h, key, &out) == ESP_OK) return true;
    uint8_t u8 = 0;
    if (nvs_get_u8(h, key, &u8) == ESP_OK) {
        out = u8;
        return true;
    }
    uint16_t u16 = 0;
    if (nvs_get_u16(h, key, &u16) == ESP_OK) {
        out = u16;
        return true;
    }
    return false;
}

}  // namespace

Store::Store(const char *name_space, Mode mode) : writing_(mode == Write)
{
    nvs_handle_t h = 0;
    if (nvs_open(name_space, writing_ ? NVS_READWRITE : NVS_READONLY, &h) == ESP_OK) handle_ = h;
}

Store::~Store()
{
    if (!handle_) return;
    if (writing_) nvs_commit(handle_);
    nvs_close(handle_);
}

void Store::get(const char *key, int &v, int limit) const
{
    if (!handle_) return;
    int32_t raw = 0;
    if (!readInt(handle_, key, raw)) return;
    if (limit > 0 && (raw < 0 || raw >= limit)) return;   // a stale or corrupt value
    v = int(raw);
}

void Store::get(const char *key, bool &v) const
{
    int n = v ? 1 : 0;
    get(key, n);
    v = n != 0;
}

void Store::get(const char *key, float &v) const
{
    if (!handle_) return;
    size_t len = sizeof(float);
    float raw = 0;
    if (nvs_get_blob(handle_, key, &raw, &len) == ESP_OK && len == sizeof(float)) v = raw;
}

void Store::set(const char *key, int v)
{
    if (!handle_ || !writing_) return;
    // The key may still be there as the u8 it used to be, which NVS refuses to overwrite
    // with a different type. Clear it and write the new one.
    if (nvs_set_i32(handle_, key, int32_t(v)) != ESP_OK) {
        nvs_erase_key(handle_, key);
        nvs_set_i32(handle_, key, int32_t(v));
    }
}

void Store::set(const char *key, bool v)
{
    set(key, v ? 1 : 0);
}

void Store::set(const char *key, float v)
{
    if (handle_ && writing_) nvs_set_blob(handle_, key, &v, sizeof(v));
}

void Store::erase(const char *key)
{
    if (handle_ && writing_) nvs_erase_key(handle_, key);
}

}  // namespace wc
