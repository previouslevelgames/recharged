include_guard(GLOBAL)

option(MSCHARGED_DIAGNOSTIC_FRONTEND_DVD_MESSAGES
    "Admit the original DVD message callbacks in the frontend source module" OFF)
option(MSCHARGED_DIAGNOSTIC_FRONTEND_INITIAL_LOADING_MESSAGE
    "Admit the literal original initial loading message in frontend main" OFF)

function(mscharged_select_original_frontend_dvd_messages target)
    if(MSCHARGED_DIAGNOSTIC_FRONTEND_INITIAL_LOADING_MESSAGE
            AND NOT MSCHARGED_DIAGNOSTIC_FRONTEND_DVD_MESSAGES)
        message(FATAL_ERROR "Original initial loading message requires the whole DVD message owner")
    endif()
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_DVD_MESSAGES)
        return()
    endif()
    # The source callbacks borrow actual reset, audio and pad owners. Reset's
    # selector already requires the genuine task/audio initialization cohort.
    foreach(_required IN ITEMS MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
            MSCHARGED_DIAGNOSTIC_FRONTEND_RESET)
        if(NOT ${_required})
            message(FATAL_ERROR "Original DVD messages require ${_required}")
        endif()
    endforeach()
    get_target_property(_kind "${target}" TYPE)
    if(NOT _kind STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original DVD callbacks belong to the existing source module")
    endif()
    get_target_property(_reset_owner "${target}" MSCHARGED_ORIGINAL_RESET_OWNER)
    if(NOT _reset_owner)
        message(FATAL_ERROR "Original DVD callbacks require this module's original ResetTask owner")
    endif()
    # As with ResetTask, only this module's compilation of original main gets
    # the admission definition. The separately compiled Credits main stays scoped.
    set_property(TARGET "${target}" PROPERTY MSCHARGED_ORIGINAL_DVD_MESSAGE_OWNER TRUE)
    set_property(SOURCE "${MSCHARGED_PREPARED}/src/Game/main.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS
        "$<$<BOOL:$<TARGET_PROPERTY:MSCHARGED_ORIGINAL_DVD_MESSAGE_OWNER>>:MSCHARGED_DIAGNOSTIC_FRONTEND_DVD_MESSAGES=1>")
    set(_source "${MSCHARGED_PREPARED}/src/Game/FE/LidOpenMessage.cpp")
    get_target_property(_existing "${target}" SOURCES)
    if(NOT _source IN_LIST _existing)
        target_sources("${target}" PRIVATE "${_source}")
    endif()
    if(MSCHARGED_DIAGNOSTIC_FRONTEND_INITIAL_LOADING_MESSAGE)
        set_property(TARGET "${target}" PROPERTY MSCHARGED_ORIGINAL_INITIAL_LOADING_MESSAGE_OWNER TRUE)
        set_property(SOURCE "${MSCHARGED_PREPARED}/src/Game/main.cpp" APPEND PROPERTY
            COMPILE_DEFINITIONS
            "$<$<BOOL:$<TARGET_PROPERTY:MSCHARGED_ORIGINAL_INITIAL_LOADING_MESSAGE_OWNER>>:MSCHARGED_DIAGNOSTIC_FRONTEND_INITIAL_LOADING_MESSAGE=1>")
    endif()
endfunction()
