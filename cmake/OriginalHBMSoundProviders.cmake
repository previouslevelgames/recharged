include_guard(GLOBAL)
include(cmake/OriginalHBMSoundArchive.cmake)
include(cmake/OriginalMEMExpHeap.cmake)
include(cmake/OriginalModuleLinkage.cmake)

# Whole original sound/player software in the same module as its source AX,
# allocator registry and completed archive owners. This selects no initializer,
# voice, sound ID, HOME event, archive data or successful hardware readiness.
function(mscharged_select_original_hbm_sound_providers target)
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Original HOME sound must share the sole original game module")
    endif()
    mscharged_select_original_hbm_sound_archive("${target}")
    mscharged_add_original_mem_exp_heap("${target}")
    mscharged_select_original_hbm_debug("${target}")
    get_target_property(_selected "${target}" SOURCES)
    foreach(_name IN ITEMS
        snd_AxManager
        snd_AxVoice
        snd_Bank
        snd_BankFile
        snd_BasicSound
        snd_Channel
        snd_DisposeCallbackManager
        snd_DvdSoundArchive
        snd_EnvGenerator
        snd_ExternalSoundPlayer
        snd_FrameHeap
        snd_InstancePool
        snd_Lfo
        snd_MemorySoundArchive
        snd_MmlParser
        snd_MmlSeqTrack
        snd_MmlSeqTrackAllocator
        snd_NandSoundArchive
        snd_PlayerHeap
        snd_RemoteSpeaker
        snd_RemoteSpeakerManager
        snd_SeqFile
        snd_SeqPlayer
        snd_SeqSound
        snd_SeqSoundHandle
        snd_SeqTrack
        snd_SoundArchive
        snd_SoundArchiveFile
        snd_SoundArchiveLoader
        snd_SoundArchivePlayer
        snd_SoundHandle
        snd_SoundHeap
        snd_SoundPlayer
        snd_SoundStartable
        snd_SoundSystem
        snd_SoundThread
        snd_StrmChannel
        snd_StrmFile
        snd_StrmPlayer
        snd_StrmSound
        snd_StrmSoundHandle
        snd_TaskManager
        snd_TaskThread
        snd_Util
        snd_WaveFile
        snd_WavePlayer
        snd_WaveSound
        snd_WaveSoundHandle
        snd_WsdFile
        snd_WsdPlayer
        snd_WsdTrack)
        set(_source "${MSCHARGED_PREPARED}/src/RVL_SDK/hbm/nw4hbm/snd/${_name}.cpp")
        if(NOT _source IN_LIST _selected)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _selected "${_source}")
        endif()
    endforeach()
    foreach(_relative IN ITEMS
        src/RVL_SDK/hbm/nw4hbm/ut/ut_DvdFileStream.cpp
        src/RVL_SDK/hbm/nw4hbm/ut/ut_LinkList.cpp
        src/RVL_SDK/mem/mem_frameHeap.c
        src/RVL_SDK/mem/mem_unitHeap.c
        src/RVL_SDK/os/OSMutex.c
        src/RVL_SDK/wenc/wenc.c)
        set(_source "${MSCHARGED_PREPARED}/${_relative}")
        if(NOT _source IN_LIST _selected)
            target_sources("${target}" PRIVATE "${_source}")
            list(APPEND _selected "${_source}")
        endif()
    endforeach()
    foreach(_relative IN ITEMS
        src/RVL_SDK/mem/mem_frameHeap.c
        src/RVL_SDK/mem/mem_unitHeap.c
        src/RVL_SDK/os/OSMutex.c
        src/RVL_SDK/wenc/wenc.c)
        set_property(SOURCE "${MSCHARGED_PREPARED}/${_relative}"
            TARGET_DIRECTORY "${target}" PROPERTY LANGUAGE C)
        set_property(SOURCE "${MSCHARGED_PREPARED}/${_relative}"
            TARGET_DIRECTORY "${target}" APPEND PROPERTY COMPILE_OPTIONS
            -fexceptions -Werror=pointer-to-int-cast -Werror=implicit-function-declaration)
    endforeach()
    target_compile_features("${target}" PRIVATE c_std_17 cxx_std_20)
endfunction()

# The platform kernel and these original OS message bodies have one host owner.
# The source module imports them; it links no second native SDK/device instance.
function(mscharged_link_original_hbm_sound_host target)
    get_target_property(_type "${target}" TYPE)
    if(_type STREQUAL "OBJECT_LIBRARY")
        set(_scope INTERFACE)
    else()
        set(_scope PRIVATE)
    endif()
    if(NOT TARGET charged_original_os_messages)
        include(cmake/OriginalOSMessages.cmake)
    endif()
    target_link_libraries("${target}" ${_scope} charged_original_os_messages)
    foreach(_symbol IN ITEMS
        AICheckInit AIGetDMABytesLeft AIInit
        ChargedDSPControlRead ChargedDSPControlWrite
        ChargedDSPMailFromHigh ChargedDSPMailFromLow ChargedDSPMailToHigh
        ChargedDSPMailToWriteHigh ChargedDSPMailToWriteLow ChargedDSPRequireMailWord
        DCFlushRangeNoSync OSClearContext OSCreateThread OSGetAlarmUserData
        OSInitMessageQueue OSJoinThread OSReceiveMessage OSRegisterVersion
        OSResumeThread OSSendMessage OSSetCurrentContext OSSetPeriodicAlarm
        WPADCanSendStreamData WPADSendStreamData
        __OSSetInterruptHandler __OSUnmaskInterrupts)
        mscharged_require_original_host_symbol("${target}" ${_scope} "${_symbol}")
    endforeach()
endfunction()
