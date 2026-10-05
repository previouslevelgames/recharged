"""Independent authored Credits phase/fade fixture; no retail bytes."""
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
    image_resource=resource('frame-image',0);font_resource=resource('fot-rodinprob18',1);movie_resource=resource('movie',0)
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
        if kind==2:pointer(at+0x90,struct.unpack_from('>I',data,lib+0x78)[0])
        if kind==3:word(at+0x90,1);word(at+0xa0,8);struct.pack_into('>2f',data,at+0x98,200,60)
        tail(at+8,list(children));return at
    def slide(value,children=(),duration=.25):
        at=allocate(0x48);name(at+0x20,value,at+0x40);struct.pack_into('>f',data,at+0x14,duration);tail(at+8,list(children));return at
    def component(value,children,x=0,y=0):
        lib=library(3,value,slides=children);return instance(4,value,lib,x,y)
    def button(value,x,y):
        slides=[slide(state,[instance(2,'hit',image_lib)]) for state in ('off','over','down')]
        return component(value,slides,x,y)
    movie_lib=library(1,'movie',resource=movie_resource)
    frames=[]
    for title in ('NINTENDO','NLG','NLG 4:3','CREDITS','Credits 4:3','COPYRIGHTS'):
        fade=component('MISSING' if missing else 'WHITE FADE',[slide('OFF'),slide('FADEIN',[instance(2,'white',image_lib)])])
        children=[instance(2,'logo',image_lib),fade]
        if title in ('NLG','NLG 4:3','CREDITS','Credits 4:3'):children.append(instance(2,'movie',movie_lib))
        if title in ('CREDITS','Credits 4:3'):
            children.extend(instance(3,'line'+str(i+1),text_lib,x=40) for i in range(20))
            children.append(instance(3,'Final Message',text_lib))
        frames.append(slide(title,[instance(1,'Layer',layer_lib,children=children)],duration=20))
    presentation=allocate(12);pointer(presentation,ring(frames));pointer(presentation+4,frames[0]);pointer(4,presentation)
    tail(8,resources);tail(12,libraries);word(16,120);word(20,len(resources));table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table

def files():
    result=visual_files();result['Art/fe/credits.fen']=scene();result['Art/fe/credits-missing.fen']=scene(True)
    result['Art/fe/MainUI.Dmn']=bundle([(name_hash('frame-image'),texture())])
    result['credits.txt']=b'# Independent credits fixture\nFIRST\n+\n@\nCENTER\nLAST\n'
    return result
