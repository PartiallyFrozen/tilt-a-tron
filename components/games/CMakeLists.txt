# The apps that are part of the firmware. There is one: the clock, which is the watch itself
# and needs things no game is given - the real-time clock, time zones, network time.
#
# Every game lives in games/<id>/ and is built as a package by tools/mktat.py. The ones a
# new watch comes with are carried by components/factory.
idf_component_register(
    SRCS "clock.cpp"
    INCLUDE_DIRS "include"
    REQUIRES engine wc_console audio esp_hw_support nvs_flash esp_timer net
    # The clock's faces borrow a few characters from the games. The art has one home, with
    # the game it belongs to; the clock embeds its own copy from there, so it goes on
    # working whether or not those games are installed.
    EMBED_FILES
        "../../games/jump/assets/hopper.png"
        "../../games/jump/assets/clouds.png"
        "../../games/jump/assets/ledges.png"
        "../../games/racer/assets/car.png"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
