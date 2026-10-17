"""The image library (#65) with the real disc reader on synthetic discs; no retail data.

Runs the C++ checks, then probes, banners and a folder scan of made-up Wii
images: playable and unsupported Charged releases, another game, a damaged
image and a file that is no disc image at all."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

from disc_fixture import write_disc

IMAGE_SIZE = 40 << 20  # the library ignores files under 32 MiB


def banner_tpl(seed):
    """A TPL with one 192 x 64 RGB5A3 image of made-up texels."""
    header = bytearray(0x40)
    struct.pack_into(">III", header, 0, 0x0020AF30, 1, 0x0C)
    struct.pack_into(">II", header, 0x0C, 0x14, 0)
    struct.pack_into(">HHII", header, 0x14, 64, 192, 5, 0x40)
    texels = bytearray()
    for n in range(192 * 64):
        value = (n * 2654435761 + seed) & 0xFFFF
        texels += struct.pack(">H", value)
    return bytes(header + texels)


def decode(tpl):
    """An independent RGB5A3 decoder (bit replication, as the GPU expands it): RGBA rows, top first."""
    rgba = bytearray(192 * 64 * 4)
    offset = 0x40
    for tile_y in range(0, 64, 4):
        for tile_x in range(0, 192, 4):
            for y in range(4):
                for x in range(4):
                    (value,) = struct.unpack_from(">H", tpl, offset)
                    offset += 2
                    if value & 0x8000:
                        r, g, b = (value >> 10) & 31, (value >> 5) & 31, value & 31
                        pixel = [c << 3 | c >> 2 for c in (r, g, b)] + [255]
                    else:
                        a = (value >> 12) & 7
                        r, g, b = (value >> 8) & 15, (value >> 4) & 15, value & 15
                        pixel = [c * 17 for c in (r, g, b)] + [a << 5 | a << 2 | a >> 1]
                    start = ((tile_y + y) * 192 + tile_x + x) * 4
                    rgba[start:start + 4] = bytes(pixel)
    return bytes(rgba)


def disc(path, game_id, revision, banners):
    files = {"test.txt": b"Synthetic fixture data.\n"}
    files.update({f"Art/fe/{name}": data for name, data in banners.items()})
    path.parent.mkdir(parents=True, exist_ok=True)
    write_disc(path, game_id=game_id, revision=revision, files=files, partition_size=0x18000)
    os.truncate(path, IMAGE_SIZE)


def run(executable):
    result = subprocess.run([executable], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout + result.stderr
    with tempfile.TemporaryDirectory(prefix="recharged-library-") as directory:
        root = pathlib.Path(directory) / "Games"
        us, eu, jp = banner_tpl(1), banner_tpl(2), banner_tpl(3)
        # Every disc has every region's file; only its own region's is its artwork.
        disc(root / "Charged (USA).iso", b"R4QE01", 1, {"us_banner.tpl": us, "eu_banner.tpl": eu})
        disc(root / "Europe" / "Charged (Europe) (Rev 2).iso", b"R4QP01", 2, {"us_banner.tpl": us, "eu_banner.tpl": eu})
        disc(root / "Charged (Japan).iso", b"R4QJ01", 0, {"jp_banner.tpl": jp})
        disc(root / "Charged (Korea).iso", b"R4QK01", 0, {})
        disc(root / "Kart.iso", b"RMCE01", 0, {})
        with open(root / "linux.iso", "wb") as output:
            output.write(b"\x01CD001" + bytes(4096))
        os.truncate(root / "linux.iso", IMAGE_SIZE)
        damaged = root / "Damaged.iso"
        write_disc(damaged, game_id=b"R4QE01", partition=True)
        with open(damaged, "r+b") as output:  # the partition table points past the end
            output.seek(0x40020)
            output.write(struct.pack(">I", 0x7FFFFFF0))
        os.truncate(damaged, IMAGE_SIZE)

        def probe(path):
            result = subprocess.run([executable, "probe", str(path)], capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stderr
            return result.stdout.split()

        assert probe(root / "Charged (USA).iso") == ["charged", "R4QE01", "1", "ISO"]
        assert probe(root / "Europe" / "Charged (Europe) (Rev 2).iso")[:3] == ["charged", "R4QP01", "2"]
        assert probe(root / "Kart.iso")[:2] == ["other", "RMCE01"]
        assert probe(root / "linux.iso")[0] == "notdisc"
        assert probe(damaged)[0] == "unreadable"

        out = pathlib.Path(directory) / "banner.rgba"
        for name, game_id, tpl in (("Charged (USA).iso", "R4QE01", us), ("Charged (Japan).iso", "R4QJ01", jp),
                                   ("Europe/Charged (Europe) (Rev 2).iso", "R4QP01", eu)):
            result = subprocess.run([executable, "banner", str(root / name), game_id, str(out)],
                                    capture_output=True, text=True, timeout=30)
            assert result.returncode == 0, result.stderr
            assert out.read_bytes() == decode(tpl), f"{name}: banner pixels"
        result = subprocess.run([executable, "banner", str(root / "Charged (Korea).iso"), "R4QK01", str(out)],
                                capture_output=True, text=True, timeout=30)
        assert result.returncode != 0 and "us_banner.tpl" in result.stderr, result.stderr

        result = subprocess.run([executable, "scan", str(root)], capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, result.stderr
        assert result.stdout.splitlines() == [
            "Charged (Europe) (Rev 2).iso playable R4QP01",
            "Charged (Japan).iso unsupported R4QJ01",
            "Charged (Korea).iso unsupported R4QK01",
            "Charged (USA).iso playable R4QE01",
            "Damaged.iso unreadable ",
            "other 1",
            "banner R4QE01 49152",
            "banner R4QJ01 49152",
            "banner R4QP01 49152",
            "no-banner R4QK01",
        ], result.stdout
    print("launcher_image_library: C++ checks, 5 probes, 3 banners and a missing one, and a folder scan of synthetic discs")


if __name__ == "__main__":
    run(sys.argv[1])
