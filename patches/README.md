# Source patches

Recharged never edits its dependencies directly. Every change to upstream source
(the decompilation, Aurora, Dawn, nod, SDL) is a patch in this folder, applied
in the order of each folder's `series` file to a copy of the pinned source
during the build. Each patch explains its reason in its header.

The game itself has two series: `mscharged-decomp/` only adapts the original
code to modern compilers and native hardware, while
`mscharged-decomp-tweaks/` holds every intended change (the optional tweaks and
the listed bug fixes). The [architecture](../docs/ARCHITECTURE.md#from-the-decompilation-to-the-program)
document describes how the build uses them.

A folder named `<dependency>-switch/` (such as `aurora-switch/`) holds changes
for the Nintendo Switch build only. They apply after the dependency's other
series, and only when the build targets the Switch: the PC builds never see
them.

For a local experiment, a series of your own can live in any folder (with its
`base` and `series` files and the patches beside them, as here). List it in the
CMake cache variable `MSCHARGED_EXTRA_PATCH_SERIES` as `DEPENDENCY=FOLDER`,
for example `-DMSCHARGED_EXTRA_PATCH_SERIES=mscharged-decomp=/path/to/series`
(several entries separated by `;`): it applies after all of that dependency's
series here. Releases and test builds never use one.
