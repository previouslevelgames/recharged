#!/usr/bin/env python3
"""Link a game module as a static game image: one relocatable object for the program.

Runs as the game module target's CXX_LINKER_LAUNCHER (cmake/StaticGameImages.cmake).
The module's own link command follows `--`. Its objects, archives and export map are
linked with `ld -r` instead of into a shared library, for systems without dynamic
loading (Nintendo Switch). As with the module's `local: *` version script, everything
but the exports becomes local: the image keeps its own operator new/delete, the
original zlib, its inline and template copies, and never binds program symbols that
the module would not. Exports are renamed charged_image_<image>__<name>; the program
looks them up by their original names (src/platform/native_module_loader.cpp).

The image's code and data are gathered into charged_image_<image>_{text,rodata,data,bss}
so the program can tell the image's memory, and its constructors into
charged_image_<image>_init, which the program runs when it loads the image, after its
own setup, as dlopen would run the module's.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Sections the program sees as the image (code, read-only data, data, zeroed data).
RANGES = ("text", "rodata", "data", "bss")

LINKER_SCRIPT = """\
SECTIONS
{{
  charged_image_{image}_text : {{ *(.text .text.* .gnu.linkonce.t.*) }}
  charged_image_{image}_rodata : {{ *(.rodata .rodata.* .gnu.linkonce.r.*) }}
  charged_image_{image}_data : {{ *(.data .data.* .gnu.linkonce.d.*) }}
  charged_image_{image}_bss : {{ *(.bss .bss.* .gnu.linkonce.b.* COMMON) }}
  charged_image_{image}_init : {{ KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*))) KEEP(*(.init_array)) }}
  charged_image_{image}_fini : {{ KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*))) KEEP(*(.fini_array)) }}
  /* A relocatable link with --gc-sections drops the unwind tables otherwise. */
  .eh_frame : {{ KEEP(*(.eh_frame)) }}
}}
"""

# Sections that may stay as they are in the image (besides the gathered ones).
ALLOWED_SECTION = re.compile(
    r"^(\.eh_frame|\.gcc_except_table(\..*)?|\.tdata(\..*)?|\.tbss(\..*)?|\.note(\..*)?|\.comment"
    r"|\.debug_.*|\.zdebug_.*|\.symtab|\.strtab|\.shstrtab|\.rela?\..*|\.group)$")
FORBIDDEN_SECTION = re.compile(r"^\.(ctors|dtors|preinit_array)(\..*)?$")
PROVIDER = re.compile(r"^\s*CHARGED_MISSING_PROVIDER\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)", re.M)


class ImageError(RuntimeError):
    pass


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise ImageError(f"{Path(command[0]).name} failed ({result.returncode}):\n"
                         f"{result.stderr.strip() or result.stdout.strip()}")
    return result.stdout


def parse_link_command(command):
    """The module link's output, inputs, export map and dependency file."""
    output = version_script = dependency_file = None
    inputs = []

    def linker_option(option):
        nonlocal version_script, dependency_file
        if option.startswith("--version-script="):
            version_script = option.split("=", 1)[1]
        elif option.startswith("--dependency-file="):
            dependency_file = option.split("=", 1)[1]
        elif option in ("--whole-archive", "--no-whole-archive", "--start-group", "--end-group"):
            inputs.append(option)
        # Other options belong to the shared-library link (-Bsymbolic-functions,
        # --gc-sections, -soname) and have no meaning for a relocatable image.

    arguments = iter(command[1:])
    for argument in arguments:
        if argument == "-o":
            output = next(arguments)
        elif argument.startswith("-Wl,"):
            for option in argument[4:].split(","):
                linker_option(option)
        elif argument == "-Xlinker":
            linker_option(next(arguments))
        elif argument.startswith("-"):
            continue  # compiler driver flags and system libraries (-l, -L)
        elif argument.endswith((".o", ".obj", ".a")):
            inputs.append(argument)
        elif re.search(r"\.so(\.[0-9]+)*$|\.dylib$|\.dll$", argument):
            raise ImageError(f"A static game image cannot contain the shared library {argument}")
        else:
            raise ImageError(f"Unexpected module link input: {argument}")
    if not output or not version_script:
        raise ImageError("The module link names no output or no export map (version script)")
    return output, inputs, version_script, dependency_file


def read_exports(version_script):
    text = Path(version_script).read_text()
    match = re.search(r"global:(.*?)local:\s*\*\s*;", text, re.S)
    if not match:
        raise ImageError(f"Export map has no explicit global/local scope: {version_script}")
    exports = [name.strip() for name in match.group(1).split(";") if name.strip()]
    for name in exports:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
            raise ImageError(f"Export map entry is not a plain symbol: {name}")
    return exports


def symbols(nm, path, defined):
    """Global (and weak) symbols of an object: defined ones or undefined ones."""
    output = run([nm, "-g", "--defined-only" if defined else "--undefined-only", path])
    return {line.split()[-1] for line in output.splitlines() if line.strip()}


def sections(readelf, path):
    """Each section's name and whether it is allocated (part of the program's memory)."""
    found = []
    for line in run([readelf, "-SW", path]).splitlines():
        match = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+[0-9a-f]+\s+[0-9a-f]+\s+[0-9a-f]+\s+[0-9a-f]+\s+(\S*)", line)
        if match:
            found.append((match.group(1), "A" in match.group(2)))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ld", required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--readelf", required=True)
    parser.add_argument("--image", required=True, help="image name, as in charged_image_<image>")
    parser.add_argument("--providers", type=Path,
                        help="the program's explicit providers for functions the game lacks")
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ["--"] else args.command
    try:
        if not re.fullmatch(r"[a-z][a-z0-9]*", args.image):
            raise ImageError(f"Invalid image name: {args.image}")
        output, inputs, version_script, dependency_file = parse_link_command(command)
        exports = read_exports(version_script)
        prefix = f"charged_image_{args.image}__"
        with tempfile.TemporaryDirectory(prefix="static-game-image-") as work:
            work = Path(work)
            # Which exports the module defines: like dlsym, an export the
            # module lacks is simply absent (null) for the program.
            everything = work / "everything.o"
            run([args.ld, "-r", "--force-group-allocation", "-o", str(everything), *inputs])
            defined = symbols(args.nm, str(everything), True)
            present = [name for name in exports if name in defined]
            if args.providers:
                provided = PROVIDER.findall(args.providers.read_text())
                own = sorted(name for name in provided if name in defined)
                if own:
                    raise ImageError(
                        f"The game now defines {', '.join(own)}; remove its provider from {args.providers}")

            script = work / "image.ld"
            script.write_text(LINKER_SCRIPT.format(image=args.image))
            linked = work / "linked.o"
            link = [args.ld, "-r", "--force-group-allocation", "--gc-sections", "-T", str(script)]
            for name in present:
                link += ["-u", name]
            if dependency_file:
                link.append(f"--dependency-file={dependency_file}")
            run(link + ["-o", str(linked), *inputs])

            renames = work / "renames.txt"
            renames.write_text("".join(f"{name} {prefix}{name}\n" for name in present))
            keep = work / "keep.txt"
            keep.write_text("".join(f"{prefix}{name}\n" for name in present))
            image = Path(output).with_name(Path(output).name + ".tmp")
            run([args.objcopy, f"--redefine-syms={renames}", f"--keep-global-symbols={keep}",
                 str(linked), str(image)])

        unexpected = []
        for name, allocated in sections(args.readelf, str(image)):
            if FORBIDDEN_SECTION.match(name):
                raise ImageError(f"Static game images do not run {name} sections")
            if name.startswith(f"charged_image_{args.image}_") or ALLOWED_SECTION.match(name):
                continue
            if allocated:
                unexpected.append(name)
        if unexpected:
            raise ImageError("Sections outside the image's code and data: " + ", ".join(sorted(unexpected)))
        left = symbols(args.nm, str(image), True) - {prefix + name for name in present}
        if left:
            raise ImageError("Symbols still global in the image: " + ", ".join(sorted(left)[:20]))
        os.replace(image, output)
    except (ImageError, OSError) as error:
        print(f"Static game image {args.image}: {error}", file=sys.stderr)
        return 1
    print(f"Static game image {args.image}: {len(present)} of {len(exports)} exports")
    return 0


if __name__ == "__main__":
    sys.exit(main())
