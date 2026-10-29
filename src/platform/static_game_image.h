#pragma once
#include <cstddef>

// A game module linked into the program as a static game image
// (tools/link_static_game_image.py). The program's build writes one table of
// them (cmake/StaticGameImages.cmake); native_module_loader.cpp loads them by
// the module file name the host asks for.
struct ChargedStaticGameExport {
    const char* name;
    // Null when the image does not define it, as dlsym would answer.
    void* address;
};
struct ChargedStaticGameRange {
    const char* begin;
    const char* end;
};
struct ChargedStaticGameImage {
    const char* name;
    const char* file_name;
    const ChargedStaticGameExport* exports;
    std::size_t export_count;
    // The image's constructors, priority-sorted as a dynamic loader runs them.
    void (*const* init_begin)();
    void (*const* init_end)();
    // Code, read-only data, data and zeroed data.
    ChargedStaticGameRange ranges[4];
};
