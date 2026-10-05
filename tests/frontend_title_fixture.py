"""Independent Title shapes and states; no retail bytes."""
import struct
from frontend_visual_fixture import files as visual_files, name_hash
from frontend_image_fixture import bundle, texture


def scene(missing=False):
    data=bytearray(24);pointers=set();libraries=[];resources=[]
    def allocate(n):
        at=len(data);data.extend(bytes(n));return at
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
    def attrs(at,x=0,y=0,sx=1,sy=1):
        struct.pack_into('>3f',data,at,x,y,0);struct.pack_into('>3f',data,at+24,sx,sy,1);data[at+48:at+53]=bytes((1,255,255,255,255))
    def resource(value,kind):
        at=allocate(0x20);resources.append(at);word(at+8,kind);word(at+12,name_hash(value));return at
    image_resource=resource('frame-image',0)
    def library(kind,value,sx=1,sy=1,slides=None):
        at=allocate(0x80 if kind==3 else 0x7c if kind==1 else 0x78);libraries.append(at);attrs(at+8,sx=sx,sy=sy);name(at+0x54,value,at+0x50);word(at+0x74,kind)
        if kind==1:pointer(at+0x78,image_resource)
        if slides:tail(at+0x78,slides);pointer(at+0x7c,slides[0])
        return at
    layer_lib=library(0,'layer')
    def instance(kind,value,lib,x=0,y=0,children=()):
        at=allocate(0x98 if kind==2 else 0x90);pointer(at+12,lib);attrs(at+0x3c,x,y);name(at+0x18,value,at+0x38)
        struct.pack_into('>f',data,at+20,1000);word(at+0x84,1 if x or y else 0);word(at+0x88,kind);data[at+0x8e]=1;tail(at+8,list(children));return at
    def slide(value,children=(),duration=300):
        at=allocate(0x48);name(at+0x20,value,at+0x40);struct.pack_into('>f',data,at+0x14,duration);tail(at+8,list(children));return at
    frames=[]
    for title,x,y in [('regular',130,-70),('widescreen',-180,50)]:
        states=[]
        for state,sx,sy in [('off',.4,.4),('over',.8,.6),('down',1.2,.2)]:
            image=instance(2,'hit',library(1,title+' '+state,sx,sy));states.append(slide(state,[image]))
        component=instance(4,'MISSING' if missing else 'Component2',library(3,title+' press',slides=states),x,y)
        layer=instance(1,'Layer2',layer_lib,children=(component,));frames.append(slide(title,[layer]))
    presentation=allocate(12);pointer(presentation,ring(frames));pointer(presentation+4,frames[0]);pointer(4,presentation)
    tail(8,resources);tail(12,libraries);word(16,127);word(20,len(resources));table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table


def files():
    result=visual_files();result['Art/fe/sms2_start.fen']=scene();result['Art/fe/title-missing.fen']=scene(True)
    result['Art/fe/MainUI.Dmn']=bundle([(name_hash('frame-image'),texture())])
    return result
