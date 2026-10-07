include_guard(GLOBAL)

# Whole original lower providers used by GameAudio::Initialize. The original
# main/source owner chooses initialization; this selector supplies no readiness
# or SDK/device instance and belongs only to the isolated source game module.
# Constructor owners404 and AX/native storage101 are existing prerequisites.
function(mscharged_select_original_game_audio_init_providers target)
    get_target_property(_kind "${target}" TYPE)
    if(NOT _kind STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original audio initialization providers require the isolated game module")
    endif()
    get_target_property(_existing "${target}" SOURCES)
    foreach(_source IN ITEMS
        src/Game/Audio/AudioResourceBundle.cpp
        src/Game/Audio/AudioBankLoader.cpp
        src/Game/Audio/SoundMap.cpp
        src/RVL_SDK/dsp/dsp.c
        src/RVL_SDK/dsp/dsp_debug.c
        src/RVL_SDK/dsp/dsp_task.c
        src/RVL_SDK/mix/mix.c
        src/RVL_SDK/mix/remote.c
        src/RVL_SDK/axfx/AXFXDelay.c
        src/RVL_SDK/axfx/AXFXDelayExpDpl2.c
        src/RVL_SDK/axfx/AXFXHooks.c
        src/RVL_SDK/axfx/AXFXReverbHi.c
        src/RVL_SDK/axfx/AXFXReverbHiDpl2.c
        src/RVL_SDK/axfx/AXFXReverbHiExp.c
        src/RVL_SDK/axfx/AXFXReverbHiExpDpl2.c
        src/RVL_SDK/sp/sp.c
        src/RVL_SDK/wenc/wenc.c)
        set(_path "${MSCHARGED_PREPARED}/${_source}")
        if(NOT _path IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_path}")
            list(APPEND _existing "${_path}")
        endif()
    endforeach()
    foreach(_source IN ITEMS src/platform/native_sp_table.cpp)
        if(NOT _source IN_LIST _existing)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _existing "${_source}")
        endif()
    endforeach()
    target_compile_features("${target}" PRIVATE c_std_17)
endfunction()
