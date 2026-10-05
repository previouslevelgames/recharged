"""Generated original NAV Done component/labels, shared by Audio and Visual tests."""
import struct
from frontend_navigation_transition_fixture import files as navigation_files
from frontend_visual_fixture import name_hash


def with_navigation(payloads):
    """Keep each submenu's actual fixtures and append NAV's independent graph."""
    result = dict(payloads)
    navigation = navigation_files()
    result['Art/fe/fe_overlay.fen'] = navigation['Art/fe/fe_overlay.fen']
    for key in ('Art/fe/english.loc', 'Art/fe/nafrench.loc', 'Art/fe/naspanish.loc'):
        raw = result[key]
        magic, version, language, count, flags = struct.unpack_from('>5I', raw)
        assert magic == 0x4e4c4f43 and version == 1
        text = raw[20 + count * 8:].decode('utf-16-be')
        values = {}
        for i in range(count):
            identifier, offset = struct.unpack_from('>2I', raw, 20 + i * 8)
            values[identifier] = text[offset:].split('\0', 1)[0]
        for name in ('BACK', 'PLAY_NOW', 'DONE', 'OPTIONS_ACCEPT'):
            values[name_hash(name.lower())] = 'AB'
        table = bytearray()
        strings = bytearray()
        for identifier, value in sorted(values.items()):
            table += struct.pack('>2I', identifier, len(strings) // 2)
            strings += (value + '\0').encode('utf-16-be')
        result[key] = struct.pack('>5I', magic, version, language, len(values), flags) + table + strings
    return result
