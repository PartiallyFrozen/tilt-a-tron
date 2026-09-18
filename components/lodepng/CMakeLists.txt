# lodepng (zlib license, https://github.com/lvandeve/lodepng): PNG decoding for
# user themes. Only the decoder is built.
idf_component_register(
    SRCS "lodepng.cpp"
    INCLUDE_DIRS "."
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC LODEPNG_NO_COMPILE_CPP)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function)
