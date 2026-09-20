/* The table a game reads its files through. The game's own sources see it declared const in
   tat_api.h, because on a watch the loader has filled it in before the game can look. Here
   it is filled in at start-up from the game's assets folder, so it is defined in a file
   that does not include that header and can write to it. */
struct emu_asset {
    const char *name;
    const unsigned char *data, *end;
};
struct emu_asset tat_assets[32];

void emu_set_asset(int slot, const char *name, const unsigned char *data, const unsigned char *end)
{
    if (slot < 0 || slot >= 32) return;
    tat_assets[slot].name = name;
    tat_assets[slot].data = data;
    tat_assets[slot].end = end;
}
