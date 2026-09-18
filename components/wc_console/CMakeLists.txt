idf_component_register(
    SRCS "console.cpp" "launcher.cpp" "icons.cpp" "settings_app.cpp" "update_app.cpp" "wifi_setup.cpp" "scroll_list.cpp" "theme.cpp"
    INCLUDE_DIRS "include"
    REQUIRES engine board net storage audio lodepng json esp_system esp_app_format nvs_flash espcoredump
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
