// wc::Store decides whether a saved high score survives a firmware update. The games
// used to write these as u8 and u16; the store writes i32 and reads all three. Get that
// wrong and every score and setting silently resets - which is exactly the kind of bug
// nobody notices until it has already happened to somebody.
extern "C" {
#include "harness.h"
}

#include "../components/engine/store.cpp"

// The fake NVS's storage (declared in tests/stubs/nvs.h).
fake_entry_t fake_nvs[16];

static void reset() { memset(fake_nvs, 0, sizeof(fake_nvs)); }

static void put(const char *key, int kind, int32_t value)
{
    fake_entry_t *e = fake_slot(key);
    e->kind = kind;
    e->value = value;
}

static void test_missing_keeps_the_default()
{
    reset();
    wc::Store s("game");
    int v = 7;
    s.get("nothing", v);
    CHECK_EQ(v, 7);   // the caller's initialiser is the default; nothing overwrites it
}

static void test_reads_what_it_wrote()
{
    reset();
    {
        wc::Store w("game", wc::Store::Write);
        w.set("best", 2101);
    }
    wc::Store r("game");
    int v = 0;
    r.get("best", v);
    CHECK_EQ(v, 2101);
}

static void test_reads_older_widths()
{
    // A score written by firmware from before the store existed.
    reset();
    put("best", FAKE_U8, 42);
    int v = 0;
    wc::Store("game").get("best", v);
    CHECK_EQ(v, 42);

    reset();
    put("depth", FAKE_U16, 9000);
    v = 0;
    wc::Store("game").get("depth", v);
    CHECK_EQ(v, 9000);
}

static void test_writing_over_an_older_width()
{
    // NVS refuses to change a key's width, so set() has to clear it first. Without that,
    // every save after an update would quietly fail.
    reset();
    put("best", FAKE_U8, 42);
    {
        wc::Store w("game", wc::Store::Write);
        w.set("best", 5000);
    }
    int v = 0;
    wc::Store("game").get("best", v);
    CHECK_EQ(v, 5000);
    CHECK_EQ(fake_find("best")->kind, FAKE_I32);
}

static void test_limit_rejects_nonsense()
{
    // A value that indexes a table must stay inside it, or the game reads off the end.
    reset();
    put("face", FAKE_I32, 99);
    int face = 1;
    wc::Store("game").get("face", face, 5);
    CHECK_EQ(face, 1);   // 99 is out of range: keep the default

    reset();
    put("face", FAKE_I32, 3);
    face = 1;
    wc::Store("game").get("face", face, 5);
    CHECK_EQ(face, 3);

    reset();
    put("face", FAKE_I32, -1);
    face = 1;
    wc::Store("game").get("face", face, 5);
    CHECK_EQ(face, 1);   // negatives too
}

static void test_bools()
{
    reset();
    {
        wc::Store w("game", wc::Store::Write);
        w.set("mirror", true);
    }
    bool mirror = false;
    wc::Store("game").get("mirror", mirror);
    CHECK(mirror);

    // A bool that was stored as a u8 by the old code still reads.
    reset();
    put("flat", FAKE_U8, 1);
    bool flat = false;
    wc::Store("game").get("flat", flat);
    CHECK(flat);
}

static void test_reads_do_not_write()
{
    reset();
    wc::Store r("game");            // opened for reading
    r.set("best", 1);               // ignored
    CHECK(fake_find("best") == 0);
}

int main()
{
    test_missing_keeps_the_default();
    test_reads_what_it_wrote();
    test_reads_older_widths();
    test_writing_over_an_older_width();
    test_limit_rejects_nonsense();
    test_bools();
    test_reads_do_not_write();
    return failures("store");
}
