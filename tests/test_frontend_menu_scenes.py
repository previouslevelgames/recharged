"""Genuine selected Main/Options/Audio/Visual flow over independent generated Wii assets."""
from pathlib import Path
import subprocess
import sys
import tempfile
import struct
from disc_fixture import write_disc
from frontend_main_menu_fixture import files as main_files
from frontend_options_fixture import files as options_files
from frontend_navigation_transition_fixture import files as navigation_files
from frontend_camera_fixture import frontend_camera_files
from frontend_menu_transition_fixture import script
from frontend_audio_options_fixture import files as audio_files
from frontend_visual_options_fixture import files as visual_files
from frontend_navigation_done_fixture import with_navigation

with tempfile.TemporaryDirectory(prefix='charged-menu-scenes-') as folder:
    root = Path(folder)
    payloads = {}
    localization = {}
    for group in (main_files(), options_files(), audio_files(), visual_files(), navigation_files()):
        for name, data in group.items():
            if name.endswith('.loc'):
                magic, version, language, count, flags = struct.unpack_from('>5I', data)
                text = data[20 + count * 8:].decode('utf-16-be')
                header, values = localization.setdefault(name, ((magic, version, language, flags), {}))
                for i in range(count):
                    key, offset = struct.unpack_from('>2I', data, 20 + i * 8)
                    values[key] = text[offset:].split('\0', 1)[0]
            else:
                payloads[name] = data
    for name, ((magic, version, language, flags), values) in localization.items():
        table = bytearray()
        strings = bytearray()
        for key, value in sorted(values.items()):
            table += struct.pack('>2I', key, len(strings) // 2)
            strings += (value + '\0').encode('utf-16-be')
        payloads[name] = struct.pack('>5I', magic, version, language, len(values), flags) + table + strings
    payloads = with_navigation(payloads)
    payloads.update(frontend_camera_files())
    payloads['Art/scripts/fe_presentation.byte_code'] = script()
    write_disc(root / 'menus.iso', files=payloads, fst_capacity=0x1000,
               partition_size=0x80000)
    subprocess.run([str(Path(sys.argv[1]).resolve()), str(root / 'menus.iso'),
                    str(root), 'generated'], check=True, timeout=110)
