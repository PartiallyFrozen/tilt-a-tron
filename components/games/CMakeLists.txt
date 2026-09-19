idf_component_register(
    SRCS "clock.cpp"
         "pindrop/pindrop.c" "jump/jump.c" "jump/jump_builtin.c" "tiltatris/tiltatris.c"
         "racer/racer.c" "racer/racer_builtin.c" "maze/maze.c" "maze/maze_builtin.c" "star/star.c"
         "breakout/breakout.c"
    INCLUDE_DIRS "include"
    REQUIRES engine wc_console audio esp_hw_support nvs_flash esp_timer net tat_api
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

# Built-in games keep the source a package would ship; only the symbols a package exports
# are renamed, so several can share one firmware and nothing in the game has to know which
# way it was built. A loaded package exports them under their plain names.
set_source_files_properties("pindrop/pindrop.c" PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_pindrop")
set_source_files_properties("jump/jump.c" "jump/jump_builtin.c"
    PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_jump;tat_assets=tat_assets_jump")
set_source_files_properties("tiltatris/tiltatris.c" PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_tiltatris")
set_source_files_properties("racer/racer.c" "racer/racer_builtin.c"
    PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_racer;tat_assets=tat_assets_racer")
set_source_files_properties("maze/maze.c" "maze/maze_builtin.c"
    PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_maze;tat_assets=tat_assets_maze")
set_source_files_properties("star/star.c" PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_star")
set_source_files_properties("breakout/breakout.c" PROPERTIES COMPILE_DEFINITIONS "tat_game=tat_game_breakout")
