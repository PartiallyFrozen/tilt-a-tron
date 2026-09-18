#include "link/link.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "storage/storage.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "link";

static void (*s_info_hook)(uint32_t *, uint32_t *, uint8_t *);
static int (*s_list_hook)(link_game_t *, int);
static bool (*s_icon_hook)(const char *, const uint8_t **, size_t *);
static const char *s_fs_root;

// One reply buffer, shared: the server handles a single command at a time, and internal
// RAM is what Wi-Fi and the display pipeline are short of.
static uint8_t s_scratch[LINK_MAX_PAYLOAD];

void link_set_fs_root(const char *root) { s_fs_root = root; }

static void (*s_changed_hook)(void);
void link_set_changed_hook(void (*fn)(void)) { s_changed_hook = fn; }
static void note_changed(void)
{
    if (s_changed_hook) s_changed_hook();
}

// An upload in progress. Only one at a time: there is one app and one wire.
#define PUT_BUF (32 * 1024)
static uint8_t *s_put_buf;
static FILE *s_put;
static uint32_t s_put_left, s_put_crc, s_put_want;
static char s_put_path[160];

static void put_abort(void)
{
    if (s_put) {
        fclose(s_put);
        unlink(s_put_path);   // a half-written file is worse than none
        s_put = NULL;
    }
    s_put_left = 0;
}

// Refuses anything that could escape the storage root. Returns false if it won't do.
static bool safe_path(const char *rel, uint16_t len, char *out, size_t out_len)
{
    if (!s_fs_root || len > 120) return false;
    if (len == 0) return snprintf(out, out_len, "%s", s_fs_root) < (int)out_len;   // the root itself
    for (uint16_t i = 0; i < len; i++) {
        const char c = rel[i];
        if (c == 0 || c == '\\' || c < 0x20) return false;
        if (c == '.' && i + 1 < len && rel[i + 1] == '.') return false;
    }
    if (rel[0] == '/') return false;
    return snprintf(out, out_len, "%s/%.*s", s_fs_root, (int)len, rel) < (int)out_len;
}

// Deleting a folder: read every name first, close the directory, and only then delete.
// Unlinking while the directory handle is still walking it corrupts the walk on FAT - that
// crashed the watch the first time this ran.
// This drive carries some phantom directory entries left by an old FAT corruption: names
// of 0xFF bytes with a nonsense size. console/theme.cpp skips them the same way. They must
// never be walked into or deleted - just ignored.
static bool real_entry(const char *name)
{
    if (!name[0]) return false;
    for (const char *c = name; *c; c++)
        if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E) return false;
    return true;
}

static void rm_recursive(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    if (depth > 4) return;   // themes are two deep; anything more is a mistake

    char *names = NULL;
    int count = 0;
    DIR *d = opendir(path);
    if (d) {
        names = malloc(64 * 64);   // up to 64 entries per folder, 63-char names
        const struct dirent *e;
        while (names && count < 64 && (e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !real_entry(e->d_name)) continue;
            snprintf(names + count * 64, 64, "%.63s", e->d_name);
            count++;
        }
        closedir(d);
    }
    for (int i = 0; i < count; i++) {
        char child[200];
        snprintf(child, sizeof(child), "%s/%s", path, names + i * 64);
        rm_recursive(child, depth + 1);
    }
    free(names);
    rmdir(path);
}

// The session ends on its own if the app goes away without saying goodbye.
#define SESSION_IDLE_US (30 * 1000000LL)
static volatile int64_t s_last_frame_us = -SESSION_IDLE_US;

bool link_session_active(void) { return esp_timer_get_time() - s_last_frame_us < SESSION_IDLE_US; }

static uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void send(uint8_t seq, uint8_t cmd, const void *payload, size_t len)
{
    uint8_t head[6] = {0xA5, 0x5A, (uint8_t)(len), (uint8_t)(len >> 8), seq, cmd};
    uint16_t crc = crc16(head + 4, 2, 0xFFFF);
    crc = crc16((const uint8_t *)payload, len, crc);
    const uint8_t tail[2] = {(uint8_t)(crc >> 8), (uint8_t)(crc)};
    // One frame can be bigger than the driver's buffer, so keep writing until it's all out.
    usb_serial_jtag_write_bytes(head, sizeof(head), pdMS_TO_TICKS(500));
    for (size_t off = 0; off < len;) {
        // Never offer more than the driver's buffer at once: a bigger ask can come back
        // short or time out, and a truncated frame just fails its CRC at the far end.
        size_t take = len - off;
        if (take > LINK_MAX_PAYLOAD) take = LINK_MAX_PAYLOAD;
        const int n = usb_serial_jtag_write_bytes((const uint8_t *)payload + off, take, pdMS_TO_TICKS(1000));
        if (n <= 0) return;
        off += n;
    }
    usb_serial_jtag_write_bytes(tail, sizeof(tail), pdMS_TO_TICKS(500));
}

static void fail(uint8_t seq, const char *why) { send(seq, LINK_ERR, why, strlen(why)); }

static void handle(uint8_t seq, uint8_t cmd, const uint8_t *body, uint16_t len)
{
    s_last_frame_us = esp_timer_get_time();
    switch (cmd) {
    case LINK_HELLO: {
        // proto, api major/minor, then two NUL-terminated strings: firmware version, board.
        uint8_t out[96];
        const esp_app_desc_t *app = esp_app_get_description();
        size_t n = 0;
        out[n++] = LINK_PROTO_VERSION, out[n++] = 0;
        out[n++] = 1, out[n++] = 0;   // game API major
        out[n++] = 0, out[n++] = 0;   // game API minor
        n += snprintf((char *)out + n, sizeof(out) - n, "%s", app->version) + 1;
        n += snprintf((char *)out + n, sizeof(out) - n, "%s", "waveshare-amoled-175c") + 1;
        send(seq, cmd | 0x80, out, n);
        ESP_LOGI(TAG, "app connected");
        break;
    }
    case LINK_INFO: {
        uint32_t out[2] = {0, 0};
        uint8_t count = 0;
        if (s_info_hook) s_info_hook(&out[0], &out[1], &count);
        uint8_t buf[9];
        memcpy(buf, out, 8);
        buf[8] = count;
        send(seq, cmd | 0x80, buf, sizeof(buf));
        break;
    }
    case LINK_LIST: {
        link_game_t *games = (link_game_t *)s_scratch;
        const int n = s_list_hook ? s_list_hook(games, 24) : 0;
        // The count byte goes in front of the entries, so shift them up once.
        memmove(s_scratch + 1, games, n * sizeof(link_game_t));
        s_scratch[0] = (uint8_t)(n);
        send(seq, cmd | 0x80, s_scratch, 1 + n * sizeof(link_game_t));
        break;
    }
    case LINK_ICON: {
        char id[17] = {0};
        memcpy(id, body, len < 16 ? len : 16);
        const uint8_t *png = NULL;
        size_t png_len = 0;
        if (s_icon_hook && s_icon_hook(id, &png, &png_len)) send(seq, cmd | 0x80, png, png_len);
        else fail(seq, "no icon for that game");
        break;
    }
    case LINK_FS_FREE: {
        uint32_t out[2] = {0, 0};
        if (s_fs_root) {
            storage_free_bytes(&out[0], &out[1]);
        }
        send(seq, cmd | 0x80, out, sizeof(out));
        break;
    }
    case LINK_FS_LIST: {
        char path[200];
        if (!safe_path((const char *)body, len, path, sizeof(path))) {
            fail(seq, "bad path");
            break;
        }
        DIR *d = opendir(path);
        if (!d) {
            fail(seq, "no such folder");
            break;
        }
        uint8_t *out = s_scratch;
        size_t n = 0;
        const struct dirent *e;
        while ((e = readdir(d)) && n + 300 < LINK_MAX_PAYLOAD) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !real_entry(e->d_name)) continue;
            char full[320];
            snprintf(full, sizeof(full), "%s/%.*s", path, 100, e->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            out[n++] = S_ISDIR(st.st_mode) ? 1 : 0;
            const uint32_t size = (uint32_t)st.st_size;
            memcpy(out + n, &size, 4);
            n += 4;
            n += snprintf((char *)out + n, LINK_MAX_PAYLOAD - n, "%.100s", e->d_name) + 1;
        }
        closedir(d);
        send(seq, cmd | 0x80, out, n);
        break;
    }
    case LINK_FS_PUT: {
        put_abort();
        if (len < 9) {
            fail(seq, "bad request");
            break;
        }
        memcpy(&s_put_left, body, 4);
        memcpy(&s_put_want, body + 4, 4);
        if (!safe_path((const char *)body + 8, len - 8, s_put_path, sizeof(s_put_path))) {
            fail(seq, "bad path");
            break;
        }
        s_put = fopen(s_put_path, "wb");
        if (!s_put) {
            fail(seq, "can't write there (is the folder missing?)");
            break;
        }
        // Batch the writes. Wear-levelled FAT turns every small write into a
        // read-modify-write of a whole sector, and that - not USB - is what limits a
        // transfer: with 4 KB writes a theme crawled in at 8 KB/s.
        if (!s_put_buf) s_put_buf = heap_caps_malloc(PUT_BUF, MALLOC_CAP_SPIRAM);
        if (s_put_buf) setvbuf(s_put, (char *)s_put_buf, _IOFBF, PUT_BUF);
        s_put_crc = 0;
        ESP_LOGI(TAG, "receiving %s, %u bytes", s_put_path, (unsigned)s_put_left);
        send(seq, cmd | 0x80, NULL, 0);
        break;
    }
    case LINK_FS_DATA: {
        if (!s_put || len > s_put_left) {
            put_abort();
            fail(seq, "not expecting that");
            break;
        }
        if (fwrite(body, 1, len, s_put) != len) {
            put_abort();
            fail(seq, "the storage is full");
            break;
        }
        s_put_crc = esp_rom_crc32_le(s_put_crc, body, len);
        s_put_left -= len;
        send(seq, cmd | 0x80, NULL, 0);
        break;
    }
    case LINK_FS_END: {
        if (!s_put) {
            fail(seq, "nothing being sent");
            break;
        }
        const bool ok = s_put_left == 0 && s_put_crc == s_put_want;
        fclose(s_put);
        s_put = NULL;
        if (!ok) {
            unlink(s_put_path);
            fail(seq, "the file arrived damaged");
            break;
        }
        ESP_LOGI(TAG, "stored %s", s_put_path);
        note_changed();
        send(seq, cmd | 0x80, NULL, 0);
        break;
    }
    case LINK_FS_GET: {
        char path[200];
        uint32_t offset = 0;
        if (len < 5) {
            fail(seq, "bad request");
            break;
        }
        memcpy(&offset, body, 4);
        if (!safe_path((const char *)body + 4, len - 4, path, sizeof(path))) {
            fail(seq, "bad path");
            break;
        }
        FILE *f = fopen(path, "rb");
        if (!f) {
            fail(seq, "no such file");
            break;
        }
        uint8_t *out = s_scratch;
        fseek(f, (long)offset, SEEK_SET);
        const size_t n = fread(out, 1, LINK_MAX_PAYLOAD, f);
        fclose(f);
        send(seq, cmd | 0x80, out, n);
        break;
    }
    case LINK_FS_FORMAT: {
        // Wipes the whole storage area. Only ever on purpose: the app has to spell it out,
        // and the watch restarts afterwards so everything it seeds is written fresh.
        static const char kPhrase[] = "ERASE EVERYTHING";
        if (len != sizeof(kPhrase) - 1 || memcmp(body, kPhrase, len) != 0) {
            fail(seq, "that isn't how you ask to erase the storage");
            break;
        }
        put_abort();
        ESP_LOGW(TAG, "formatting the storage area on request");
        send(seq, cmd | 0x80, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(200));   // let the reply reach the app before the port drops
        storage_format();
        esp_restart();
        break;
    }
    case LINK_FS_DELETE: {
        char path[200];
        if (!safe_path((const char *)body, len, path, sizeof(path))) {
            fail(seq, "bad path");
            break;
        }
        rm_recursive(path, 0);
        note_changed();
        send(seq, cmd | 0x80, NULL, 0);
        break;
    }
    case LINK_FS_MKDIR: {
        char path[200];
        if (!safe_path((const char *)body, len, path, sizeof(path))) {
            fail(seq, "bad path");
            break;
        }
        mkdir(path, 0775);
        note_changed();
        send(seq, cmd | 0x80, NULL, 0);
        break;
    }
    default: fail(seq, "this watch doesn\'t know that command"); break;
    }
}

static void link_task(void *arg)
{
    static uint8_t body[LINK_MAX_PAYLOAD];
    static uint8_t in[512];
    enum { SYNC0, SYNC1, HEAD, BODY, CRC } state = SYNC0;
    uint8_t head[4];   // len lo, len hi, seq, cmd
    uint16_t want = 0, got = 0;
    uint8_t crc_in[2];
    int have = 0, pos = 0;

    for (;;) {
        if (pos >= have) {
            // A frame is up to 4 KB. Asking the driver for one byte at a time meant four
            // thousand calls per frame and cost most of the transfer rate; read in blocks.
            have = usb_serial_jtag_read_bytes(in, sizeof(in), pdMS_TO_TICKS(200));
            pos = 0;
            if (have <= 0) {
                have = 0;
                if (state != SYNC0 && esp_timer_get_time() - s_last_frame_us > 2000000LL) state = SYNC0;
                continue;
            }
        }

        // The body is the bulk of a frame and needs no inspection: take it in one go.
        if (state == BODY) {
            int n = have - pos;
            if (n > want - got) n = want - got;
            memcpy(body + got, in + pos, n);
            got += n;
            pos += n;
            if (got == want) {
                got = 0;
                state = CRC;
            }
            continue;
        }

        const uint8_t b = in[pos++];
        switch (state) {
        case SYNC0: state = (b == 0xA5) ? SYNC1 : SYNC0; break;
        case SYNC1:
            state = (b == 0x5A) ? HEAD : (b == 0xA5 ? SYNC1 : SYNC0);
            got = 0;
            break;
        case HEAD:
            head[got++] = b;
            if (got == 4) {
                want = (uint16_t)(head[0] | (head[1] << 8));
                got = 0;
                if (want > LINK_MAX_PAYLOAD) {
                    state = SYNC0;   // not ours, or corrupt: go back to hunting for a sync word
                    break;
                }
                state = want ? BODY : CRC;
            }
            break;
        case CRC:
            crc_in[got++] = b;
            if (got == 2) {
                uint16_t crc = crc16(head + 2, 2, 0xFFFF);
                crc = crc16(body, want, crc);
                if (crc == (uint16_t)((crc_in[0] << 8) | crc_in[1])) handle(head[2], head[3], body, want);
                state = SYNC0;
                got = 0;
            }
            break;
        default: break;   // BODY is handled above
        }
    }
}

void link_set_info_hook(void (*fn)(uint32_t *, uint32_t *, uint8_t *)) { s_info_hook = fn; }
void link_set_list_hook(int (*fn)(link_game_t *, int)) { s_list_hook = fn; }
void link_set_icon_hook(bool (*fn)(const char *, const uint8_t **, size_t *)) { s_icon_hook = fn; }

void link_start(void)
{
    // The console already writes to this port through the VFS; installing the driver and
    // pointing the VFS at it lets us read as well, without the two fighting over the
    // peripheral. Logging carries on as before.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    // Room for a whole frame each way. With a small buffer the host can only push a
    // fraction of a frame before it has to stop and wait for the watch to drain it, and
    // a transfer runs at a few KB/s - the same lesson the USB drive taught, where an
    // 8 KB buffer beat the 512 byte default four times over.
    cfg.rx_buffer_size = LINK_MAX_PAYLOAD + 512;
    cfg.tx_buffer_size = LINK_MAX_PAYLOAD + 512;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "can't open the USB port for the manager app");
        return;
    }
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(link_task, "link", 5120, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "USB link ready");
}
