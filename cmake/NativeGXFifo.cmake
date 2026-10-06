include_guard(GLOBAL)

# Exercise the actual SDK descriptor operations without initializing a GPU.
# This qualifies native pointer/watermark transport and source exclusion only.
if(BUILD_TESTING AND TARGET aurora::gx)
    add_executable(native_gx_fifo_tests tests/native_gx_fifo.cpp)
    target_compile_features(native_gx_fifo_tests PRIVATE cxx_std_17)
    target_link_libraries(native_gx_fifo_tests PRIVATE
        aurora::gx charged_native_interrupts)
    add_test(NAME native_gx_fifo COMMAND native_gx_fifo_tests)
    set_tests_properties(native_gx_fifo PROPERTIES TIMEOUT 30)
endif()
