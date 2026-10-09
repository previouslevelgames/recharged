include_guard(GLOBAL)

# Wii Remote / DolphinBar HID detection shared by the launcher and the native
# Wii Remote driver; needs only SDL's HID API.
add_library(charged_wiimote_scan STATIC src/platform/wiimote_scan.cpp)
target_include_directories(charged_wiimote_scan PUBLIC src)
target_compile_features(charged_wiimote_scan PRIVATE cxx_std_20)
target_link_libraries(charged_wiimote_scan PUBLIC SDL3::SDL3)
