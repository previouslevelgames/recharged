"""Independent raw PCM + actual SDL device/source-SDK qualification."""
from pathlib import Path
import os
import struct
import subprocess
import sys
import tempfile


def fnv1a(data):
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def run(command, driver="dummy"):
    environment = dict(os.environ)
    environment["SDL_AUDIO_DRIVER"] = driver
    environment["SDL_AUDIO_DEVICE_SAMPLE_FRAMES"] = "128"
    result = subprocess.run(command, env=environment, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=30)
    print(result.stdout, end="")
    if result.returncode:
        raise RuntimeError(f"SDK qualifier exited {result.returncode}: {command[0]}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--original-only":
        run([str(Path(sys.argv[2]).resolve()), str(fnv1a(bytes(384)))])
        return
    if len(sys.argv) not in (2, 3):
        raise ValueError("test_native_ai.py NATIVE_AI_TEST [ORIGINAL_MOVIE_AI_TEST]")
    native = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="mscharged-native-ai-") as temporary:
        raw = struct.pack("<256h", *[((n * 491 + 12345) % 65536) - 32768
                                     for n in range(256)])
        path = Path(temporary) / "dma.pcm"
        path.write_bytes(raw)
        run([native, str(path), str(fnv1a(raw))])
    run([native, "--bad-driver"], driver="mscharged-invalid-audio-driver")
    if len(sys.argv) == 3:
        run([str(Path(sys.argv[2]).resolve()), str(fnv1a(bytes(384)))])


if __name__ == "__main__":
    main()
