# Native port strategy

The foundation is the reconstructed retail C/C++ source from
[mscharged-decomp](https://github.com/yannicksuter/mscharged-decomp), compiled
for modern systems. We preserve the original gameplay, timing, and presentation.
This is a native source port; it does not execute or recompile the PowerPC binary.

## Source workflow

1. **Pin the source.** The decompilation, Aurora, and other dependencies are
   submodules at specific commits. New upstream commits only enter the port
   through an explicit, reviewed update.
2. **Copy, then patch.** Before patching, the build exports an exact copy of the
   decomp's complete `include/`, `libs/`, and `src/` trees. Ordered patches apply
   to this generated copy, leaving submodule checkouts unchanged.
3. **Format for reading.** clang-format lays out the compiled copy consistently.
   Whitespace-only changes are verified token by token; layout-dependent files
   (`__LINE__`, stringified arguments) are left exactly as patched.
4. **Compile the game source natively.** Original game and engine translation
   units remain the authoritative implementation.

## Original game, native platform

The playable port must start at the original `Game/main.cpp::main(...)` and run
the original flow throughout startup, menus, resource loading, animation,
gameplay, rendering calls, audio, input processing, and shutdown.

Native adapters provide the underlying graphics, audio, controller, file,
memory, clock, and window services. Compatibility patches handle Wii hardware
interfaces, modern compiler differences, pointer widths, and serialized data
layouts. Original code still decides what to load, update, display, and play.

This phase preserves retail behavior, including bugs and quirks. Suspected
reconstruction errors are investigated upstream. Intentional changes and
moddability come after a faithful playable foundation.

## Current boundary

The decompilation is unfinished, and full original startup and game execution
are still being integrated. Selected-source checks and scene previews validate
parts of the native platform; they do not establish a playable game. Missing
source or services remain explicit rather than being bypassed.

See [building](BUILDING.md), [runtime checks](RUNTIME.md), and the
[patch workflow](../patches/README.md).
