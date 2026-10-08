#if !defined(MSCHARGED_NATIVE) || !defined(MSCHARGED_GAME_MODULE)
#error Original entry export belongs only to the isolated source game module
#endif

// Preserve the exact original source main body and its C ABI symbol. The host
// invokes this export after native setup; all game flow remains source-owned.
// The source TU's unchanged hidden main symbol is the entry itself. This
// integration export retains and calls its compiled body, not a copied startup.
// Explicit assembly labels use the compiler's real external-symbol prefix.
// ELF main has no prefix; Mach-O's original C main is _main.
#define MSCHARGED_ENTRY_LABEL_IMPL(value) #value
#define MSCHARGED_ENTRY_LABEL(value) MSCHARGED_ENTRY_LABEL_IMPL(value)
extern "C" __attribute__((visibility("hidden"))) int OriginalMain()
    __asm__(MSCHARGED_ENTRY_LABEL(__USER_LABEL_PREFIX__) "main");
#undef MSCHARGED_ENTRY_LABEL
#undef MSCHARGED_ENTRY_LABEL_IMPL
extern "C" __attribute__((visibility("default"))) int charged_original_entry() {
    return OriginalMain();
}
