idf_component_register(
    SRCS "main.cpp" "bench_game.cpp"
    INCLUDE_DIRS "."
    REQUIRES engine board games wc_console net storage audio nvs_flash app_update esp_timer lodepng heap
)
