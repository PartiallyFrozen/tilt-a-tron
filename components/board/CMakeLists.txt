idf_component_register(
    SRCS "board.c" "display.c" "touch.c" "imu.c" "buttons.c" "pmu.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_timer
)
