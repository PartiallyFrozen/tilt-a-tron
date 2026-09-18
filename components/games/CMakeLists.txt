idf_component_register(
    SRCS "breakout.cpp" "maze.cpp" "racer.cpp" "jump.cpp"
    INCLUDE_DIRS "include"
    REQUIRES engine wc_console audio esp_hw_support nvs_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
