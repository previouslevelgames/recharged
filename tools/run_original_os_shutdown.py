"""Verify whole original SDK shutdown through native terminal power removal.

Only generated disposable system records/media are used. This is not original
ResetTask/main or active-voice game shutdown qualification.
"""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from disc_fixture import write_disc


def checked(condition, text):
    if not condition:
        raise AssertionError(text)


def checksum(raw):
    return sum(struct.unpack(">" + "I" * ((len(raw) - 4) // 4), raw[4:])) & 0xFFFFFFFF


def main():
    if len(sys.argv) not in (3, 4) or (len(sys.argv)==4 and sys.argv[3]!="--sleeping-thread"):
        raise SystemExit("usage: run_original_os_shutdown.py EXECUTABLE AX-SOURCE-IMAGE [--sleeping-thread]")
    executable, image = (Path(value).resolve() for value in sys.argv[1:3])
    identity = hashlib.sha256(image.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix="original-whole-shutdown-") as temporary:
        root = Path(temporary)
        disc = root / "synthetic.iso"
        write_disc(disc, files={"shutdown.txt": b"Generated shutdown medium\n"})
        run = subprocess.run([str(executable), str(image), identity, str(disc), str(root), *sys.argv[3:]],
                             capture_output=True, text=True, timeout=20)
        print(run.stdout, end="")
        print(run.stderr, end="", file=sys.stderr)
        checked(run.returncode == 0, "Original shutdown/device boundary failed")
        checked("Whole original OSShutdownSystem terminal PASS" in run.stdout,
                "Process exited without actual terminal verification")
        if len(sys.argv)==4:
            checked("Original sleeping-worker terminal proof:" in run.stdout,
                    "Power removal did not retain the actual cancelled source frame")
        checked(hashlib.sha256(image.read_bytes()).hexdigest() == identity,
                "Original source image changed during qualification")
        receipt = dict(line.split("=", 1) for line in (root / "terminal.txt").read_text().splitlines())
        checked(receipt["whole_source_shutdown"] == "1" and int(receipt["ax_frames"]) >= 8,
                "Missing real original AX predecessor or terminal receipt")
        # The observed SET_ALARM predecessor is reached by original async
        # Open/Read/Seek (3). Its Stop branch writes/closes synchronously; the
        # registered NAND shutdown hook contributes exactly one async Flush.
        checked(int(receipt["ios_completions"]) == 4,
                "Original Open/Read/Seek plus shutdown Flush callbacks differ")
        data = root / "nand/data/title/00000001/00000002/data"
        before = (root / "state-before.bin").read_bytes()
        state = (data / "state.dat").read_bytes()
        expected = bytearray(before)
        expected[5:7] = bytes((1, 2))  # Original no-IPL.IDL reboot-state/RTC waiting choice.
        struct.pack_into(">I", expected, 0, checksum(expected))
        checked(state == expected, "Original state wire/decision/checksum/opaque bytes differ")
        before_play = (root / "play-before.bin").read_bytes()
        play = (data / "play_rec.dat").read_bytes()
        checked(len(play) == 128 and struct.unpack_from(">I", play)[0] == checksum(play),
                "Original stopped play-record wire/checksum is invalid")
        checked(struct.unpack_from(">q", play, 96)[0] > 0, "Original stop timestamp did not advance")
        normalized = bytearray(play)
        normalized[:4] = before_play[:4]
        normalized[96:104] = before_play[96:104]
        checked(normalized == before_play, "Stop changed original title/start/opaque record fields")
        rtc = (root / "rtc.bin").read_bytes()
        expected_rtc = bytearray(68)
        expected_rtc[2:4] = bytes.fromhex("fffc")
        checked(rtc == expected_rtc, "Original RTC event clear/SRAM persistence differs")
        print("Whole original OSShutdownSystem CPU wire/terminal oracle PASS; SDK leaf only")


if __name__ == "__main__":
    main()
