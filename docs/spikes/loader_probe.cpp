// Spike for docs/GAME_API.md section 6: can this chip run code that wasn't in the
// firmware image? Everything else about loadable games is ordinary work; this is the one
// thing that decides whether the design is possible at all.
//
// It copies a tiny function into each kind of memory a loaded game could live in, then
// calls it and checks the answer. Delete this file once the loader is real.
#include "loader_probe.h"

#include <cstring>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "esp_partition.h"

static const char *TAG = "probe";

// A whole function, assembled into rodata so nothing in it depends on where it sits:
// take a stack frame, return 42, done. No calls, no constant pool, no absolute addresses,
// which is what makes it safe to copy somewhere else and jump to.
extern "C" const uint8_t probe_code_start[], probe_code_end[];
__asm__(".section .rodata.probe_code,\"a\"\n"
        ".align 4\n"
        ".global probe_code_start\n"
        "probe_code_start:\n"
        "  entry a1, 32\n"
        "  movi.n a2, 42\n"
        "  retw.n\n"
        ".align 4\n"
        ".global probe_code_end\n"
        "probe_code_end:\n"
        ".previous\n");

using probe_fn = int (*)(void);

static bool run_from(void *dst, const char *what)
{
    const size_t n = size_t(probe_code_end - probe_code_start);
    std::memcpy(dst, probe_code_start, n);
    // Code written as data has to reach memory before the instruction side reads it.
    esp_cache_msync(dst, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    const int got = reinterpret_cast<probe_fn>(dst)();
    ESP_LOGW(TAG, "  %-26s %p -> %d %s", what, dst, got, got == 42 ? "RUNS" : "WRONG ANSWER");
    return got == 42;
}

void loader_probe(void)
{
    const size_t n = size_t(probe_code_end - probe_code_start);
    ESP_LOGW(TAG, "can this chip run code it wasn't built with? (%u byte test function)", unsigned(n));

    // 1. Internal RAM marked executable. Always works, but there is only ~60 KB of it and
    //    Wi-Fi and the display want that, so it can't be where games live.
    if (void *p = heap_caps_malloc(256, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT)) {
        run_from(p, "internal RAM (IRAM)");
        heap_caps_free(p);
    } else {
        ESP_LOGW(TAG, "  internal RAM (IRAM)        no executable heap");
    }

    // 2. PSRAM mapped into the instruction bus. This is the one that matters: 8 MB, and a
    //    game could be relocated straight into it at launch. Write it through the ordinary
    //    data pointer, then call the same physical memory through its executable mapping.
    size_t biggest = 0;
    esp_err_t err = esp_mmu_map_get_max_consecutive_free_block_size(MMU_MEM_CAP_EXEC, MMU_TARGET_PSRAM0, &biggest);
    ESP_LOGW(TAG, "  PSRAM executable window    %s, %u KB free", esp_err_to_name(err), unsigned(biggest / 1024));

    // One MMU page, aligned, so the mapping covers exactly what we wrote.
    const size_t kPage = 64 * 1024;
    if (void *ram = heap_caps_aligned_alloc(kPage, kPage, MALLOC_CAP_SPIRAM)) {
        esp_paddr_t paddr = 0;
        mmu_target_t target = MMU_TARGET_PSRAM0;
        if (esp_mmu_vaddr_to_paddr(ram, &paddr, &target) == ESP_OK) {
            std::memcpy(ram, probe_code_start, n);
            esp_cache_msync(ram, kPage, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            void *exec = nullptr;
            err = esp_mmu_map(paddr, kPage, MMU_TARGET_PSRAM0, MMU_MEM_CAP_EXEC, 0, &exec);
            if (err == ESP_OK) {
                const int got = reinterpret_cast<probe_fn>(exec)();
                ESP_LOGW(TAG, "  PSRAM code executed        %p -> %d %s", exec, got,
                         got == 42 ? "RUNS" : "WRONG ANSWER");
                esp_mmu_unmap(exec);
            } else {
                ESP_LOGW(TAG, "  PSRAM code executed        map failed: %s", esp_err_to_name(err));
            }
        }
        heap_caps_free(ram);
    }

    // 3. Flash mapped executable - how the firmware itself runs. A game could be fixed up
    //    at install time and then run in place, costing no RAM at all.
    err = esp_mmu_map_get_max_consecutive_free_block_size(MMU_MEM_CAP_EXEC, MMU_TARGET_FLASH0, &biggest);
    ESP_LOGW(TAG, "  flash executable window    %s, %u KB free", esp_err_to_name(err), unsigned(biggest / 1024));

    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    if (part && part->address < 0x1000000) {
        void *mapped = nullptr;
        err = esp_mmu_map(part->address, 64 * 1024, MMU_TARGET_FLASH0, MMU_MEM_CAP_EXEC, 0, &mapped);
        ESP_LOGW(TAG, "  flash mapped executable    %s at %p", esp_err_to_name(err), mapped);
        if (err == ESP_OK) esp_mmu_unmap(mapped);
    }
}
