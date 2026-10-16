# Source patches

The port never edits its dependencies directly. Every change to upstream source
(the decompilation, Aurora, Dawn, nod, SDL) is a patch in this folder, applied
in the order of each folder's `series` file to a copy of the pinned source
during the build. Each patch explains its reason in its header.

The game itself has two series: `mscharged-decomp/` only adapts the original
code to modern compilers and native hardware, while
`mscharged-decomp-tweaks/` holds every intended change (the optional tweaks and
the listed bug fixes). The [architecture](../docs/ARCHITECTURE.md#from-the-decompilation-to-the-program)
document describes how the build uses them.
