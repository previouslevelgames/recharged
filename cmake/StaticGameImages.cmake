include_guard(GLOBAL)

# Static game images: the game modules linked into the program instead of
# loaded at run time, for systems without dynamic loading (Nintendo Switch).
# Each module keeps its sources, settings and export map; only its link step
# changes: tools/link_static_game_image.py, its CXX_LINKER_LAUNCHER, links it
# into one relocatable object (the target file, <module>.o) whose symbols are
# local but for its exports, as the module's version script makes them. The
# program links the images, a table of them (written here once the export maps
# are final) and explicit providers for the functions the game calls that no
# source defines yet (src/platform/static_game_image_providers.cpp).
option(MSCHARGED_STATIC_GAME "Link the game modules into the program instead of loading them (no dynamic loading)" OFF)
if(MSCHARGED_STATIC_GAME AND (WIN32 OR APPLE))
    message(FATAL_ERROR "Static game images are ELF relocatable objects: Linux and Nintendo Switch builds only")
endif()

set(MSCHARGED_STATIC_GAME_IMAGE_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/link_static_game_image.py")
set(MSCHARGED_STATIC_GAME_IMAGE_PROVIDERS "${CMAKE_CURRENT_LIST_DIR}/../src/platform/static_game_image_providers.cpp")
cmake_path(NORMAL_PATH MSCHARGED_STATIC_GAME_IMAGE_TOOL)
cmake_path(NORMAL_PATH MSCHARGED_STATIC_GAME_IMAGE_PROVIDERS)
if(BUILD_TESTING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_test(NAME static_game_image COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_LIST_DIR}/../tests/test_static_game_image.py"
        "${CMAKE_CXX_COMPILER}" "${CMAKE_LINKER}" "${CMAKE_OBJCOPY}" "${CMAKE_NM}" "${CMAKE_READELF}")
    set_tests_properties(static_game_image PROPERTIES TIMEOUT 120)
endif()

# Links the game module `target` as the static game image `image` (us, eu).
function(mscharged_make_static_game_image target image)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Only a game module becomes a static game image: ${target}")
    endif()
    foreach(_tool IN ITEMS CMAKE_LINKER CMAKE_OBJCOPY CMAKE_NM CMAKE_READELF)
        if(NOT ${_tool})
            message(FATAL_ERROR "Static game images need ${_tool} (GNU binutils)")
        endif()
    endforeach()
    set_target_properties("${target}" PROPERTIES PREFIX "" SUFFIX ".o"
        CXX_LINKER_LAUNCHER "${Python3_EXECUTABLE};-B;${MSCHARGED_STATIC_GAME_IMAGE_TOOL};--ld;${CMAKE_LINKER};--objcopy;${CMAKE_OBJCOPY};--nm;${CMAKE_NM};--readelf;${CMAKE_READELF};--image;${image};--providers;${MSCHARGED_STATIC_GAME_IMAGE_PROVIDERS};--"
        MSCHARGED_STATIC_GAME_IMAGE "${image}")
    set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
        "${MSCHARGED_STATIC_GAME_IMAGE_TOOL}" "${MSCHARGED_STATIC_GAME_IMAGE_PROVIDERS}")
    set_property(GLOBAL APPEND PROPERTY MSCHARGED_STATIC_GAME_IMAGE_TARGETS "${target}")
endfunction()

# The program `program` links every static game image. Deferred to the end of
# the configuration: feature selections replace a module's export map until then.
function(mscharged_link_static_game_images program)
    # A deferred call expands its arguments when it runs: pass the name itself.
    cmake_language(EVAL CODE "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]]
        CALL mscharged_finish_static_game_images [[${program}]])")
endfunction()

function(mscharged_finish_static_game_images program)
    get_property(_targets GLOBAL PROPERTY MSCHARGED_STATIC_GAME_IMAGE_TARGETS)
    if(NOT _targets)
        message(FATAL_ERROR "${program} links static game images, but no game module became one")
    endif()
    set(_declarations "")
    set(_definitions "")
    set(_list "")
    foreach(_target IN LISTS _targets)
        get_target_property(_image "${_target}" MSCHARGED_STATIC_GAME_IMAGE)
        get_target_property(_map "${_target}" MSCHARGED_ORIGINAL_EXPORT_MAP)
        file(READ "${_map}" _text)
        string(REGEX MATCH "global:[^}]*local:" _globals "${_text}")
        string(REGEX REPLACE "^global:|local:$" "" _globals "${_globals}")
        string(REGEX REPLACE "[ \t\r\n]" "" _globals "${_globals}")
        set(_exports "")
        foreach(_name IN LISTS _globals)
            if(_name STREQUAL "")
                continue()
            endif()
            string(APPEND _declarations "extern char charged_image_${_image}__${_name}[] __attribute__((weak));\n")
            string(APPEND _exports "    {\"${_name}\", charged_image_${_image}__${_name}},\n")
        endforeach()
        # The linker's section bounds; weak, as an image may have no data or zeroed data.
        string(APPEND _declarations
            "extern void (*const __start_charged_image_${_image}_init[])() __attribute__((weak));\n"
            "extern void (*const __stop_charged_image_${_image}_init[])() __attribute__((weak));\n")
        set(_ranges "")
        foreach(_section IN ITEMS text rodata data bss)
            string(APPEND _declarations
                "extern const char __start_charged_image_${_image}_${_section}[] __attribute__((weak));\n"
                "extern const char __stop_charged_image_${_image}_${_section}[] __attribute__((weak));\n")
            string(APPEND _ranges
                "{__start_charged_image_${_image}_${_section}, __stop_charged_image_${_image}_${_section}}, ")
        endforeach()
        string(APPEND _definitions
            "const ChargedStaticGameExport ${_image}_exports[] = {\n${_exports}};\n"
            "const ChargedStaticGameImage ${_image}_image = {\"${_image}\", \"$<TARGET_FILE_NAME:${_target}>\",\n"
            "    ${_image}_exports, sizeof ${_image}_exports / sizeof *${_image}_exports,\n"
            "    __start_charged_image_${_image}_init, __stop_charged_image_${_image}_init,\n"
            "    {${_ranges}}};\n")
        string(APPEND _list "&${_image}_image, ")
        target_link_options("${program}" PRIVATE "$<TARGET_FILE:${_target}>")
        set_property(TARGET "${program}" APPEND PROPERTY LINK_DEPENDS "$<TARGET_FILE:${_target}>")
        add_dependencies("${program}" "${_target}")
    endforeach()
    set(_table "${CMAKE_BINARY_DIR}/generated/static_game_images.cpp")
    file(GENERATE OUTPUT "${_table}" CONTENT
"// Written by cmake/StaticGameImages.cmake: the static game images linked into this program.
#include \"platform/static_game_image.h\"

extern \"C\" {
${_declarations}}

namespace {
${_definitions}}

extern \"C\" const ChargedStaticGameImage* const* charged_static_game_images()
{
    static const ChargedStaticGameImage* const images[] = {${_list}nullptr};
    return images;
}
")
    target_sources("${program}" PRIVATE "${_table}" "${MSCHARGED_STATIC_GAME_IMAGE_PROVIDERS}")
    target_compile_definitions(charged_native_module_loader PRIVATE MSCHARGED_STATIC_GAME_IMAGES=1)
endfunction()
