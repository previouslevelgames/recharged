#!/bin/sh
# Packs a mesa-switch build as one libvulkan.a object, inside the driver's
# build image (tools/switch/build-nvk.sh). Usage: package-nvk.sh <mesa> <out>
set -e

MESA="$1"
OUT="$2"
B="$MESA/builddir-switch"
D=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
LIBS=/opt/devkitpro/portlibs/switch/lib

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT INT TERM
cd "$W"

# The whole driver; the other archives the build makes are its parts.
cp "$B/src/nouveau/vulkan/libvulkan.a" mesa-vulkan.a
# vulkan-dispatch.o is the driver already linked from members the archive also
# keeps, so it goes in first and alone.
"$D-ar" x mesa-vulkan.a vulkan-dispatch.o
"$D-ar" d mesa-vulkan.a vulkan-dispatch.o
# Its index lacks the Rust objects' symbols (core::panicking::*).
"$D-ranlib" mesa-vulkan.a
INPUTS="vulkan-dispatch.o mesa-vulkan.a $LIBS/libexpat.a $LIBS/libzstd.a"

"$D-nm" -g --defined-only $INPUTS 2>/dev/null | awk 'NF==3{print $3}' | sort -u > archdef.txt
"$D-nm" -g --defined-only mesa-vulkan.a 2>/dev/null \
    | awk '$2 ~ /^[TD]$/ && $3 ~ /^vk/ {print $3}' | sort -u > seeds.txt

# Weak references pull in nothing, so weakly named implementations are seeded
# until none is missing.
pass=0
while :; do
    pass=$((pass + 1))
    sed 's/^/-u /' seeds.txt > seed.args
    rm -f nvk.o
    "$D-ld" -r -o nvk.o @seed.args $INPUTS
    "$D-nm" -u nvk.o | awk '$1=="w"{print $2}' | sort -u > weakundef.txt
    comm -12 weakundef.txt archdef.txt > dropped.txt
    n=$(wc -l < dropped.txt | tr -d ' ')
    echo "==> pass $pass: $n implementation(s) still left out"
    [ "$n" -eq 0 ] && break
    if [ "$pass" -ge 8 ]; then
        echo "package-nvk.sh: no closure after $pass passes" >&2
        exit 1
    fi
    cat dropped.txt >> seeds.txt
    sort -u seeds.txt -o seeds.txt
done

# Only vk* stays global, or the driver's Mesa collides with the older Mesa in
# devkitPro's libEGL.a.
"$D-nm" -g --defined-only nvk.o | awk 'NF==3 && $3 ~ /^vk/ {print $3}' | sort -u > keep.txt
# devkitA64's newlib declares flockfile and funlockfile, but only libsysbase's
# __-prefixed pair exists.
"$D-objcopy" --keep-global-symbols=keep.txt \
    --redefine-sym flockfile=__flockfile --redefine-sym funlockfile=__funlockfile nvk.o nvk-hidden.o

mkdir -p "$OUT/lib" "$OUT/include"
"$D-ar" rcs "$OUT/lib/libvulkan.a" nvk-hidden.o
cp -R "$MESA/include/vulkan" "$OUT/include/vulkan"

cat > "$OUT/NOTICE.md" <<EOF
# Vulkan driver

\`lib/libvulkan.a\` is a build of third-party open-source software, partially
linked into one object with everything but the vk* symbols hidden.

## Mesa 3D: the NVK Vulkan driver and runtime, with a Horizon port

Mesa $(cat "$MESA/VERSION") from https://github.com/danfromtico/mesa-switch at
${MESA_REF:-an unrecorded commit}, with the patches in patches/mesa-switch.
MIT, as upstream Mesa; individual components carry their own licenses (see
licenses/ and the file headers there).

## Expat and zstd

devkitPro's switch-libexpat and switch-zstd portlibs: Mesa reads its driver
configuration with expat and compresses its shader cache with zstd. MIT and BSD.

## Rust

NVK's shader compiler is written in Rust; the Rust runtime it needs is linked
in. Its crates come through Mesa's meson wraps under MIT or Apache-2.0.
EOF

if [ -n "$OWNER" ]; then
    chown -R "$OWNER" "$OUT"
fi
echo "==> $OUT/lib/libvulkan.a: $(wc -c < "$OUT/lib/libvulkan.a" | tr -d ' ') bytes, $(wc -l < keep.txt | tr -d ' ') global vk* symbols"
