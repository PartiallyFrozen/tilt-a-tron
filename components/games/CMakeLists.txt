idf_component_register(
    SRCS "breakout.cpp" "maze.cpp" "racer.cpp" "jump.cpp" "tiltatris.cpp" "clock.cpp"
    INCLUDE_DIRS "include"
    REQUIRES engine wc_console audio esp_hw_support nvs_flash esp_timer net
    EMBED_FILES
        "assets/jump/hopper.png"
        "assets/jump/monster.png"
        "assets/jump/ledges.png"
        "assets/jump/spring.png"
        "assets/jump/shot.png"
        "assets/jump/clouds.png"
        "assets/racer/car.png"
        "assets/racer/trees.png"
        "assets/racer/sign.png"
        "assets/racer/stand.png"
        "assets/racer/bush.png"
        "assets/maze/ball.png"
        "assets/maze/flag.png"
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
