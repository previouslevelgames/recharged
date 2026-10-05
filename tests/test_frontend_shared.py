"""Permanent MainUI sharing over real NL reads; all resources are generated."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_session_fixture import files, scene
from frontend_image_fixture import bundle, texture
from frontend_visual_fixture import name_hash

executable = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='mscharged-frontend-shared-') as folder:
    root = Path(folder)
    for mode in ('success', 'bad-unused', 'missing-main', 'font-collision'):
        payloads = files()
        nav = bytearray(scene())
        struct.pack_into('>I', nav, 16 + 0x24c, name_hash('nav-image'))
        payloads['Art/fe/nav.fen'] = nav
        hidden = bytearray(scene(missing_image=True))
        # Empty active slide still retains a missing static image in its ring.
        first_extra = struct.unpack_from('>I', hidden, 16 + 0x34)[0]
        struct.pack_into('>I', hidden, 16 + 0x1c, first_extra)
        payloads['Art/fe/hidden-missing.fen'] = hidden
        hidden_present = bytearray(hidden)
        struct.pack_into('>I', hidden_present, 16 + 0x24c, name_hash('frame-image'))
        payloads['Art/fe/hidden-present.fen'] = hidden_present
        entries = [(name_hash('frame-image'), texture(value=65)),
                   (name_hash('frame-image'), b'unused duplicate'),
                   (name_hash('nav-image'), texture(2, value=91)),
                   (name_hash('invisible'), texture(8, value=7))]
        if mode == 'bad-unused': entries[-1] = (name_hash('invisible'), b'invalid texture')
        if mode == 'font-collision': entries.append((name_hash('fe/fonts/eurfonttext18_1'), texture()))
        payloads['Art/fe/MainUI.Dmn'] = bundle(entries)
        if mode == 'missing-main': del payloads['Art/fe/MainUI.Dmn']
        write_disc(root / 'shared.iso', files=payloads)
        subprocess.run([executable, str(root / 'shared.iso'), str(root), mode], check=True, timeout=60)
