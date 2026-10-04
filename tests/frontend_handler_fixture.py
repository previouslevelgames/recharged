"""Generated nested notification component; no original game bytes."""
import struct
from frontend_session_fixture import files as session_files, scene
from frontend_visual_fixture import name_hash


def notification():
    raw=scene()
    size=struct.unpack_from('>I',raw,8)[0]
    data=bytearray(raw[16:16+size])
    pointers=list(struct.unpack(f'>{(len(raw)-16-size)//4}I',raw[16+size:]))
    data.extend(bytes(0x758-len(data)))
    def word(at,value):struct.pack_into('>I',data,at,value)
    def pointer(at,value):
        word(at,value)
        if at not in pointers:pointers.append(at)
    def ring(at):pointer(at,at);pointer(at+4,at)
    def attributes(at):
        struct.pack_into('>3f',data,at+24,1,1,1)
        data[at+48:at+53]=bytes([1,255,255,255,255])
    component,library,slide=0x600,0x690,0x710
    pointer(0x38,component)
    ring(component);pointer(component+12,library)
    struct.pack_into('>f',data,component+20,100)
    data[component+24:component+36]=b'Notification'
    word(component+0x38,name_hash('notification'));attributes(component+0x3c)
    word(component+0x88,4);data[component+0x8e]=1
    pointer(0x1a0,library);pointer(0x1a4,library)
    pointer(library,0x1a0);pointer(library+4,0x1a0);pointer(12,library)
    attributes(library+8);word(library+0x50,name_hash('notice'))
    data[library+0x54:library+0x5a]=b'Notice';word(library+0x74,3)
    pointer(library+0x78,slide);pointer(library+0x7c,slide)
    ring(slide);pointer(slide+8,0x80)
    struct.pack_into('>2f',data,slide+16,.125,.5)
    data[slide+0x20:slide+0x26]=b'Slide1';word(slide+0x40,name_hash('slide1'))
    table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table


def files():
    result=session_files();result['Art/fe/notification.fen']=notification();return result
