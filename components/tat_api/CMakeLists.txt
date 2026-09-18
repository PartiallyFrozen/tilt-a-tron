# The contract between the console and a game, plus the console's side of it.
idf_component_register(
    SRCS "tat_host.cpp"
    INCLUDE_DIRS "include"
    REQUIRES engine wc_console audio
)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
