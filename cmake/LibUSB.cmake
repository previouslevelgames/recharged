# libusb for SDL's HIDAPI joystick drivers. SDL reaches the GameCube controller
# adapter (WUP-028) and the Switch 2 controllers only through libusb, and
# ignores them without it. libusb has no CMake build, so this lists its sources.
#
# Windows and macOS link the pinned extern/libusb statically. Linux keeps SDL's
# own detection: with the libusb development package installed at build time,
# SDL loads the system libusb-1.0.so.0 at run time, so the Steam Deck package
# links no new library. The checkout is prepared on every platform, so every
# package carries its license notice.
mscharged_prepare_dependency(libusb MSCHARGED_LIBUSB_PREPARED)

if(WIN32 OR APPLE)
    set(libusb_root "${MSCHARGED_LIBUSB_PREPARED}/libusb")
    add_library(usb-1.0 STATIC
        "${libusb_root}/core.c"
        "${libusb_root}/descriptor.c"
        "${libusb_root}/hotplug.c"
        "${libusb_root}/io.c"
        "${libusb_root}/strerror.c"
        "${libusb_root}/sync.c")
    if(WIN32)
        target_sources(usb-1.0 PRIVATE
            "${libusb_root}/os/events_windows.c"
            "${libusb_root}/os/threads_windows.c"
            "${libusb_root}/os/windows_common.c"
            "${libusb_root}/os/windows_usbdk.c"
            "${libusb_root}/os/windows_winusb.c")
    else()
        target_sources(usb-1.0 PRIVATE
            "${libusb_root}/os/events_posix.c"
            "${libusb_root}/os/threads_posix.c"
            "${libusb_root}/os/darwin_usb.c")
        # Security: the Darwin backend reads the process's entitlements.
        target_link_libraries(usb-1.0 PRIVATE "-framework IOKit" "-framework CoreFoundation"
            "-framework Security")
    endif()
    target_include_directories(usb-1.0
        PUBLIC "${libusb_root}"
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/libusb" "${libusb_root}/os")
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        # Upstream's Windows headers pack structures through pshpack1.h.
        target_compile_options(usb-1.0 PRIVATE -Wno-pragma-pack)
    endif()
    add_dependencies(usb-1.0 verify_libusb)
    add_library(LibUSB::LibUSB ALIAS usb-1.0)
    # SDL exports its targets into its build tree even without installing
    # them, and its static library links this one.
    export(TARGETS usb-1.0 FILE "${CMAKE_CURRENT_BINARY_DIR}/extern/libusb/LibUSBTargets.cmake")

    # SDL's FindLibUSB looks for an installed copy. These answers stand in for
    # its searches: the alias takes the place of the imported target it would
    # make, and its link probe is answered here because the archive exists only
    # at build time.
    set(LibUSB_INCLUDE_PATH "${libusb_root}" CACHE PATH "libusb headers (cmake/LibUSB.cmake)" FORCE)
    set(LibUSB_LIBRARY usb-1.0 CACHE STRING "libusb target (cmake/LibUSB.cmake)" FORCE)
    set(HAVE_LIBUSB_H 1 CACHE INTERNAL "")
    set(MSCHARGED_SDL_LIBUSB_STATIC ON)
endif()
