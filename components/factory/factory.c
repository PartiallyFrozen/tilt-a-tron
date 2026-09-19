#include "factory/factory.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "loader/loader.h"
#include "nvs.h"
#include "storage/storage.h"

static const char *TAG = "factory";

#define FACTORY(id) extern const uint8_t _binary_##id##_tat_start[], _binary_##id##_tat_end[];
FACTORY(breakout) FACTORY(maze) FACTORY(racer) FACTORY(jump) FACTORY(tiltatris)
FACTORY(star) FACTORY(pindrop) FACTORY(starfall) FACTORY(skatergirlz) FACTORY(echo)
#undef FACTORY

// In the order they appear on a new watch's home screen.
#define ENTRY(id) {_binary_##id##_tat_start, _binary_##id##_tat_end}
static const struct {
    const uint8_t *start, *end;
} s_games[] = {
    ENTRY(breakout), ENTRY(maze),    ENTRY(racer),    ENTRY(jump),        ENTRY(tiltatris),
    ENTRY(star),     ENTRY(pindrop), ENTRY(starfall), ENTRY(skatergirlz), ENTRY(echo),
};
#define N_GAMES ((int)(sizeof(s_games) / sizeof(s_games[0])))

static bool inspect(int i, loader_entry_t *out)
{
    return loader_inspect(s_games[i].start, (size_t)(s_games[i].end - s_games[i].start), out);
}

// Written beside its real name and moved into place, so losing power half way through
// leaves no file rather than half of one. A ".part" is not a ".tat" and is never scanned.
static bool write_package(const loader_entry_t *game, int i)
{
    char path[128], part[136];
    snprintf(path, sizeof(path), "%s/%s.tat", loader_games_dir(), game->id);
    snprintf(part, sizeof(part), "%s.part", path);

    FILE *f = fopen(part, "wb");
    if (!f) {
        ESP_LOGE(TAG, "cannot write %s", part);
        return false;
    }
    const size_t len = (size_t)(s_games[i].end - s_games[i].start);
    const bool ok = fwrite(s_games[i].start, 1, len, f) == len;
    if (fclose(f) != 0 || !ok) {
        remove(part);
        ESP_LOGE(TAG, "writing %s failed - is storage full?", game->name);
        return false;
    }
    remove(path);
    if (rename(part, path) != 0) {
        remove(part);
        return false;
    }
    ESP_LOGI(TAG, "installed %s v%u", game->name, game->version);
    return true;
}

static const loader_entry_t *find(const loader_entry_t *list, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(list[i].id, id) == 0) return &list[i];
    return NULL;
}

// everything: put back whatever is missing, whether or not it was seeded before.
static int install(bool everything, factory_progress_fn progress)
{
    if (!storage_ready()) return 0;
    mkdir(loader_games_dir(), 0775);   // fine if it is already there

    loader_entry_t *have = heap_caps_malloc(sizeof(loader_entry_t) * LOADER_MAX_GAMES, MALLOC_CAP_SPIRAM);
    if (!have) return 0;
    const int n_have = loader_scan(have, LOADER_MAX_GAMES);
    // An empty folder is a new watch or a wiped drive, and either way the answer is the
    // same. It is also somebody who removed every game on purpose, who will be less put
    // out by getting them back than everyone else would be by a watch with nothing on it.
    if (n_have == 0) everything = true;

    nvs_handle_t nvs = 0;
    const bool remember = nvs_open("factory", NVS_READWRITE, &nvs) == ESP_OK;

    int written = 0;
    for (int i = 0; i < N_GAMES; i++) {
        loader_entry_t game;
        if (!inspect(i, &game)) {
            ESP_LOGE(TAG, "factory game %d is not a sound package", i);
            continue;
        }
        // What is remembered is which exact build this watch was last given, not just its
        // version: a firmware can carry a rebuilt game under the same number - a compiler
        // flag changed, say - and that should still arrive, once. (An earlier firmware
        // kept a 16-bit version under the same key; that counts as "seen", nothing more.)
        uint32_t given = 0;
        uint16_t old_style = 0;
        const bool have_stamp = remember && nvs_get_u32(nvs, game.id, &given) == ESP_OK;
        const bool seen_before = have_stamp || (remember && nvs_get_u16(nvs, game.id, &old_style) == ESP_OK);
        const bool firmware_copy_is_new = !have_stamp || given != game.stamp;
        const loader_entry_t *installed = find(have, n_have, game.id);

        bool want;
        if (!installed) {
            want = everything || !seen_before;   // new to this watch, or asked for back
        } else {
            // Bring it up to date - but only when the firmware's copy is one this watch
            // has not been given before, and never over something newer. After that, a
            // different build under this id is somebody's own work and is left alone.
            want = installed->stamp != game.stamp && firmware_copy_is_new && installed->version <= game.version;
        }
        if (want && progress) progress(game.name, i, N_GAMES);
        if (want && write_package(&game, i)) written++;
        if (remember && firmware_copy_is_new) {
            nvs_erase_key(nvs, game.id);   // the old 16-bit entry cannot be overwritten in place
            nvs_set_u32(nvs, game.id, game.stamp);
        }
    }
    if (remember) {
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    heap_caps_free(have);
    if (written) storage_changed();
    return written;
}

int factory_seed(factory_progress_fn progress) { return install(false, progress); }
int factory_restore(factory_progress_fn progress) { return install(true, progress); }

int factory_order(const char *id)
{
    for (int i = 0; i < N_GAMES; i++) {
        loader_entry_t game;
        if (inspect(i, &game) && strcmp(game.id, id) == 0) return i;
    }
    return -1;
}

bool factory_is(uint32_t stamp)
{
    for (int i = 0; i < N_GAMES; i++) {
        loader_entry_t game;
        if (inspect(i, &game) && game.stamp == stamp) return true;
    }
    return false;
}
