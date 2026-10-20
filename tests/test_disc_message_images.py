"""Disc-message images read from a synthetic disc's main.dol through nod.

The executable and images are synthetic; no retail bytes are involved."""
import hashlib
import pathlib
import struct
import subprocess
import sys
import tempfile

from disc_fixture import write_disc

LANGUAGES = ("eng", "fre", "deu", "spa", "ita", "jpn", "uke")
TYPES = ("FatalErrorImage", "NoDiskImage", "WrongDiskImage", "RetryImage", "Loading")
TABLE = 0x80300010
USA_TABLE = 0x804E70E0  # the USA release's table: a modified main.dol keeps it there


def pack(raw):
    """The game's packing: zero runs as (0, count); the last byte as is."""
    packed, i = bytearray(), 0
    while i + 1 < len(raw):
        if raw[i] == 0:
            run = 0
            while i + run + 1 < len(raw) and raw[i + run] == 0 and run < 255:
                run += 1
            packed += bytes((0, run))
            i += run
        else:
            packed.append(raw[i])
            i += 1
    packed.append(raw[-1])
    return bytes(packed)


def make_dol(table_address=TABLE):
    images = []
    for index in range(35):
        width, height = 8 * (1 + index % 3), 2 + index % 4
        raw = bytes((index + 1) if (n + index) % 3 == 0 else 0 for n in range(width * height // 8))
        images.append((width, height, pack(raw)))
    blob = bytearray()
    addresses = []
    for _, _, data in images:
        addresses.append(0x80100000 + len(blob))
        blob += data + b"\0" * (-len(data) % 4)
    table = bytearray(16)  # the table starts 16 bytes into its section
    for (width, height, data), address in zip(images, addresses):
        table += struct.pack(">4I", width, height, len(data), address)
    text = b"\x60\x00\x00\x00" * 8
    dol = bytearray(0x100)
    sections = ((0, 0x100, 0x80004000, text), (7, 0x100 + len(text), 0x80100000, blob),
                (8, 0x100 + len(text) + len(blob), table_address - 16, table))
    for index, offset, address, payload in sections:
        struct.pack_into(">I", dol, index * 4, offset)
        struct.pack_into(">I", dol, 0x48 + index * 4, address)
        struct.pack_into(">I", dol, 0x90 + index * 4, len(payload))
    struct.pack_into(">I", dol, 0xE0, 0x80004000)
    for _, _, _, payload in sections:
        dol += payload
    return bytes(dol), images


def run(executable):
    dol, images = make_dol()
    digest = hashlib.sha1(dol).hexdigest()
    checks = 0
    with tempfile.TemporaryDirectory(prefix="mscharged-discmsg-") as directory:
        image = pathlib.Path(directory) / "discmsg.iso"
        write_disc(image, dol=dol)

        result = subprocess.run([executable, "--disc", str(image), "--sha1", digest, "--table", f"{TABLE:08x}"],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        lines = result.stdout.splitlines()
        assert lines[0] == f"main.dol {len(dol)} bytes sha1 {digest}", lines[0]
        assert len(lines) == 36, result.stdout
        for index, ((width, height, data), line) in enumerate(zip(images, lines[1:])):
            name = f"HEADER_{TYPES[index % 5]}_{LANGUAGES[index // 5]}"
            expected = f"{index} {name} {width}x{height} {len(data)} {hashlib.sha1(data).hexdigest()}"
            assert line == expected, (line, expected)
            checks += 1

        # Another executable, or a table where there is none.
        for arguments, message in ((["--sha1", "0" * 40, "--table", f"{TABLE:08x}"], "SHA-1"),
                                   (["--sha1", digest, "--table", "80004000"], "not in a data section")):
            result = subprocess.run([executable, "--disc", str(image)] + arguments,
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode != 0 and message in result.stderr, (arguments, result.stderr)
            checks += 1

        # A modified main.dol (a Wiimmfi-patched image, say) with the images
        # where the release keeps them: they are used.
        dol, images = make_dol(USA_TABLE)
        write_disc(image, dol=dol)
        result = subprocess.run([executable, "--disc", str(image), "--release", "R4QE01", "1"],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        lines = result.stdout.splitlines()
        assert lines[1] == "modified main.dol" and len(lines) == 37, result.stdout
        for index, ((width, height, data), line) in enumerate(zip(images, lines[2:])):
            assert line.endswith(f" {width}x{height} {len(data)} {hashlib.sha1(data).hexdigest()}"), line
        checks += 1

        # The plain fixture's stub main.dol has no images: none, not an error.
        write_disc(image)
        result = subprocess.run([executable, "--disc", str(image), "--release", "R4QE01", "1"],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stderr
        lines = result.stdout.splitlines()
        assert lines[1:] == ["modified main.dol", "no images: The disc-message table 0x804E70E0 is not in a data section"], \
            result.stdout
        checks += 1
    print(f"disc_message_images_disc: {checks} checks through nod")


if __name__ == "__main__":
    run(sys.argv[1])
