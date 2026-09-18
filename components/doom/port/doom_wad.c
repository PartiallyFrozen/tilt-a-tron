// Where the WAD lives: a 6 MB region near the end of the 32 MB flash, past every
// other partition. Newer partition tables name it ("wad"); on a watch that still has
// an older table the same region is registered at run time, so nothing needs reflashing.
//
//   +0x0000  header: "TATWAD1", length, original file name   (written last)
//   +0x1000  the WAD file, byte for byte
//
// Doom reads it through a memory mapping (doom_wad_map), so lumps are used in place.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doom/doom_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

#define WAD_OFFSET 0x1900000
#define WAD_SIZE 0x600000
#define WAD_DATA 0x1000
#define WAD_SUBTYPE 0x40

static const char *TAG = "doom_wad";

typedef struct {
    char magic[8];
    uint32_t length;
    char name[32];
} wad_header_t;

static const esp_partition_t *s_part;
static const uint8_t *s_map;
static esp_partition_mmap_handle_t s_map_handle;
static uint32_t s_write_len, s_written, s_erased;
static char s_write_name[32];

static const esp_partition_t *region(void)
{
    if (s_part) return s_part;
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "wad");
    if (!s_part) {
        esp_err_t err = esp_partition_register_external(NULL, WAD_OFFSET, WAD_SIZE, "wad", ESP_PARTITION_TYPE_DATA,
                                                        WAD_SUBTYPE, &s_part);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "can't claim the WAD region: %s", esp_err_to_name(err));
            s_part = NULL;
        }
    }
    return s_part;
}

static bool read_header(wad_header_t *h)
{
    const esp_partition_t *p = region();
    if (!p || esp_partition_read(p, 0, h, sizeof(*h)) != ESP_OK) return false;
    if (memcmp(h->magic, "TATWAD1", 8) != 0) return false;
    if (h->length < 12 || h->length > p->size - WAD_DATA) return false;
    h->name[sizeof(h->name) - 1] = 0;
    return true;
}

bool doom_wad_info(doom_wad_info_t *out)
{
    wad_header_t h;
    memset(out, 0, sizeof(*out));
    if (!read_header(&h)) return false;
    // A WAD starts with "IWAD" or "PWAD".
    char id[4];
    if (esp_partition_read(region(), WAD_DATA, id, 4) != ESP_OK || memcmp(id + 1, "WAD", 3) != 0) return false;
    out->present = true;
    out->length = h.length;
    strlcpy(out->name, h.name, sizeof(out->name));
    return true;
}

// For the engine (doom_port.c): the mapped WAD, or NULL.
const uint8_t *doom_wad_map(uint32_t *length, const char **name)
{
    static doom_wad_info_t info;
    if (!s_map) {
        if (!doom_wad_info(&info)) return NULL;
        const void *ptr = NULL;
        esp_err_t err = esp_partition_mmap(region(), 0, WAD_DATA + info.length, ESP_PARTITION_MMAP_DATA, &ptr, &s_map_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mmap failed: %s", esp_err_to_name(err));
            return NULL;
        }
        s_map = (const uint8_t *)ptr + WAD_DATA;
        ESP_LOGI(TAG, "%s: %u bytes mapped at %p", info.name, (unsigned)info.length, s_map);
    }
    if (length) *length = info.length;
    if (name) *name = info.name;
    return s_map;
}

bool doom_wad_write_begin(const char *name, uint32_t length)
{
    const esp_partition_t *p = region();
    if (!p || s_map) return false;   // in use: the engine reads straight from it
    if (length < 12 || length > p->size - WAD_DATA) return false;
    // Wipe the header first, so a half-written WAD is never mistaken for a whole one.
    if (esp_partition_erase_range(p, 0, WAD_DATA) != ESP_OK) return false;
    const char *base = strrchr(name ? name : "", '/');
    strlcpy(s_write_name, base ? base + 1 : (name && *name ? name : "doom1.wad"), sizeof(s_write_name));
    for (char *c = s_write_name; *c; c++)
        if (*c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
    s_write_len = length;
    s_written = 0;
    s_erased = WAD_DATA;
    return true;
}

bool doom_wad_write(const void *data, size_t len)
{
    const esp_partition_t *p = region();
    if (!p || !s_write_len || s_written + len > s_write_len) return false;
    // Erase just ahead of the writes, 64 KB at a time, so progress is steady.
    const uint32_t end = WAD_DATA + s_written + len;
    while (s_erased < end) {
        if (esp_partition_erase_range(p, s_erased, 0x10000 - (s_erased & 0xFFFF)) != ESP_OK) return false;
        s_erased += 0x10000 - (s_erased & 0xFFFF);
    }
    if (esp_partition_write(p, WAD_DATA + s_written, data, len) != ESP_OK) return false;
    s_written += len;
    return true;
}

bool doom_wad_write_end(void)
{
    const esp_partition_t *p = region();
    const bool whole = p && s_write_len && s_written == s_write_len;
    if (whole) {
        wad_header_t h;
        memset(&h, 0, sizeof(h));
        memcpy(h.magic, "TATWAD1", 8);
        h.length = s_write_len;
        strlcpy(h.name, s_write_name, sizeof(h.name));
        if (esp_partition_write(p, 0, &h, sizeof(h)) != ESP_OK) return false;
        ESP_LOGI(TAG, "stored %s, %u bytes", h.name, (unsigned)h.length);
    }
    s_write_len = 0;
    return whole;
}

bool doom_wad_install_file(const char *path, void (*progress)(int percent))
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    const size_t chunk = 32768;
    uint8_t *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    bool ok = buf && size > 0 && doom_wad_write_begin(path, (uint32_t)size);
    long done = 0;
    while (ok && done < size) {
        const size_t n = fread(buf, 1, chunk, f);
        if (n == 0) break;
        ok = doom_wad_write(buf, n);
        done += (long)n;
        if (progress) progress((int)(done * 100 / size));
    }
    ok = ok && done == size && doom_wad_write_end();
    free(buf);
    fclose(f);
    return ok;
}
