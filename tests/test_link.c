// The USB link's two pieces of pure logic:
//
//   safe_path  decides what a computer is allowed to write to. If it is wrong, anything
//              plugged into the watch can reach outside the storage folder.
//   crc16      decides whether a frame is believed. If it disagrees with the clients,
//              transfers fail in ways that look like flaky hardware.
//
// Both are static inside link.c, so the file is compiled in whole against the stubs in
// tests/stubs rather than linked against the firmware.
#include "harness.h"
#include "stubs/compat.h"

#include "../components/link/link.c"

// link.c reaches into the rest of the firmware for a few things. None of them are part
// of what is being tested here, so they only need to exist.
const char *net_device_key(void) { return "0000000000000000"; }
bool storage_free_bytes(uint32_t *total, uint32_t *free_bytes) { *total = *free_bytes = 0; return true; }
esp_err_t storage_format(void) { return ESP_OK; }

static void test_crc16(void)
{
    // Vectors for CRC-16/CCITT-FALSE, which is what both clients implement.
    CHECK_EQ(crc16((const uint8_t *)"", 0, 0xFFFF), 0xFFFF);
    CHECK_EQ(crc16((const uint8_t *)"A", 1, 0xFFFF), 0xB915);
    CHECK_EQ(crc16((const uint8_t *)"123456789", 9, 0xFFFF), 0x29B1);

    // Feeding it in two goes must match one go: that is how a frame's header and body
    // are checked, and it is where an off-by-one would hide.
    const uint8_t all[] = {1, 2, 3, 4, 5, 6};
    CHECK_EQ(crc16(all + 2, 4, crc16(all, 2, 0xFFFF)), crc16(all, 6, 0xFFFF));
}

// Lengths come from strlen, so a miscounted literal can never fail a test the code would
// have passed - which is exactly what happened the first time this was written.
#define SAFE(p) safe_path((p), (uint16_t)strlen(p), out, sizeof(out))

static void test_safe_path(void)
{
    char out[200];
    link_set_fs_root("/data");

    // Ordinary paths resolve under the root.
    CHECK(SAFE("theme.json"));
    CHECK_STR(out, "/data/theme.json");
    CHECK(SAFE("Theme/CPU/background.png"));
    CHECK_STR(out, "/data/Theme/CPU/background.png");

    // No payload means the root itself, which is how a listing starts.
    CHECK(SAFE(""));
    CHECK_STR(out, "/data");

    // Escapes, in every shape that has to be refused.
    CHECK(!SAFE(".."));
    CHECK(!SAFE("../secrets"));
    CHECK(!SAFE("Theme/../../etc"));
    CHECK(!SAFE("a/../b"));
    CHECK(!SAFE("/etc/passwd"));          // absolute
    CHECK(!SAFE("Theme\\..\\x"));         // backslash
    CHECK(!SAFE("a\nb"));                  // control character
    CHECK(!safe_path("a\0b", 3, out, sizeof(out)));                  // embedded NUL

    // A single dot is a legitimate character: ".firmware" is a real file the watch writes.
    CHECK(SAFE(".firmware"));
    CHECK_STR(out, "/data/.firmware");

    // Absurd lengths are refused rather than truncated into something else.
    char very_long[200];
    memset(very_long, 'a', sizeof(very_long));
    CHECK(!safe_path(very_long, 130, out, sizeof(out)));

    // With no root configured nothing is allowed, so a misconfigured build writes nowhere.
    link_set_fs_root(0);
    CHECK(!SAFE("theme.json"));
    link_set_fs_root("/data");
}

static void test_real_entry(void)
{
    // The drive carries phantom 0xFF directory entries from an old corruption. Walking
    // into one of those while deleting is what crashed the watch once.
    CHECK(real_entry("Default"));
    CHECK(real_entry(".firmware"));
    CHECK(!real_entry(""));
    CHECK(!real_entry("\xff\xff\xff"));
    CHECK(!real_entry("bad\x01name"));
}

int main(void)
{
    test_crc16();
    test_safe_path();
    test_real_entry();
    return failures("link");
}
