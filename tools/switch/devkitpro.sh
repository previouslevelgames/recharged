#!/bin/sh
# Runs a command in the Switch build image, the pinned devkitPro toolchain image
# (tools/switch/toolchain.env) with a few host tools (tools/switch/Dockerfile),
# as the current user, in the current directory.
# The checkout (and its Git directory, for source preparation) is mounted at
# the same path, so build directories and logs work inside and outside.
#
#   tools/switch/devkitpro.sh cmake --workflow --preset switch
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
. "$here/toolchain.env"
git_dir=$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)
mounts="-v $root:$root"
case "$git_dir" in
    "$root"/*) ;;
    *) mounts="$mounts -v $git_dir:$git_dir" ;;
esac
image="recharged-devkitpro:$(printf '%s\n' "$DEVKITPRO_IMAGE" | cat - "$here/Dockerfile" | sha256sum | cut -c1-12)"
if ! docker image inspect "$image" >/dev/null 2>&1; then
    docker build -q -t "$image" --build-arg DEVKITPRO_IMAGE="$DEVKITPRO_IMAGE" "$here" >&2
fi
tty=
[ -t 0 ] && [ -t 1 ] && tty=-t
mkdir -p "$root/build/.devkitpro-home"
# shellcheck disable=SC2086
exec docker run --rm -i $tty -u "$(id -u):$(id -g)" $mounts -w "$PWD" \
    -e HOME="$root/build/.devkitpro-home" -e DEVKITPRO=/opt/devkitpro \
    -e PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/usr/local/bin:/usr/bin:/bin \
    "$image" "$@"
