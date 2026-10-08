include_guard(GLOBAL)
# Complete reconstructed font and text layout units. CPU qualifiers exercise
# descriptor parsing/metrics/layout; original manager callbacks qualify separately.
add_library(charged_original_fonts STATIC
    "${MSCHARGED_PREPARED}/src/NL/nlFont.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTextEscape.cpp"
    "${MSCHARGED_PREPARED}/src/NL/nlTextBox.cpp")
add_dependencies(charged_original_fonts verify_prepared)
target_compile_features(charged_original_fonts PUBLIC cxx_std_17)
target_include_directories(charged_original_fonts PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_original_fonts PUBLIC
    charged_original_core charged_game_print)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_fonts PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
elseif(MSVC)
    target_compile_options(charged_original_fonts PRIVATE /Gy /Gw)
endif()

# Whole original manager. Its unresolved GL providers belong to the genuine
# game graph; this library supplies no rendering or alternate font readiness.
add_library(charged_original_font_manager STATIC
    "${MSCHARGED_PREPARED}/src/Game/Font/fontmanager.cpp")
add_dependencies(charged_original_font_manager verify_prepared)
target_compile_features(charged_original_font_manager PUBLIC cxx_std_17)
target_include_directories(charged_original_font_manager PRIVATE
    "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
target_link_libraries(charged_original_font_manager PUBLIC
    charged_original_fonts charged_original_bundles)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU" AND NOT MSVC)
    target_compile_options(charged_original_font_manager PRIVATE
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
elseif(MSVC)
    target_compile_options(charged_original_font_manager PRIVATE /Gy /Gw)
endif()

if(BUILD_TESTING)
    add_executable(original_textbox_tests tests/original_textbox.cpp)
    target_compile_features(original_textbox_tests PRIVATE cxx_std_20)
    target_include_directories(original_textbox_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_textbox_tests PRIVATE
        charged_original_fonts charged_original_bundles
        aurora::dvd aurora::core SDL3::SDL3)
    # This CPU source qualifier discards only unexecuted DrawString and colour
    # rendering providers. It supplies no graphics/font-manager readiness.
    if(MSVC)
        target_compile_options(original_textbox_tests PRIVATE /Gy /Gw)
        target_link_options(original_textbox_tests PRIVATE /OPT:REF)
    elseif(APPLE)
        target_link_options(original_textbox_tests PRIVATE -Wl,-dead_strip)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(original_textbox_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME original_textbox COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_textbox.py"
        "$<TARGET_FILE:original_textbox_tests>")
    set_tests_properties(original_textbox PROPERTIES TIMEOUT 120
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()
if(BUILD_TESTING)
    add_executable(original_font_tests tests/original_font.cpp)
    target_compile_features(original_font_tests PRIVATE cxx_std_20)
    target_include_directories(original_font_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_link_libraries(original_font_tests PRIVATE
        charged_original_fonts charged_original_bundles charged_decomp_startup
        aurora::dvd aurora::core)
    # Drop unexecuted DrawString/GetExtendedColour only in this partial-link
    # qualifier. Their actual GL/string-support providers remain required by
    # the eventual whole-game production link.
    if(MSVC)
        target_compile_options(original_font_tests PRIVATE /Gy /Gw)
        target_link_options(original_font_tests PRIVATE /OPT:REF)
    elseif(APPLE)
        target_link_options(original_font_tests PRIVATE -Wl,-dead_strip)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_link_options(original_font_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME original_fonts COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_font.py"
        "$<TARGET_FILE:original_font_tests>")
    set_tests_properties(original_fonts PROPERTIES TIMEOUT 120
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()

# Actual SDK texture metadata is required even though this qualifier never
# initializes GX or draws. The inherited Game Replay header currently fails
# Clang's pointer-truncation check; its replay ABI remains an explicit gate.
# Keep this bounded early-arena fixture restricted to its qualified GNU host.
if(BUILD_TESTING AND TARGET aurora::gx
    AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
    add_executable(original_font_loading_tests
        tests/original_font_loading.cpp tests/original_font_loading_host.cpp
        "${MSCHARGED_PREPARED}/src/Game/Font/fontmanager.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlFont.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlTextEscape.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlBundleFile.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLTextureAnim.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glTextureManager.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glTexture.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLInventory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/glResourcePool.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/glx/glxTexture.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gl/gl.cpp"
        "${MSCHARGED_PREPARED}/src/NL/gc/gcSwizzler.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GL/GLVertexAnim.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlAVLTree.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlString.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/MemAlloc.cpp"
        "${MSCHARGED_PREPARED}/src/NL/plat/nlMemory.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlDebug.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlSlotPool.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakRegistry.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakValueBase.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakNode.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakEntry.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakNameRecycler.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakValue.cpp"
        "${MSCHARGED_PREPARED}/src/Game/TweakCallback.cpp"
        "${MSCHARGED_PREPARED}/src/Game/GameTweaks.cpp"
        "${MSCHARGED_PREPARED}/src/NL/nlStringSupport.cpp"
        "${MSCHARGED_PREPARED}/src/Game/AIPad.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Game.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feResourceManager.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feFontResource.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feScene.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/BaseSceneHandler.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Task/FrontEndTask.cpp"
        "${MSCHARGED_PREPARED}/src/Game/Team.cpp"
        "${MSCHARGED_PREPARED}/src/Game/AI/StatsGatherer.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feSceneResource.cpp"
        "${MSCHARGED_PREPARED}/src/Game/FE/feTextureResource.cpp"
        src/platform/tweak_storage.cpp)
    target_compile_features(original_font_loading_tests PRIVATE cxx_std_20)
    target_include_directories(original_font_loading_tests PRIVATE
        "${MSCHARGED_PREPARED}/libs/RVL_SDK/include")
    target_compile_definitions(original_font_loading_tests PRIVATE dSINGLE=1)
    # Match the qualified partial-link fixture policy. At O3, GCC devirtualizes
    # generic shared_ptr release into unexecuted Game event-template providers;
    # their genuine event/source closure is a separate gate. Production source
    # libraries and compiler inventories retain the selected profile's flags.
    target_compile_options(original_font_loading_tests PRIVATE
        -O1
        -ffp-contract=off -fno-strict-aliasing -fsigned-char -Wno-unknown-pragmas
        -ffunction-sections -fdata-sections)
    # Discard only unexecuted whole game/renderer providers in this CPU test.
    # All original font/FE/resource decisions and genuine static constructors
    # remain linked; no source methods are replaced or made successful.
    target_link_options(original_font_loading_tests PRIVATE -Wl,--gc-sections)
    target_link_libraries(original_font_loading_tests PRIVATE
        charged_original_core charged_game_print aurora::dvd aurora::core
        aurora::gx aurora::mtx SDL3::SDL3)
    add_test(NAME original_font_loading COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_original_font_loading.py"
        "$<TARGET_FILE:original_font_loading_tests>")
    set_tests_properties(original_font_loading PROPERTIES TIMEOUT 120
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_AUDIODRIVER=dummy")
endif()

include(cmake/OriginalStringPrefix.cmake)
include(cmake/OriginalTempStrings.cmake)
