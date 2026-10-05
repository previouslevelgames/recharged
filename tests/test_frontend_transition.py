"""Original synthetic title transition over actual NL Wii files; no retail data."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_handler_fixture import files, notification
from frontend_camera_fixture import frontend_camera_files
from frontend_visual_fixture import name_hash

def script():
    op=lambda code,value=0:(code<<11)|value
    code=[op(8,8),op(10),op(3,0),op(8,15),op(2,0),op(8,28),op(8,44),
          op(0,0),op(8,32),op(8,48),op(2,1),op(2,1),op(8,25),op(10)]
    strings=b'startmainmenumove\0'
    header=[0xe11c2112,2,0,0,4,len(code)*2,len(strings),0,0,0,0,0,0,0,0,0,0,0]
    return (struct.pack('>18I',*header)+struct.pack('>IIHBB',0x17a6dd,0,2,0,0)
        +struct.pack('>IIHBB',0xc41b2549,4,2,0,0)+struct.pack('>f',.1)
        +struct.pack('>'+str(len(code))+'H',*code)+strings)

def renamed_scene(name):
    data=bytearray(notification())
    data[16+0x50:16+0x70]=name.encode()+bytes(32-len(name))
    struct.pack_into('>I',data,16+0x70,name_hash(name.lower()))
    return bytes(data)

with tempfile.TemporaryDirectory(prefix='mscharged-frontend-transition-') as folder:
    root=Path(folder);payloads=files();payloads.update(frontend_camera_files())
    payloads['Art/fe/sms2_start.fen']=renamed_scene('regular')
    payloads['Art/fe/main_menu_v3.fen']=renamed_scene('MAIN')
    payloads['Art/scripts/fe_presentation.byte_code']=script()
    write_disc(root/'transition.iso',files=payloads,fst_capacity=0x1000,partition_size=0x10000)
    subprocess.run([str(Path(sys.argv[1]).resolve()),str(root/'transition.iso'),str(root),'generated'],check=True,timeout=100)
