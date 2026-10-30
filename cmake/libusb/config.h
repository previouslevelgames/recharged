/* libusb build configuration of the static copy that cmake/LibUSB.cmake builds
   on Windows (MinGW Clang) and macOS (Apple Clang). It replaces the config.h
   that libusb's configure script writes. */
#pragma once

#define ENABLE_LOGGING 1
#define DEFAULT_VISIBILITY
#define PRINTF_FORMAT(a, b) __attribute__((__format__(__printf__, a, b)))

#if defined(_WIN32)
#define PLATFORM_WINDOWS 1
#define HAVE_STRUCT_TIMESPEC 1
#else
#define PLATFORM_POSIX 1
#define HAVE_IOKIT_USB_IOUSBHOSTFAMILYDEFINITIONS_H 1
#define HAVE_NFDS_T 1
#define HAVE_PTHREAD_THREADID_NP 1
#define HAVE_SYS_TIME_H 1
#define _GNU_SOURCE 1
#endif
