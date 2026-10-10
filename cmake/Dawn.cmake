# Pinned native GPU providers: Vulkan on Linux and Metal on macOS. Browser,
# toolchain, fuzzing, and upstream test sources remain omitted.
set(MSCHARGED_DAWN_NESTED
    third_party/abseil-cpp
    third_party/jinja2
    third_party/markupsafe
    third_party/spirv-headers/src
    # The shared patch series also patches this generator on Metal builds.
    # Its source must be exported even when the SPIR-V backend is disabled.
    third_party/spirv-tools/src)
if(NOT APPLE)
    list(APPEND MSCHARGED_DAWN_NESTED
        third_party/vulkan-headers/src
        third_party/vulkan-utility-libraries/src)
endif()
mscharged_prepare_dependency(dawn MSCHARGED_DAWN_PREPARED ${MSCHARGED_DAWN_NESTED})
# Use Dawn's own Abseil pin for both Dawn and Aurora in this build. The separate
# host-only presets keep their existing direct Abseil provider.
set(MSCHARGED_ABSEIL_PREPARED "${MSCHARGED_DAWN_PREPARED}/third_party/abseil-cpp")

if(APPLE)
    # Dawn configures its Objective-C++ Metal sources before Aurora.
    enable_language(OBJC OBJCXX)
endif()

function(mscharged_add_dawn)
    set(CMAKE_CXX_STANDARD 20)
    set(BUILD_SHARED_LIBS OFF)
    set(DAWN_FETCH_DEPENDENCIES OFF)
    set(DAWN_ENABLE_INSTALL OFF)
    set(DAWN_BUILD_SAMPLES OFF)
    set(DAWN_BUILD_TESTS OFF)
    set(DAWN_BUILD_BENCHMARKS OFF)
    set(DAWN_BUILD_NODE_BINDINGS OFF)
    set(DAWN_BUILD_PROTOBUF OFF)
    set(DAWN_BUILD_FUZZERS OFF)
    set(DAWN_USE_GLFW OFF)
    if(APPLE)
        set(CMAKE_OBJCXX_STANDARD 20)
        set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
        set(DAWN_USE_WAYLAND OFF)
        set(DAWN_USE_X11 OFF)
        set(DAWN_ENABLE_VULKAN OFF)
        set(DAWN_ENABLE_METAL ON)
        set(DAWN_ENABLE_SPIRV_VALIDATION OFF)
    elseif(WIN32)
        # Windows starts with Vulkan, which cross-compiles with LLVM-MinGW.
        # Desktop HWND surfaces only: UWP CoreWindow/XAML need WinRT headers
        # that MinGW ships broken.
        set(DAWN_USE_WINDOWS_UI OFF)
        set(DAWN_USE_WAYLAND OFF)
        set(DAWN_USE_X11 OFF)
        set(DAWN_ENABLE_VULKAN ON)
        set(DAWN_ENABLE_METAL OFF)
    else()
        set(DAWN_USE_WAYLAND ON)
        set(DAWN_USE_X11 ON)
        set(DAWN_ENABLE_VULKAN ON)
        set(DAWN_ENABLE_METAL OFF)
    endif()
    set(DAWN_ENABLE_NULL OFF)
    set(DAWN_ENABLE_D3D11 OFF)
    set(DAWN_ENABLE_D3D12 OFF)
    set(DAWN_ENABLE_DESKTOP_GL OFF)
    set(DAWN_ENABLE_OPENGLES OFF)
    set(DAWN_ENABLE_WEBGPU_ON_WEBGPU OFF)
    set(DAWN_ENABLE_SWIFTSHADER OFF)
    set(DAWN_BUILD_MONOLITHIC_LIBRARY STATIC)
    set(DAWN_VERSION_FILE "${CMAKE_CURRENT_SOURCE_DIR}/patches/dawn/base")
    set(TINT_BUILD_CMD_TOOLS OFF)
    set(TINT_BUILD_TESTS OFF)
    set(TINT_BUILD_BENCHMARKS OFF)
    set(TINT_BUILD_FUZZERS OFF)
    set(TINT_BUILD_IR_BINARY OFF)
    set(TINT_BUILD_GLSL_WRITER OFF)
    set(TINT_BUILD_GLSL_VALIDATOR OFF)
    set(TINT_BUILD_HLSL_WRITER OFF)
    set(TINT_BUILD_MSL_WRITER ${DAWN_ENABLE_METAL})
    set(TINT_BUILD_SPV_READER OFF)
    set(TINT_BUILD_SPV_WRITER ${DAWN_ENABLE_VULKAN})
    set(TINT_BUILD_WGSL_READER ON)
    set(TINT_BUILD_WGSL_WRITER ON)
    set(TINT_BUILD_TINTD OFF)
    set(TINT_BUILD_MESA OFF)
    add_subdirectory("${MSCHARGED_DAWN_PREPARED}" "${CMAKE_CURRENT_BINARY_DIR}/extern/dawn" EXCLUDE_FROM_ALL)
    if(WIN32 AND MINGW)
        # MSVC-isms in Windows-only sources: std::getenv without <cstdlib>,
        # and abseil's tz reader opening files with O_NONBLOCK.
        if(TARGET tint_utils_system)
            target_compile_options(tint_utils_system PRIVATE -include cstdlib)
        endif()
        if(TARGET absl_time_zone)
            target_compile_definitions(absl_time_zone PRIVATE O_NONBLOCK=0)
        endif()
    endif()
    # Aurora's core compiles its backend selection using the same flags.
    foreach(backend IN ITEMS VULKAN NULL D3D11 D3D12 METAL DESKTOP_GL OPENGLES WEBGPU_ON_WEBGPU)
        set(DAWN_ENABLE_${backend} "${DAWN_ENABLE_${backend}}" PARENT_SCOPE)
    endforeach()
endfunction()
