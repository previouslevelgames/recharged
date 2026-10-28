#!/usr/bin/env python3
"""Compiler launcher of the Windows and macOS game modules: the rollback's sections.

On ELF, cmake/rollback_sections.ld gathers the writable data of the original
game's code in sections of their own, which a rollback saves with the arenas.
PE and Mach-O linkers take no such script. So this launcher runs the compile
command it is given, then, when the source is one of the original game's files
the script gathers, renames the object's writable data and zeroed data
sections, the static locals' guard variables among them:

PE: .data, .bss and their "$" variants (one by variable) to .rbkd$m and
.rbkb$m. The linker merges the sections named "<name>$<x>" into one section
<name>, in the order of <x>: src/platform/rollback_platform.cpp puts the
bounds, __rbk_data_start and the others, in the "$a" and "$z" sections
around them.

Mach-O: __DATA,__data, __DATA,__bss and __DATA,__common to __DATA,__rbkd,
__DATA,__rbkb and __DATA,__rbkc. The compiler puts the zeroed globals in
__bss (internal ones, the guard variables among them) and __common (external
ones): zero fill sections, not common symbols, whose flag the new names keep;
two names, so that an object holds no two sections of one name. The linker
gathers the sections of
one name and defines their bounds itself, section$start$__DATA$__rbkd and the
others (src/platform/rollback_state.h; __rbkc is a range of its own). Common symbols (C's tentative
definitions under -fcommon) have no section to rename: an object with one is
refused.

The files are the linker script's: the launcher reads them from it.

Usage: rollback_sections.py <compile command...>
"""

import fnmatch
import os
import re
import struct
import subprocess
import sys

LINKER_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "cmake", "rollback_sections.ld")


def gathered(script, section):
    """The (files, excluded files) patterns a section of the linker script gathers."""
    body = re.search(section + r"\s*(?:\(NOLOAD\))?\s*:\s*\{(.*?)\n    \}", script, re.S).group(1)
    rules = []
    for match in re.finditer(r"(?:EXCLUDE_FILE\(([^)]*)\)\s*)?(\*src/[^(\s]+)\(", body):
        rules.append((match.group(2), tuple((match.group(1) or "").split())))
    return rules


def load_rules():
    with open(LINKER_SCRIPT) as file:
        script = file.read()
    data, bss = gathered(script, "rbk_data"), gathered(script, "rbk_bss")
    if not data or data != bss:
        raise SystemExit(f"rollback_sections.py: rbk_data and rbk_bss of {LINKER_SCRIPT} gather different files")
    return data


def matches(path, patterns):
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in patterns)


def gathered_source(source):
    """Whether the linker script gathers the globals of this source file."""
    path = os.path.abspath(source).replace(os.sep, "/")
    return any(fnmatch.fnmatchcase(path, files) and not matches(path, excluded) for files, excluded in load_rules())


MACHO_MAGIC_64 = 0xFEEDFACF
MACHO_FAT_MAGIC = 0xCAFEBABE
LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2
MACHO_NAMES = {b"__data": b"__rbkd", b"__bss": b"__rbkb", b"__common": b"__rbkc"}


def rename_macho(image, base, path):
    """Renames the sections of the thin 64-bit Mach-O object at `base`; True when it changed."""
    magic, = struct.unpack_from("<I", image, base)
    if magic != MACHO_MAGIC_64:
        raise SystemExit(f"rollback_sections.py: {path} is not a 64-bit little-endian Mach-O object")
    commands, = struct.unpack_from("<I", image, base + 16)
    at, changed = base + 32, False
    for _ in range(commands):
        command, size = struct.unpack_from("<II", image, at)
        if command == LC_SEGMENT_64:
            sections, = struct.unpack_from("<I", image, at + 64)
            for index in range(sections):
                section = at + 72 + index * 80
                name = bytes(image[section:section + 16]).rstrip(b"\0")
                segment = bytes(image[section + 16:section + 32]).rstrip(b"\0")
                if segment == b"__DATA" and name in MACHO_NAMES:
                    image[section:section + 16] = MACHO_NAMES[name].ljust(16, b"\0")
                    changed = True
        elif command == LC_SYMTAB:
            symbols_at, symbols, strings_at, _ = struct.unpack_from("<IIII", image, at + 8)
            for index in range(symbols):
                strx, kind, _, _, value = struct.unpack_from("<IBBHQ", image, base + symbols_at + index * 16)
                # External, undefined, not a debugging entry, with a size: common.
                if kind & 0xE0 == 0 and kind & 0x0E == 0 and kind & 0x01 and value:
                    end = image.index(b"\0", base + strings_at + strx)
                    name = bytes(image[base + strings_at + strx:end]).decode(errors="replace")
                    raise SystemExit(f"rollback_sections.py: {path} has the common symbol {name}, "
                                     "which no section of the rollback holds (compile with -fno-common)")
        at += size
    return changed


def rename_macho_sections(path):
    with open(path, "r+b") as file:
        image = bytearray(file.read())
        magic, = struct.unpack_from(">I", image, 0)
        if magic == MACHO_FAT_MAGIC:
            # A universal object: every architecture's object, at its offset.
            count, = struct.unpack_from(">I", image, 4)
            changed = False
            for index in range(count):
                offset, = struct.unpack_from(">I", image, 8 + index * 20 + 8)
                changed = rename_macho(image, offset, path) or changed
        else:
            changed = rename_macho(image, 0, path)
        if changed:
            file.seek(0)
            file.write(image)


def is_macho(path):
    with open(path, "rb") as file:
        head = file.read(4)
    return len(head) == 4 and (struct.unpack("<I", head)[0] == MACHO_MAGIC_64
                               or struct.unpack(">I", head)[0] == MACHO_FAT_MAGIC)


def rename_sections(path, data_name=b".rbkd$m", bss_name=b".rbkb$m"):
    if is_macho(path):
        rename_macho_sections(path)
        return
    with open(path, "r+b") as file:
        image = bytearray(file.read())
        # A big object (more than 65279 sections) has a header of its own.
        if image[0:4] == b"\x00\x00\xff\xff" and struct.unpack_from("<H", image, 4)[0] >= 2:
            sections, symbols_at, symbols = struct.unpack_from("<III", image, 44)
            table, symbol_size = 56, 20
        else:
            sections, = struct.unpack_from("<H", image, 2)
            symbols_at, symbols, optional = struct.unpack_from("<IIH", image, 8)
            table, symbol_size = 20 + optional, 18
        strings = symbols_at + symbols * symbol_size
        changed = False
        for index in range(sections):
            at = table + index * 40
            raw = bytes(image[at:at + 8])
            if raw.startswith(b"/"):
                offset = int(raw[1:].rstrip(b"\0").decode())
                end = image.index(b"\0", strings + offset)
                name = bytes(image[strings + offset:end])
            else:
                name = raw.rstrip(b"\0")
            if name in (b".data", b".bss") or name.startswith((b".data$", b".bss$")):
                new = data_name if name.startswith(b".data") else bss_name
                image[at:at + 8] = new.ljust(8, b"\0")
                changed = True
        if changed:
            file.seek(0)
            file.write(image)


def main(command):
    result = subprocess.run(command)
    if result.returncode:
        return result.returncode
    source = output = None
    for index, argument in enumerate(command):
        following = command[index + 1] if index + 1 < len(command) else None
        if argument == "-c" and following:
            source = following
        elif argument == "-o" and following:
            output = following
        elif argument.startswith("-o") and len(argument) > 2:
            output = argument[2:]
        elif argument.endswith((".c", ".cc", ".cpp", ".cxx")) and not argument.startswith("-"):
            source = source or argument
    if source and gathered_source(source):
        # A game file whose object is not found would keep its globals out of
        # the rollback without a word.
        if not output or not os.path.isfile(output):
            print(f"rollback_sections.py: no object for {source} in the compile command", file=sys.stderr)
            return 1
        rename_sections(output)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
