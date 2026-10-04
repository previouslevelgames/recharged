"""Generated retained-session FEN/font/image inputs. No retail bytes."""
import struct
from frontend_animation_fixture import animation
from frontend_layout_fixture import layout
from frontend_visual_fixture import files as visuals, name_hash
from frontend_image_fixture import bundle, texture


def scene(animated=True, bad_colour=False, missing_image=False):
    raw = animation() if animated else layout()
    length = struct.unpack_from('>I', raw, 8)[0]
    data = bytearray(raw[16:16+length])
    pointers = list(struct.unpack(f'>{(len(raw)-16-length)//4}I', raw[16+length:]))
    def word(at,value): struct.pack_into('>I',data,at,value)
    def pointer(at,value):
        word(at,value)
        if at not in pointers: pointers.append(at)
    word(0x70,name_hash('slide'))
    if not animated:
        struct.pack_into('>2f',data,0x220,128,64)
        struct.pack_into('>2f',data,0x118,128,64)
    if missing_image: word(0x24c,0xbadbaaad)
    if bad_colour:
        word(0x280+20,5)
        first,last=0x280+28,0x280+28+56
        for axis in range(3):
            struct.pack_into('>4f',data,first+16*axis,200,200+200/3,200+400/3,0)
            struct.pack_into('>4f',data,last+16*axis,400,-1,-1,2)
    # Second empty presentation slide, plus a duplicate name for ambiguity checks.
    for name in ('Blank','Duplicate','Duplicate'):
        at=len(data);data.extend(bytes(0x48))
        # Insert before first; tail changes and traversal remains authored order.
        old_tail=struct.unpack_from('>I',data,0x18)[0]
        pointer(at,0x30);pointer(at+4,old_tail)
        pointer(old_tail,at);pointer(0x34,at);pointer(0x18,at)
        struct.pack_into('>f',data,at+0x14,2)
        data[at+0x20:at+0x20+len(name)]=name.encode()
        word(at+0x40,name_hash(name.lower()))
    table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table


def files():
    payloads=visuals()
    payloads.update({'Art/fe/session.fen':scene(), 'Art/fe/text.fen':scene(False),
        'Art/fe/bad.fen':b'malformed but nonempty frontend data',
        'Art/fe/colour.fen':scene(bad_colour=True),
        'Art/fe/missing-image.fen':scene(missing_image=True)})
    textures=bundle([(name_hash('frame-image'),texture())])
    payloads.update({'Art/fe/MainUI.Dmn':textures,'Art/fe/InGameUI.Res':textures,'Art/fe/InGameUI.Dmn':bundle([])})
    return payloads
