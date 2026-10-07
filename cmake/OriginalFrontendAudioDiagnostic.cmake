include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_AUDIO
    "Admit original Async Splash-bank requests in the selected frontend diagnostic" OFF)
function(mscharged_select_original_frontend_audio target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_AUDIO)
        return()
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE OR
       NOT MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE)
        message(FATAL_ERROR "Original frontend audio requires the named sequence and source GameAudio initialization")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original frontend audio providers belong to the isolated game module")
    endif()
    target_compile_definitions("${target}" PRIVATE MSCHARGED_DIAGNOSTIC_FRONTEND_AUDIO=1)
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN ITEMS
        src/Game/AsyncLoading.cpp
        src/NL/nlTicker.cpp
        src/NL/nlRandom.cpp
        src/NL/nlDebugFile.cpp
        src/Game/FE/feMusic.cpp)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _existing "${_path}")
        endif()
    endforeach()
    foreach(_symbol IN ITEMS OSGetTick ChargedGetBusClock)
        target_link_options(charged_original_main_credits_host INTERFACE
            "-Wl,--require-defined=${_symbol},--export-dynamic-symbol=${_symbol}")
    endforeach()
    # Existing native SP/byte domains, actual NL/allocator/AX providers and
    # whole Movie source are same-module initialization prerequisites.
    # This selector supplies no bank preload, VM state or cue invocation.
endfunction()
