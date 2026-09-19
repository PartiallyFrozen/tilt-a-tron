#include "loader/loader.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mmu_map.h"
#include "esp_rom_crc.h"
#include "exports.h"
#include "storage/storage.h"

static const char *TAG = "loader";

#define GAMES_DIR STORAGE_ROOT "/Games"

// ---------------------------------------------------------------- the package

#define TAT_MAGIC "TATPKG\0\x01"
#define KIND_GAME 1

#define SEC_CODE 0x45444F43u   /* 'CODE' little-endian */
#define SEC_ICON 0x4E4F4349u
#define SEC_ASSET 0x54535341u  /* 'ASST' */
#define SEC_SHOT 0x544F4853u

typedef struct __attribute__((packed)) {
    char magic[8];
    uint32_t header_crc32;   /* over everything after this field */
    uint32_t total_size;
    uint8_t kind, reserved;
    uint16_t api_major, api_minor, version;
    char id[16], name[24], author[24];
    uint8_t accent_r, accent_g, accent_b, pad;
    uint16_t section_count;
    uint16_t pad2;
} tat_header_t;

typedef struct __attribute__((packed)) {
    uint32_t type, offset, size, crc32;
} tat_section_t;

// Read a file whole into PSRAM. Packages are tens of kilobytes; the alternative is seeking
// around a FAT filesystem for every section and check, which is slower and far fussier.
static uint8_t *read_all(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = heap_caps_malloc((size_t)len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    const size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        heap_caps_free(buf);
        return NULL;
    }
    *len_out = got;
    return buf;
}

// Check a package before a byte of it is trusted: the magic, the size, the header's own
// CRC, then every section's. A file that arrived half-sent or was edited by hand is
// rejected here rather than crashing the console later.
static bool check_package(const uint8_t *buf, size_t len, const tat_header_t **head_out,
                          const tat_section_t **secs_out)
{
    if (len < sizeof(tat_header_t)) return false;
    const tat_header_t *h = (const tat_header_t *)buf;
    if (memcmp(h->magic, TAT_MAGIC, 8) != 0) return false;
    if (h->total_size != len) return false;
    if (h->section_count == 0 || h->section_count > 64) return false;

    const size_t table = sizeof(tat_header_t) + (size_t)h->section_count * sizeof(tat_section_t);
    if (len < table) return false;

    // The header CRC covers everything after the field itself, table included.
    const size_t after = offsetof(tat_header_t, total_size);
    const uint32_t want = esp_rom_crc32_le(0, buf + after, table - after);
    if (want != h->header_crc32) {
        ESP_LOGE(TAG, "header CRC: got %08lx want %08lx", (unsigned long)h->header_crc32,
                 (unsigned long)want);
        return false;
    }

    const tat_section_t *secs = (const tat_section_t *)(buf + sizeof(tat_header_t));
    for (int i = 0; i < h->section_count; i++) {
        if (secs[i].offset < table || secs[i].offset + secs[i].size > len) return false;
        if (esp_rom_crc32_le(0, buf + secs[i].offset, secs[i].size) != secs[i].crc32) {
            ESP_LOGE(TAG, "section %d (%.4s) is corrupt", i, (const char *)&secs[i].type);
            return false;
        }
    }
    *head_out = h;
    *secs_out = secs;
    return true;
}

const char *loader_games_dir(void) { return GAMES_DIR; }

int loader_scan(loader_entry_t *out, int max)
{
    DIR *d = opendir(GAMES_DIR);
    if (!d) return 0;   /* nothing installed yet is not an error */

    int n = 0;
    struct dirent *e;
    while (n < max && (e = readdir(d)) != NULL) {
        const size_t nl = strlen(e->d_name);
        if (nl < 5 || strcasecmp(e->d_name + nl - 4, ".tat") != 0) continue;

        char path[sizeof(out[0].path)];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", GAMES_DIR, e->d_name) >= sizeof(path)) continue;

        size_t len = 0;
        uint8_t *buf = read_all(path, &len);
        if (!buf) continue;
        const tat_header_t *h;
        const tat_section_t *secs;
        if (check_package(buf, len, &h, &secs) && h->kind == KIND_GAME) {
            // Two files claiming the same game is a real state to end up in - installing
            // under one name over a copy that arrived under another. The id is what saves
            // and the carousel are keyed on, so the second one is left alone rather than
            // shown twice and given the first one's scores.
            bool seen = false;
            for (int k = 0; k < n && !seen; k++) seen = strcmp(out[k].id, h->id) == 0;
            if (seen) {
                ESP_LOGW(TAG, "%s is already installed; ignoring %s", h->id, e->d_name);
                heap_caps_free(buf);
                continue;
            }
            loader_entry_t *o = &out[n++];
            memset(o, 0, sizeof(*o));
            snprintf(o->path, sizeof(o->path), "%s", path);
            snprintf(o->id, sizeof(o->id), "%.15s", h->id);
            snprintf(o->name, sizeof(o->name), "%.23s", h->name);
            snprintf(o->author, sizeof(o->author), "%.23s", h->author);
            o->version = h->version;
            o->api_major = h->api_major;
            o->api_minor = h->api_minor;
            o->accent_r = h->accent_r, o->accent_g = h->accent_g, o->accent_b = h->accent_b;
            o->size = (uint32_t)len;
            // The launcher shows an unrunnable game rather than hiding it, so that someone
            // whose watch is too old is told why instead of finding the icon simply gone.
            o->runnable = h->api_major == TAT_API_MAJOR && h->api_minor <= TAT_API_MINOR;
            ESP_LOGI(TAG, "found %s v%u by %s (%u bytes)%s", o->name, o->version, o->author,
                     (unsigned)o->size, o->runnable ? "" : " - needs a different firmware");
        } else {
            ESP_LOGW(TAG, "%s is not a usable package", e->d_name);
        }
        heap_caps_free(buf);
    }
    closedir(d);
    return n;
}

// ---------------------------------------------------------------- the ELF

typedef struct __attribute__((packed)) {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} Elf32_Ehdr;

typedef struct __attribute__((packed)) {
    uint32_t name, type, flags, addr, offset, size;
    uint32_t link, info, addralign, entsize;
} Elf32_Shdr;

typedef struct __attribute__((packed)) {
    uint32_t name, value, size;
    uint8_t info, other;
    uint16_t shndx;
} Elf32_Sym;

typedef struct __attribute__((packed)) {
    uint32_t offset, info;
    int32_t addend;
} Elf32_Rela;

#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHN_UNDEF 0
#define ET_EXEC 2
#define EM_XTENSA 94

#define R_XTENSA_32 1
#define R_XTENSA_ASM_EXPAND 11
#define R_XTENSA_SLOT0_OP 20

struct loader_game {
    void *ram;          // the writable view: where the image was built
    void *exec;         // the executable view of the same physical memory
    size_t span;        // how much was mapped, rounded to MMU pages
    const tat_game_t *desc;
    tat_asset_t *assets;
    int asset_count;
    uint8_t *file;      // the package, kept because assets point into it
};

// Every section the linker marked as belonging in memory. They were laid out contiguously
// from zero by the package's linker script, so a section's addr IS its offset in the image.
static size_t image_span(const Elf32_Shdr *sh, int n)
{
    size_t span = 0;
    for (int i = 0; i < n; i++) {
        if (!(sh[i].flags & SHF_ALLOC)) continue;
        const size_t end = sh[i].addr + sh[i].size;
        if (end > span) span = end;
    }
    return span;
}

// `assets` is the table this package's own files live in. A game that ships files declares
// `extern const tat_asset_t tat_assets[]` and points its descriptor at it: built in, the
// firmware build supplies that symbol, and from a package the loader does. It is the one
// name a game may import that the console does not export.
// The image lives at two addresses at once. The same physical memory is mapped normally,
// where it can be read and written a byte at a time, and again on the instruction bus,
// where the processor will execute it - and the instruction bus only does aligned 32-bit
// loads. So a pointer to a function has to carry the executable address and a pointer to
// a string the ordinary one, and which it is depends on the section the symbol lives in.
//
// Getting this wrong is not subtle: point a string at the executable view and the first
// thing that prints it takes a LoadStoreError.
//
// Literal pools are the exception that needs no thought. They sit inside .text and the
// processor reads them with l32r, which is a 32-bit aligned load, so they are fine there.
static uintptr_t base_for(const Elf32_Shdr *sh, uint16_t shndx, uintptr_t data, uintptr_t exec)
{
    return (sh[shndx].flags & SHF_EXECINSTR) ? exec : data;
}

static bool apply_relocations(const uint8_t *elf, const Elf32_Shdr *sh, int shnum,
                              uint8_t *ram, size_t span, uintptr_t data_base, uintptr_t exec_base,
                              const tat_asset_t *assets)
{
    for (int i = 0; i < shnum; i++) {
        if (sh[i].type != SHT_RELA) continue;
        // sh_info names the section being patched; sh_link the symbol table to read.
        const Elf32_Shdr *target = &sh[sh[i].info];
        if (!(target->flags & SHF_ALLOC)) continue;
        const Elf32_Shdr *symtab = &sh[sh[i].link];
        const Elf32_Sym *syms = (const Elf32_Sym *)(elf + symtab->offset);
        const char *strs = (const char *)(elf + sh[symtab->link].offset);

        const Elf32_Rela *rel = (const Elf32_Rela *)(elf + sh[i].offset);
        const int count = (int)(sh[i].size / sizeof(Elf32_Rela));
        for (int r = 0; r < count; r++) {
            const uint32_t type = rel[r].info & 0xFF;
            // Everything PC-relative inside the image is already correct, because the image
            // is copied in one piece and nothing outside it is reached that way (see
            // docs/GAME_API.md 6.1). ASM_EXPAND is a relaxation hint and we linked with
            // relaxation off, so only the absolute words are left to fix.
            if (type == R_XTENSA_SLOT0_OP || type == R_XTENSA_ASM_EXPAND) continue;
            if (type != R_XTENSA_32) {
                ESP_LOGE(TAG, "relocation type %lu is not supported", (unsigned long)type);
                return false;
            }

            const Elf32_Sym *sym = &syms[rel[r].info >> 8];
            uintptr_t value;
            if (sym->shndx == SHN_UNDEF) {
                const char *name = strs + sym->name;
                const void *addr = strcmp(name, "tat_assets") == 0 ? (const void *)assets
                                                                   : loader_lookup_export(name);
                if (!addr) {
                    ESP_LOGE(TAG, "this game wants '%s', which the console does not export", name);
                    return false;
                }
                value = (uintptr_t)addr;
            } else {
                // Linked at zero, so the symbol's value is its offset in the image - and
                // which view that offset is taken from depends on whether it is code.
                value = base_for(sh, sym->shndx, data_base, exec_base) + sym->value;
            }
            // The package is a linked executable, not an object file, so r_offset is
            // already an address in the image rather than an offset within the section.
            // Adding the section's address on top of it - which is what an object file
            // would need - writes wildly past the end and corrupts the heap; .rela.rodata
            // is where the game's descriptor lives, so it does it every single time.
            const uint32_t where = rel[r].offset;
            if (where + 4 > span) {
                ESP_LOGE(TAG, "relocation at %08lx is outside the image", (unsigned long)where);
                return false;
            }
            *(uint32_t *)(ram + where) = (uint32_t)(value + (uintptr_t)rel[r].addend);
        }
    }
    return true;
}

static const void *find_symbol(const uint8_t *elf, const Elf32_Shdr *sh, int shnum,
                               const char *want, uintptr_t data_base, uintptr_t exec_base)
{
    for (int i = 0; i < shnum; i++) {
        if (sh[i].type != SHT_SYMTAB) continue;
        const Elf32_Sym *syms = (const Elf32_Sym *)(elf + sh[i].offset);
        const char *strs = (const char *)(elf + sh[sh[i].link].offset);
        const int n = (int)(sh[i].size / sizeof(Elf32_Sym));
        for (int s = 0; s < n; s++)
            if (syms[s].shndx != SHN_UNDEF && strcmp(strs + syms[s].name, want) == 0)
                return (const void *)(base_for(sh, syms[s].shndx, data_base, exec_base) + syms[s].value);
    }
    return NULL;
}

loader_game_t *loader_open(const char *path)
{
    size_t len = 0;
    uint8_t *file = read_all(path, &len);
    if (!file) {
        ESP_LOGE(TAG, "cannot read %s", path);
        return NULL;
    }
    const tat_header_t *h;
    const tat_section_t *secs;
    if (!check_package(file, len, &h, &secs) || h->kind != KIND_GAME) {
        ESP_LOGE(TAG, "%s is not a usable game package", path);
        heap_caps_free(file);
        return NULL;
    }
    if (h->api_major != TAT_API_MAJOR || h->api_minor > TAT_API_MINOR) {
        ESP_LOGE(TAG, "%s wants API %u.%u; this console has %u.%u", h->name, h->api_major,
                 h->api_minor, TAT_API_MAJOR, TAT_API_MINOR);
        heap_caps_free(file);
        return NULL;
    }

    const uint8_t *elf = NULL;
    size_t elf_len = 0;
    int assets = 0;
    for (int i = 0; i < h->section_count; i++) {
        if (secs[i].type == SEC_CODE) elf = file + secs[i].offset, elf_len = secs[i].size;
        else if (secs[i].type == SEC_ASSET) assets++;
    }
    if (!elf || elf_len < sizeof(Elf32_Ehdr)) {
        ESP_LOGE(TAG, "%s has no code", h->name);
        heap_caps_free(file);
        return NULL;
    }

    const Elf32_Ehdr *eh = (const Elf32_Ehdr *)elf;
    if (memcmp(eh->ident, "\x7f" "ELF\1\1", 6) != 0 || eh->machine != EM_XTENSA) {
        ESP_LOGE(TAG, "%s: not an Xtensa ELF", h->name);
        heap_caps_free(file);
        return NULL;
    }
    // It has to be a linked executable: relocation offsets mean something different in an
    // object file, and taking one for the other writes all over memory.
    if (eh->type != ET_EXEC) {
        ESP_LOGE(TAG, "%s: ELF type %u, expected a linked executable", h->name, eh->type);
        heap_caps_free(file);
        return NULL;
    }
    if (eh->shoff + (size_t)eh->shnum * sizeof(Elf32_Shdr) > elf_len) {
        ESP_LOGE(TAG, "%s: the section table runs past the end of the code", h->name);
        heap_caps_free(file);
        return NULL;
    }
    const Elf32_Shdr *sh = (const Elf32_Shdr *)(elf + eh->shoff);
    const size_t span = image_span(sh, eh->shnum);
    if (span == 0 || span > 1024 * 1024) {
        ESP_LOGE(TAG, "%s: image is %u bytes", h->name, (unsigned)span);
        heap_caps_free(file);
        return NULL;
    }

    loader_game_t *g = calloc(1, sizeof(*g));
    if (!g) {
        heap_caps_free(file);
        return NULL;
    }
    g->file = file;

    // The asset table is built before the code is relocated, because the game's descriptor
    // points at it and that pointer is one of the things relocation has to fill in.
    if (assets) {
        g->assets = calloc(assets, sizeof(tat_asset_t));
        if (!g->assets) {
            ESP_LOGE(TAG, "no memory for %s's file table", h->name);
            loader_close(g);
            return NULL;
        }
        for (int i = 0; i < h->section_count; i++) {
            if (secs[i].type != SEC_ASSET || secs[i].size < 16) continue;
            // name[16] then the bytes; both stay inside the package we are holding on to.
            const uint8_t *p = file + secs[i].offset;
            tat_asset_t *a = &g->assets[g->asset_count++];
            a->name = (const char *)p;
            a->data = p + 16;
            a->end = p + secs[i].size;
        }
    }

    // An MMU page is the unit that can be mapped, so the image gets a whole number of them
    // and has to start on one.
    const size_t kPage = 64 * 1024;
    g->span = (span + kPage - 1) / kPage * kPage;
    g->ram = heap_caps_aligned_alloc(kPage, g->span, MALLOC_CAP_SPIRAM);
    if (!g->ram) {
        ESP_LOGE(TAG, "no memory for %s (%u bytes)", h->name, (unsigned)g->span);
        loader_close(g);
        return NULL;
    }
    memset(g->ram, 0, g->span);

    for (int i = 0; i < eh->shnum; i++) {
        if (!(sh[i].flags & SHF_ALLOC)) continue;
        if (sh[i].type == SHT_NOBITS) continue;   /* .bss: already zeroed above */
        if (sh[i].offset + sh[i].size > elf_len || sh[i].addr + sh[i].size > g->span) {
            ESP_LOGE(TAG, "%s: section %d does not fit its own image", h->name, i);
            loader_close(g);
            return NULL;
        }
        memcpy((uint8_t *)g->ram + sh[i].addr, elf + sh[i].offset, sh[i].size);
    }

    // The code has to be reachable through the instruction bus, and that is a different
    // address for the same physical memory. Map it first, because every pointer inside the
    // image has to be relocated to where the game will actually see itself - then write the
    // relocations through the data view, which is the one that can be written at all.
    esp_paddr_t paddr = 0;
    mmu_target_t target = MMU_TARGET_PSRAM0;
    if (esp_mmu_vaddr_to_paddr(g->ram, &paddr, &target) != ESP_OK) {
        ESP_LOGE(TAG, "cannot find the physical address of the image");
        loader_close(g);
        return NULL;
    }
    esp_err_t err = esp_mmu_map(paddr, g->span, MMU_TARGET_PSRAM0,
                                MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ, 0, &g->exec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot map the code to run: %s", esp_err_to_name(err));
        loader_close(g);
        return NULL;
    }
    ESP_LOGI(TAG, "image %u bytes: data view %p, exec view %p", (unsigned)g->span, g->ram, g->exec);

    if (!apply_relocations(elf, sh, eh->shnum, (uint8_t *)g->ram, g->span, (uintptr_t)g->ram,
                           (uintptr_t)g->exec, g->assets)) {
        loader_close(g);
        return NULL;
    }

    ESP_LOGI(TAG, "relocations applied");

    // Code written as data has to reach memory before the instruction side reads it.
    esp_cache_msync(g->ram, g->span, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    ESP_LOGI(TAG, "cache synced");

    // The descriptor is const data, so find_symbol hands back its address in the data view;
    // the function pointers inside it were relocated to the executable one.
    g->desc = (const tat_game_t *)find_symbol(elf, sh, eh->shnum, "tat_game", (uintptr_t)g->ram,
                                              (uintptr_t)g->exec);
    if (!g->desc || g->desc->magic != TAT_GAME_MAGIC) {
        ESP_LOGE(TAG, "%s exports no game", h->name);
        loader_close(g);
        return NULL;
    }

    ESP_LOGI(TAG, "loaded %s: %u bytes of image at %p, %d assets", h->name, (unsigned)span,
             g->exec, g->asset_count);
    return g;
}

const tat_game_t *loader_descriptor(loader_game_t *g) { return g ? g->desc : NULL; }

const tat_asset_t *loader_assets(loader_game_t *g, int *count)
{
    if (count) *count = g ? g->asset_count : 0;
    return g ? g->assets : NULL;
}

void loader_close(loader_game_t *g)
{
    if (!g) return;
    if (g->exec) esp_mmu_unmap(g->exec);
    if (g->ram) heap_caps_free(g->ram);
    if (g->file) heap_caps_free(g->file);
    free(g->assets);
    free(g);
}
