include_guard(GLOBAL)

# Temporary owner-authorized source-endpoint integration. The default normal
# game and selected Credits diagnostic are unchanged while this option is OFF.
# Full Async/presentation service dispatch, FE AX/SFX and Title remain held.
# Remove this selection once the actual loading dispatcher owns the full route.
option(MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE
    "Select original Boot/Nav/Intro source endpoints in the main diagnostic" OFF)
function(mscharged_select_original_frontend_sequence target)
    if(NOT MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE)
        return()
    endif()
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "The frontend sequence requires the original-main module")
    endif()
    get_target_property(_sequence_type "${target}" TYPE)
    if(NOT _sequence_type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Frontend sequence game providers belong only to the isolated original module")
    endif()
    target_compile_definitions("${target}" PRIVATE
        MSCHARGED_DIAGNOSTIC_FRONTEND_SEQUENCE=1)
    # Original presentation includes its source-side interpreter implementation.
    target_include_directories("${target}" PRIVATE "${MSCHARGED_PREPARED}")
    get_target_property(_sequence_existing "${target}" SOURCES)
    foreach(_sequence_source IN ITEMS
        src/Game/AsyncLoading.cpp
        src/Game/FE/GameSceneManager.cpp
        src/Game/FE/feDPD.cpp
        src/Game/FE/fePageControls.cpp
        src/Game/FE/fePointerManager.cpp
        src/Game/FE/fePointer.cpp
        src/Game/FE/fePointerButton.cpp
        src/Game/FE/feCupFlow.cpp
        src/Game/FE/FEAudio.cpp
        src/Game/DB/BasicGameInfo.cpp
        src/Game/DB/UserOptions.cpp
        src/Game/DB/SaveLoad.cpp
        src/Game/DB/StadiumInfo.cpp
        src/Game/DB/GameProgress.cpp
        src/Game/SH/SHBootLoading.cpp
        src/Game/SH/SHNavigation.cpp
        src/Game/SH/SHHallOfFame.cpp
        src/Game/Render/FrontEndPresentation.cpp
        src/Game/InterpreterCore.cpp
        src/Game/InterpreterOperations.cpp
        src/Game/OverlayManager.cpp
        src/Game/RumbleActions.cpp
        src/Game/Audio/AudioSystem.cpp
        src/Game/Audio/AudioResourceRuntime.cpp
        src/Game/Audio/audio.cpp
        src/Game/Audio/GameStreams.cpp
        src/Game/Audio/XSoundHandle.cpp
        src/Game/Audio/XSoundCueHandle.cpp
        src/Game/Audio/SoundInstance.cpp
        src/Game/Audio/AudioRpc.cpp
        src/Game/Audio/AudioSequenceInstance.cpp
        src/Game/Audio/Transition.cpp
        src/NL/plat/nlFlash.cpp
        src/NL/nlDebugString.cpp)
        set(_sequence_path "${MSCHARGED_PREPARED}/${_sequence_source}")
        if(NOT _sequence_path IN_LIST _sequence_existing)
            target_sources("${target}" PRIVATE "${_sequence_path}")
        endif()
    endforeach()
    if(NOT "src/platform/vm_address_abi.cpp" IN_LIST _sequence_existing)
        target_sources("${target}" PRIVATE src/platform/vm_address_abi.cpp)
    endif()
    # Current prepared SaveLoad has permanent fixed32 TPL/Wii16 transport.
    # Its actual game-owned banner allocation is retained; Title is still not
    # admitted by this selected factory until its other providers qualify.
    include(cmake/OriginalSaveIcons.cmake)
    mscharged_add_original_save_icon_transport("${target}")
endfunction()
