"""Actual selected Title/Main source ordering over independently encoded assets."""
from pathlib import Path
import subprocess,sys,tempfile,struct,os
from disc_fixture import write_disc
from frontend_title_fixture import files as title_files
from frontend_main_menu_fixture import files as main_files
from frontend_options_fixture import files as options_files
from frontend_navigation_transition_fixture import files as navigation_files,scene as navigation_scene
from frontend_camera_fixture import frontend_camera_files

def script():
    op=lambda code,value=0:(code<<11)|value
    strings=b'startmainmenuball\0fechoosecaptains\0outofballcam\0startmainmenumove\0'
    options=len(b'startmainmenuball\0');back=options+len(b'fechoosecaptains\0');title=back+len(b'outofballcam\0')
    functions=[
        (0x0017a6dd,[op(8,8),op(10)]),
        (0x1b0db3b8,[op(8,47),op(2,95),op(8,5),op(3,0),op(0,0),op(2,0),op(8,23),op(8,44),op(8,46),op(2,0),op(8,21),op(8,41),op(10)]),
        (0x6f23e5f3,[op(3,options),op(2,0),op(2,0),op(8,23),op(2,1),op(8,35),op(2,13),op(2,1),op(8,25),op(10)]),
        (0xc18f9013,[op(8,42),op(3,back),op(2,0),op(2,1),op(8,23),op(8,44),op(0,1),op(8,21),op(8,46),op(2,1),op(2,2),op(8,25),op(10)]),
        (0xc41b2549,[op(3,title),op(8,15),op(2,0),op(8,28),op(8,44),op(0,2),op(8,32),op(8,48),op(2,1),op(2,1),op(8,25),op(10)])]
    code=[];table=bytearray()
    for key,instructions in functions:
        table.extend(struct.pack('>IIHBB',key,len(code)*2,2,0,0));code.extend(instructions)
    header=[0xe11c2112,len(functions),0,0,12,len(code)*2,len(strings),0,0,0,0,0,0,0,0,0,0,0]
    return struct.pack('>18I',*header)+table+struct.pack('>3f',.25,1,.1)+struct.pack('>'+str(len(code))+'H',*code)+strings

exe=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='charged-title-flow-') as folder:
    root=Path(folder);subprocess.run([exe,'--fixture',str(root)],check=True,timeout=20)
    payloads={};localization={}
    for group in (title_files(),main_files(),options_files(),navigation_files()):
        for name,data in group.items():
            if name.endswith('.loc'):
                magic,version,language,count,flags=struct.unpack_from('>5I',data)
                text=data[20+count*8:].decode('utf-16-be');header,values=localization.setdefault(name,((magic,version,language,flags),{}))
                for i in range(count):
                    key,offset=struct.unpack_from('>2I',data,20+i*8);values[key]=text[offset:].split('\0',1)[0]
            else:payloads[name]=data
    for name,((magic,version,language,flags),values) in localization.items():
        table=bytearray();strings=bytearray()
        for key,value in sorted(values.items()):table+=struct.pack('>2I',key,len(strings)//2);strings+=(value+'\0').encode('utf-16-be')
        payloads[name]=struct.pack('>5I',magic,version,language,len(values),flags)+table+strings
    payloads['Art/fe/fe_overlay.fen']=navigation_scene(title_accept=True)
    payloads.update(frontend_camera_files());payloads['Art/scripts/fe_presentation.byte_code']=script()
    payloads.update({'audio/calculation.bun':(root/'calculation.bun').read_bytes(),'audio/FE_GEN_Music.resbun':(root/'music.resbun').read_bytes(),'audio/FE_GEN_Music.nlxwb':(root/'music.nlxwb').read_bytes()})
    write_disc(root/'title-flow.iso',files=payloads,fst_capacity=0x1000,partition_size=0x80000)
    subprocess.run([exe,str(root/'title-flow.iso'),str(root),'generated'],check=True,timeout=150,env={**os.environ,'SDL_VIDEODRIVER':'dummy','SDL_RENDER_DRIVER':'software','SDL_AUDIODRIVER':'dummy'})
