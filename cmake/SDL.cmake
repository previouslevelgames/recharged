mscharged_prepare_dependency(sdl MSCHARGED_SDL_PREPARED)

function(mscharged_add_sdl)
    # This SDL pin deliberately adds extra escapes for legacy macro evaluation.
    # Keep that behavior scoped to SDL when CMake 4.4+ adds CMP0219.
    if(POLICY CMP0219)
        set(CMAKE_POLICY_DEFAULT_CMP0219 OLD)
    endif()
    set(SDL_SHARED OFF)
    set(SDL_STATIC ON)
    set(SDL_TEST_LIBRARY OFF)
    set(SDL_TESTS OFF)
    set(SDL_EXAMPLES OFF)
    set(SDL_INSTALL OFF)
    add_subdirectory("${MSCHARGED_SDL_PREPARED}" "${CMAKE_CURRENT_BINARY_DIR}/extern/sdl" EXCLUDE_FROM_ALL)
endfunction()
mscharged_add_sdl()
add_dependencies(SDL3-static verify_prepared)
