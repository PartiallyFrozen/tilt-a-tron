idf_component_register(
    SRCS "audio.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_i2s nvs_flash
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
