# The Discord Social SDK for the launcher's Discord friends
# (src/launcher/discord_friends.cpp, docs/COMPETITIVE.md). It is Discord's
# closed library: its terms allow shipping it inside Recharged, not passing it
# on by itself, so it is never in this repository. MSCHARGED_DISCORD_SOCIAL_SDK
# names the folder of its download (discord_social_sdk, with include/ and the
# libraries); official builds get it from CI. Without it the launcher has no
# Discord friends. The launcher loads the library at run time, from beside the
# program, so a computer where it does not load just goes without.
set(MSCHARGED_DISCORD_SOCIAL_SDK "" CACHE PATH "Discord Social SDK folder (discord_social_sdk; empty: none)")

if(MSCHARGED_DISCORD_SOCIAL_SDK)
    set(_sdk "${MSCHARGED_DISCORD_SOCIAL_SDK}")
    if(WIN32)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
            set(MSCHARGED_DISCORD_SOCIAL_SDK_LIBRARY "${_sdk}/bin/release/arm64/discord_partner_sdk.dll")
        else()
            set(MSCHARGED_DISCORD_SOCIAL_SDK_LIBRARY "${_sdk}/bin/release/discord_partner_sdk.dll")
        endif()
    elseif(APPLE)
        set(MSCHARGED_DISCORD_SOCIAL_SDK_LIBRARY "${_sdk}/lib/release/libdiscord_partner_sdk.dylib")
    else()
        set(MSCHARGED_DISCORD_SOCIAL_SDK_LIBRARY "${_sdk}/lib/release/libdiscord_partner_sdk.so")
    endif()
    foreach(_file IN ITEMS "${_sdk}/include/cdiscord.h" "${MSCHARGED_DISCORD_SOCIAL_SDK_LIBRARY}"
                           "${_sdk}/License-Notices.txt")
        if(NOT EXISTS "${_file}")
            message(FATAL_ERROR "MSCHARGED_DISCORD_SOCIAL_SDK: ${_file} is missing")
        endif()
    endforeach()
    message(STATUS "Discord friends: with the Discord Social SDK from ${_sdk}")
endif()

# The SDK's header for `target`, which declares its C functions and loads
# them at run time; nothing links to the library.
function(mscharged_discord_social_sdk target)
    if(MSCHARGED_DISCORD_SOCIAL_SDK)
        target_include_directories(${target} SYSTEM PRIVATE "${MSCHARGED_DISCORD_SOCIAL_SDK}/include")
        target_compile_definitions(${target} PRIVATE MSCHARGED_HAS_DISCORD_SOCIAL_SDK=1)
    endif()
endfunction()

# The library and its notices beside the program `target`.
function(mscharged_discord_social_sdk_beside target)
    if(MSCHARGED_DISCORD_SOCIAL_SDK)
        get_filename_component(name "${MSCHARGED_DISCORD_SOCIAL_SDK_LIBRARY}" NAME)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MSCHARGED_DISCORD_SOCIAL_SDK_LIBRARY}"
                    "$<TARGET_FILE_DIR:${target}>/${name}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/LICENSES/discord-social-sdk"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MSCHARGED_DISCORD_SOCIAL_SDK}/License-Notices.txt"
                    "$<TARGET_FILE_DIR:${target}>/LICENSES/discord-social-sdk/License-Notices.txt"
            VERBATIM)
    endif()
endfunction()
