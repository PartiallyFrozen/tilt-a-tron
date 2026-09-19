idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs wear_levelling esp_partition vfs nvs_flash
    EMBED_FILES
        "../../themes/README.txt"
        "../../themes/Default/theme.json"
        "../../themes/Default/background.png"
        "../../themes/Default/icons/settings.png"
        "../../themes/Default/icons/clock.png"
        "../../themes/Guide/guide-home.png"
        "../../themes/Guide/guide-menus.png"
        "../../themes/Guide/guide-icon.png"
)
