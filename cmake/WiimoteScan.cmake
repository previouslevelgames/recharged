include_guard(GLOBAL)

# Wii Remote / DolphinBar HID detection, camera helpers, keyboard and gamepad bindings
# and the gyro pointer, shared by the launcher and the native input drivers; needs only SDL.
add_library(charged_wiimote_scan STATIC src/platform/wiimote_scan.cpp src/platform/wiimote_ir.cpp
    src/platform/wiimote_bluetooth.cpp src/platform/key_bindings.cpp src/platform/gamepad_bindings.cpp
    src/platform/gyro_pointer.cpp)
target_include_directories(charged_wiimote_scan PUBLIC src)
target_compile_features(charged_wiimote_scan PRIVATE cxx_std_20)
target_link_libraries(charged_wiimote_scan PUBLIC SDL3::SDL3)

