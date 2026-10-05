"""Synthetic authored movie resource, preserving name/runtime-handle distinction."""
import struct
from frontend_credits_fixture import files as credits_files
from thp_movie_fixture import files as movie_files
from frontend_visual_fixture import name_hash
from frontend_image_fixture import bundle,texture

def files():
    result=credits_files();raw=result['Art/fe/credits.fen'];size,reloc=struct.unpack_from('>II',raw,8)
    data=bytearray(raw[16:16+size]);pointers=set(struct.unpack('>'+str(reloc//4)+'I',raw[16+size:]))
    for at in range(len(data)-0x98):
        if data[at+0x18:at+0x1d]!=b'logo\0':continue
        data[at+0x18:at+0x38]=b'movie\0'+bytes(26);struct.pack_into('>I',data,at+0x38,name_hash('movie'))
        library=struct.unpack_from('>I',data,at+12)[0];resource=struct.unpack_from('>I',data,library+0x78)[0]
        struct.pack_into('>I',data,at+0x90,resource);pointers.add(at+0x90)
        struct.pack_into('>4f',data,library+8+56,0,0,1,1)
    table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    result['Art/fe/credits.fen']=struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table
    # A distinct synthetic FEN proves the source-authored unresolved movie
    # case before any runtime SetTextureHandle, through real NL bytes.
    pending=bytearray(data)
    for at in range(len(pending)-0x98):
        if pending[at+0x18:at+0x1e]==b'movie\0':
            resource=struct.unpack_from('>I',pending,at+0x90)[0]
            struct.pack_into('>I',pending,resource+12,name_hash('movie'))
    result['Art/fe/credits-pending.fen']=struct.pack('>4I',0x46454e4c,1,len(pending),len(table))+pending+table
    blue=bytearray(texture(fmt=0));blue[8:12]=bytes([5,6,5,0]);blue[32:]=b'\x00\x1f'*((len(blue)-32)//2)
    result['Art/fe/MainUI.Dmn']=bundle([(name_hash('frame-image'),blue)])
    result.update(movie_files());return result
