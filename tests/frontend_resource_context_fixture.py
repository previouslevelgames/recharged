"""Independent generated FEN/font/image context inputs; no retail bytes."""
import struct
from frontend_session_fixture import files as session_files, scene
from frontend_visual_fixture import name_hash


def empty(embedded=False):
    data=bytearray(36)
    struct.pack_into('>I',data,4,24)
    struct.pack_into('>I',data,16,997)
    pointers=[4]
    if embedded:
        data.extend(bytes(32))
        struct.pack_into('>I',data,8,36)
        struct.pack_into('>I',data,20,1)
        struct.pack_into('>4I',data,36,36,36,2,0x7711)
        pointers.extend([8,36,40])
    table=b''.join(struct.pack('>I',p) for p in pointers)
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table


def edit(raw, *, saved_valid=False, resource_hash=None):
    result=bytearray(raw)
    if saved_valid:result[16+0x250]=1
    if resource_hash is not None:struct.pack_into('>I',result,16+0x24c,resource_hash)
    return bytes(result)


def pair():
    data=bytearray(96)
    struct.pack_into('>I',data,4,24)
    struct.pack_into('>I',data,8,68)
    struct.pack_into('>II',data,16,998,2)
    struct.pack_into('>4I',data,36,68,68,0,name_hash('frame-image'))
    struct.pack_into('>4I',data,68,36,36,1,name_hash('fot-rodinprob18'))
    table=b''.join(struct.pack('>I',p) for p in [4,8,36,40,68,72])
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table


def files():
    result=session_files()
    result['Art/fe/empty.fen']=empty()
    result['Art/fe/embedded.fen']=empty(True)
    result['Art/fe/pair.fen']=pair()
    result['Art/fe/saved-valid.fen']=edit(scene(),saved_valid=True)
    result['Art/fe/fallback.fen']=edit(scene(False),resource_hash=name_hash('missing-original-font'))
    result['Art/fe/fallback2.fen']=edit(scene(False),resource_hash=name_hash('another-missing-original-font'))
    result['Art/fe/dynamic.fen']=edit(scene(),resource_hash=name_hash('movie'))
    return result
