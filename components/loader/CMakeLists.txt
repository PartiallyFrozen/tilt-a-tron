# Loading a game that was not built into the firmware (docs/GAME_API.md sections 2 and 6).
idf_component_register(
    SRCS "loader.c" "exports.c" "packaged_game.cpp"
    INCLUDE_DIRS "include"
    REQUIRES tat_api storage esp_mm esp_rom engine wc_console
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
