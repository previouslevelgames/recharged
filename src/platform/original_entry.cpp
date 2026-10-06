#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Original entry export belongs only to the isolated source game module
#endif

// Preserve the exact original source main body and its C ABI symbol. The host
// invokes this export after native setup; all game flow remains source-owned.
// The source TU's unchanged hidden main symbol is the entry itself. This
// integration export retains and calls its compiled body, not a copied startup.
extern "C" __attribute__((visibility("hidden"))) int OriginalMain() __asm__("main");
extern "C" __attribute__((visibility("default"))) int charged_original_entry() {
    return OriginalMain();
}
