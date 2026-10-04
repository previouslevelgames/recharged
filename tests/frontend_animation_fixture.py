"""Generated one-image FEN animation, with real key/ring layouts and no retail bytes."""
import struct
from frontend_layout_fixture import layout
from frontend_visual_fixture import name_hash


def animation():
    original = layout()
    size = struct.unpack_from('>I', original, 8)[0]
    data = bytearray(original[16:16 + size])
    relocations = list(struct.unpack(f'>{(len(original)-16-size)//4}I', original[16+size:]))
    def word(at, value): struct.pack_into('>I', data, at, value)
    def pointer(at, value):
        word(at, value)
        if at not in relocations: relocations.append(at)
    # Convert the generated single label/library/font resource to an image.
    word(0x108, 2); pointer(0x110, 0x240); word(0x114, 1)
    word(0x214, 1); word(0x248, 0); word(0x24c, name_hash('frame-image'))
    struct.pack_into('>4f', data, 0x1e0, 0, 0, 1, 1)
    struct.pack_into('>f', data, 0x44, 2); word(0x4c, 1)
    struct.pack_into('>f', data, 0x94, 2)
    record = len(data); first = record + 28; last = first + 56
    data.extend(bytes(28+2*56))
    pointer(0x3c, record); pointer(record+4, record); pointer(record+8, record)
    pointer(record+12, 0x80); struct.pack_into('>H', data, record+16, 1)
    word(record+20, 1); pointer(record+24, last)
    for at, nxt, prev, time, point in ((first,last,last,0,-160),(last,first,first,2,160)):
        pointer(at+48,nxt); pointer(at+52,prev)
        for axis in range(3):
            p = point if axis == 0 else 0
            c1, c2 = ((-160/3,160/3) if axis == 0 else (0,0)) if at == first else (-1,-1)
            struct.pack_into('>4f',data,at+16*axis,p,c1,c2,time)
    table = b''.join(struct.pack('>I', value) for value in sorted(relocations))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table
