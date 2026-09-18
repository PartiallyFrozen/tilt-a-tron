# The Doom engine (GPL-2.0: see LICENSE) and its Tilt-a-tron platform layer (port/).
# src/ is doomgeneric (github.com/ozkl/doomgeneric), itself Chocolate Doom, with the
# desktop platform files left out; the few local changes are marked "TILT-A-TRON".
file(GLOB DOOM_SRCS "src/*.c")
idf_component_register(
    SRCS ${DOOM_SRCS} "port/doom_port.c" "port/doom_wad.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_partition esp_timer spi_flash
    LDFRAGMENTS "linker.lf"
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CMAP256=1 FEATURE_SOUND=1 DOOMGENERIC_RESX=320 DOOMGENERIC_RESY=200)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -w)
