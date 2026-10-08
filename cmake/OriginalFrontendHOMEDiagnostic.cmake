include_guard(GLOBAL)
include(cmake/OriginalFrontendHBMDiagnostic.cmake)

# Admission of literal original HOME calls only. The original FrontEndTask owns
# the input predicate and HBMManager owns all construction/state/return decisions.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_HOME
    "Admit the original HOME Show/Update/draw/Back path" OFF)

function(mscharged_select_original_frontend_home target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_HOME)
        return()
    endif()
    foreach(_required IN ITEMS
            MSCHARGED_DIAGNOSTIC_FRONTEND_HBM
            MSCHARGED_DIAGNOSTIC_FRONTEND_TASK_CADENCE
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_INITIALIZE
            MSCHARGED_DIAGNOSTIC_GAME_AUDIO_TASK
            MSCHARGED_DIAGNOSTIC_FRONTEND_FIXED_INPUT
            MSCHARGED_DIAGNOSTIC_FRONTEND_PLAT_PAD_TASK)
        if(NOT ${_required})
            message(FATAL_ERROR "Original HOME admission requires ${_required}")
        endif()
    endforeach()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HOME providers belong to the sole game module")
    endif()
    get_target_property(_selected "${target}" SOURCES)
    set(_sound_owner "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/snd_AxManager.cpp")
    if(NOT _sound_owner IN_LIST _selected)
        message(FATAL_ERROR "Original HOME needs the previously selected whole sound providers")
    endif()
    foreach(_relative IN ITEMS
        src/RVL_SDK/hbm/HBMBase.cpp
        src/RVL_SDK/hbm/HBMGUIManager.cpp
        src/Game/Render/HomeButtonFade.cpp)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _selected)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _selected "${_source}")
        endif()
    endforeach()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_HOME=1)
    # Full HBMCalc retains this original state17 request. The truthful native
    # SC store provider must be linked before activation; a missing import may
    # not be deferred by ELF's lazy function binding.
    foreach(_symbol IN ITEMS WPADSaveConfig WPADGetSpeakerVolume WPADSetSpeakerVolume)
        mscharged_require_original_host_symbol(charged_original_main_credits_host
            INTERFACE "${_symbol}")
    endforeach()
endfunction()
