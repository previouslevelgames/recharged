#!/bin/sh
# Builds the NVK Vulkan driver for the Switch, as one libvulkan.a, from the
# pinned mesa-switch (extern/mesa-switch with patches/mesa-switch) in Docker.
# x86_64 hosts cross-compile NVK's Rust; arm64 hosts build it natively.
#
#   tools/switch/build-nvk.sh [dest]    (default build/nvk-switch)
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
here="$root/tools/switch"
dest="${1:-$root/build/nvk-switch}"
work="$root/build/nvk"
. "$here/toolchain.env"

sha256_stdin() { sha256sum | awk '{print $1}'; }

pin=$(git -C "$root" ls-files --stage extern/mesa-switch | awk '{print $2}')
stamp="$pin $(cat "$0" "$here/package-nvk.sh" "$here/toolchain.env" "$root"/patches/mesa-switch/* | sha256_stdin)"
if [ -f "$dest/lib/libvulkan.a" ] && [ "$(cat "$dest/.stamp" 2>/dev/null)" = "$stamp" ]; then
    echo "==> NVK already built in $dest"
    exit 0
fi

# The build writes into its tree as root inside its container.
remove_as_root() {
    [ -e "$1" ] || return 0
    docker run --rm -v "$(dirname "$1"):/w" "$DEVKITPRO_IMAGE" rm -rf "/w/$(basename "$1")"
}

echo "==> preparing mesa-switch $pin"
prepared=$(python3 -B "$root/tools/prepare_sources.py" --dependency mesa-switch --build-dir "$work")
src="$work/mesa"
remove_as_root "$src"
mkdir -p "$src"
cp -a "$prepared/." "$src/"

python3 - "$src/Docker.rust" "$DEVKITPRO_IMAGE" "$MESA_RUST_TOOLCHAIN" "$MESA_PY_TOOLS" "$MESA_CARGO_TOOLS" <<'PINS'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
for old, new in [
    ("FROM devkitpro/devkita64:latest", "FROM " + sys.argv[2]),
    ("--default-toolchain nightly &&", "--default-toolchain " + sys.argv[3] + " &&"),
    ("rustup component add rust-src", "rustup component add rust-src && rustup target add aarch64-unknown-linux-gnu"),
    ("pip3 install --break-system-packages meson mako", "pip3 install --break-system-packages " + sys.argv[4]),
    ("cargo install bindgen-cli cbindgen", "cargo install --locked " + sys.argv[5]),
]:
    if text.count(old) != 1:
        sys.exit("Unexpected Mesa Dockerfile: " + old)
    text = text.replace(old, new)
# NVK's Rust is built for AArch64 Linux and linked into the Horizon program.
path.write_text(text + "ENV MESA_SWITCH_RUST_TARGET=aarch64-unknown-linux-gnu\n")
PINS
image="recharged-mesa-rust:$(sha256_stdin < "$src/Docker.rust" | cut -c1-12)"
# NVK_DOCKER_RUN_OPTIONS (such as --cpus 6) reach the build container.
python3 - "$src/build-switch.sh" "$image" <<'IMAGE_NAME'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
for old, new in [
    ('IMAGE_NAME="devkitpro-mesa-rust"', 'IMAGE_NAME="' + sys.argv[2] + '"'),
    ('docker run -d --name "$CONTAINER_NAME"', 'docker run -d ${NVK_DOCKER_RUN_OPTIONS:-} --name "$CONTAINER_NAME"'),
]:
    if text.count(old) != 1:
        sys.exit("Unexpected Mesa build script: " + old)
    text = text.replace(old, new)
path.write_text(text)
IMAGE_NAME

echo "==> building Mesa"
(cd "$src" && bash ./build-switch.sh)
docker rm -f mesa-switch-build >/dev/null

echo "==> packaging into $dest"
remove_as_root "$dest"
mkdir -p "$dest"
docker run --rm -v "$src:/project:ro" -v "$here:/tools:ro" -v "$dest:/out" \
    -e MESA_REF="$pin" -e OWNER="$(id -u):$(id -g)" \
    "$image" sh /tools/package-nvk.sh /project /out
docker run --rm -v "$work:/w" "$image" chown -R "$(id -u):$(id -g)" /w/mesa
echo "$stamp" > "$dest/.stamp"
echo "==> ok"
