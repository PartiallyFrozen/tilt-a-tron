idf_component_register(
    SRCS "link.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_usb_serial_jtag esp_app_format esp_timer storage esp_rom
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
