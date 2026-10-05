"""Generated shared Audio/NAV graph with original fixed Done bounds overlap."""
from pathlib import Path
import struct, subprocess, sys, tempfile
from disc_fixture import write_disc
from frontend_audio_options_fixture import files
from frontend_navigation_done_fixture import with_navigation

payloads = with_navigation(files())
raw = bytearray(payloads['Art/fe/options_audio_options.fen'])
for name, x, y in [('scrollbar_right', 0, 0), ('up_arrow', 0, -210)]:
    found = []
    for at in range(16, len(raw) - 0x90, 4):
        if raw[at + 0x18:at + 0x38].split(b'\0', 1)[0] == name.encode() and struct.unpack_from('>I', raw, at + 0x88)[0] == 4:
            found.append(at)
    assert len(found) == 1
    struct.pack_into('>3f', raw, found[0] + 0x3c, x, y, 0)
payloads['Art/fe/options_audio_options.fen'] = bytes(raw)
with tempfile.TemporaryDirectory(prefix='charged-audio-navigation-') as directory:
    root = Path(directory)
    write_disc(root / 'audio.iso', files=payloads, fst_capacity=0x1000, partition_size=0x40000)
    subprocess.run([str(Path(sys.argv[1]).resolve()), str(root / 'audio.iso'), str(root), 'generated'], check=True, timeout=100)
