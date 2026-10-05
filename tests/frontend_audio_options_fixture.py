"""Independently encoded Options slides, image and real text hit fixtures."""
import struct
from frontend_visual_fixture import files as visual_files, name_hash
from frontend_image_fixture import bundle, texture

def scene(missing=False):
    data=bytearray(24);pointers=set();libraries=[];resources=[]
    def allocate(n):at=len(data);data.extend(bytes(n));return at
    def word(at,value):struct.pack_into('>I',data,at,value)
    def pointer(at,value):word(at,value);pointers.add(at)
    def ring(nodes):
        for i,at in enumerate(nodes):pointer(at,nodes[(i+1)%len(nodes)]);pointer(at+4,nodes[i-1])
        return nodes[-1] if nodes else None
    def tail(at,nodes):
        value=ring(nodes)
        if value is not None:pointer(at,value)
    def name(at,value,hash_at):
        raw=value.encode();assert len(raw)<32;data[at:at+len(raw)]=raw;word(hash_at,name_hash(value.lower()))
    def attrs(at,x=0,y=0,scale=1):
        struct.pack_into('>3f',data,at,x,y,0);struct.pack_into('>3f',data,at+24,scale,scale,1);data[at+48:at+53]=bytes((1,255,255,255,255))
    def resource(value,kind):
        at=allocate(0x1c if kind==1 else 0x20);resources.append(at);word(at+8,kind);word(at+12,name_hash(value));return at
    image_resource=resource('frame-image',0);font_resource=resource('fot-rodinprob18',1)
    def library(kind,value,scale=1,resource=None,slides=None):
        at=allocate(0x88 if kind==2 else 0x80 if kind==3 else 0x7c if kind==1 else 0x78);libraries.append(at);attrs(at+8,scale=scale);name(at+0x54,value,at+0x50);word(at+0x74,kind)
        if resource is not None:pointer(at+0x78,resource)
        if kind==2:struct.pack_into('>2f',data,at+0x80,200,60)
        if slides:tail(at+0x78,slides);pointer(at+0x7c,slides[0])
        return at
    layer_lib=library(0,'layer');group_lib=library(4,'group');image_lib=library(1,'hit',scale=.4,resource=image_resource);text_lib=library(2,'text',resource=font_resource)
    def instance(kind,value,lib,x=0,y=0,children=()):
        at=allocate(0x114 if kind==3 else 0x98 if kind==2 else 0x90);pointer(at+12,lib);attrs(at+0x3c,x,y);name(at+0x18,value,at+0x38)
        struct.pack_into('>f',data,at+20,1000);word(at+0x84,1 if x or y else 0);word(at+0x88,kind);data[at+0x8e]=1
        if kind==3:word(at+0x90,1);word(at+0xa0,8);struct.pack_into('>2f',data,at+0x98,200,60)
        tail(at+8,list(children));return at
    def slide(value,children=(),duration=.25):
        at=allocate(0x48);name(at+0x20,value,at+0x40);struct.pack_into('>f',data,at+0x14,duration);tail(at+8,list(children));return at
    def component(value,children,x=0,y=0):
        lib=library(3,value,slides=children);return instance(4,value,lib,x,y)
    def button(value,y):
        slides=[]
        for state in ('off','over','slide1','unused'):
            hit=instance(2,'hit',image_lib)
            slides.append(slide(state,(hit,)))
        return component(value,slides,0,y)
    left=component('scrollbar_left',[slide('Slide1',[button('down_arrow'+(str(i+1) if i else ''),70-i*70) for i in range(3)])],-100,0)
    right=component('scrollbar_right',[slide('Slide1',[button('up_arrow'+(str(i+1) if i else ''),70-i*70) for i in range(3)])],100,0)
    children=[left,right]
    for index,(volume,text) in enumerate((('MUSIC VOLUME','MUSIC SETTING'),('SFX VOLUME','SFX SETTING2'),('VOX VOLUME','VOX SETTING3'))):
        bars=[instance(2,'whitebox'+(str(i+1) if i else ''),image_lib,x=i*8) for i in range(10)]
        children.append(instance(5,volume,group_lib,children=bars));children.append(instance(3,'missing' if missing and index==2 else text,text_lib))
    group=instance(5,'visual_options',group_lib,children=children)
    inside=slide('OPTIONS_IN',[instance(1,'Layer',layer_lib,children=[instance(2,'blackbox',image_lib),group])])
    outside=slide('OPTIONS_OUT',[instance(1,'Layer',layer_lib,children=[instance(2,'blackbox',image_lib)])])
    presentation=allocate(12);pointer(presentation,ring([inside,outside]));pointer(presentation+4,inside);pointer(4,presentation)
    tail(8,resources);tail(12,libraries);word(16,112);word(20,len(resources));table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table

def files():
    result=visual_files();result['Art/fe/options_audio_options.fen']=scene();result['Art/fe/audio-missing.fen']=scene(missing=True)
    result['Art/fe/MainUI.Dmn']=bundle([(name_hash('frame-image'),texture())])
    ids=sorted([1]+[name_hash(s.lower()) for s in ('OPTIONS_AUDIO_MUSIC_LEVEL','OPTIONS_AUDIO_SFX_LEVEL','OPTIONS_AUDIO_VOX_LEVEL')])
    strings='AB\0LEVEL {0}\0'.encode('utf-16-be');table=b''.join(struct.pack('>II',h,0 if h==1 else 3) for h in ids)
    result['Art/fe/english.loc']=struct.pack('>5I',0x4e4c4f43,1,0x7a947b29,len(ids),1)+table+strings
    return result
