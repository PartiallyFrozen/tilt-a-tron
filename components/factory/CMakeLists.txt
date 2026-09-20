# The games a new watch comes with.
#
# No game is part of the firmware: they are all packages, installed and removed like any
# other. But a watch has to come with something to play, and its storage can be wiped, so
# the firmware carries a factory copy of each and puts them in place the first time it
# starts. They are built here by the same tool anyone else would use - tools/mktat.py, from
# games/<id>/ - so the firmware's own games are also the proof that the tool works.
idf_component_register(
    SRCS "factory.c"
    INCLUDE_DIRS "include"
    REQUIRES loader storage nvs_flash
)

set(FACTORY_GAMES breakout maze racer jump tiltatris star pindrop starfall skatergirlz echo radar)

set(repo "${CMAKE_CURRENT_LIST_DIR}/../..")
set(mktat "${repo}/tools/mktat.py")
set(out_dir "${CMAKE_BINARY_DIR}/factory")
idf_build_get_property(python PYTHON)

foreach(game ${FACTORY_GAMES})
    set(src "${repo}/games/${game}")
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS "${src}/*")
    set(tat "${out_dir}/${game}.tat")
    add_custom_command(
        OUTPUT "${tat}"
        COMMAND ${python} "${mktat}" "${src}" --out "${tat}" --gcc "${CMAKE_C_COMPILER}"
                --work "${out_dir}/work"
        DEPENDS ${inputs} "${mktat}" "${repo}/components/tat_api/include/tat/tat_api.h"
        COMMENT "Packing ${game}.tat"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${tat}" BINARY)
endforeach()
