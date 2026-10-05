"""Independently encoded Options slides, image and real text hit fixtures."""
import struct
from frontend_visual_fixture import files as visual_files, name_hash
from frontend_image_fixture import bundle, texture

def scene(missing=False,missing_door=False):
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
        if kind==2:pointer(at+0x90,image_resource);word(at+0x94,1)
        if kind==3:word(at+0x90,1);word(at+0xa0,8);struct.pack_into('>2f',data,at+0x98,200,60)
        tail(at+8,list(children));return at
    def slide(value,children=(),duration=.25):
        at=allocate(0x48);name(at+0x20,value,at+0x40);struct.pack_into('>f',data,at+0x14,duration);tail(at+8,list(children));return at
    def comp(value,states,x=0,y=0):
        lib=library(3,value,slides=states);return instance(4,value,lib,x=x,y=y)
    def label(value):return instance(3,value,text_lib)
    def simple(value,childname=""):
        states=[]
        for state in ('off','over','down'):
            children=[]
            if childname:children=[instance(5,'Group',group_lib,children=(label(childname),))]
            states.append(slide(state,children))
        return comp(value,states)
    children=[]
    for i in range(4):
        if missing and i==3:continue
        states=[slide(state,(instance(2,'pointer',image_lib),)) for state in ('waiting','cursor')]
        children.append(comp('cursor'+str(i),states))
    timer=comp('the_timer',[slide(state,(label('Timer'),)) for state in ('4:3','16:9')]);children.append(timer)
    children.append(comp('no home',[slide('Slide1'),slide('widescreen')]))
    doors=[]
    for i in range(1,9):
        if missing_door and i==8:continue
        doors.append(comp('door_'+str(i),[slide('Slide1',(instance(2,'doors',image_lib,x=i*20-90),),duration=.6)]))
    group=instance(5,'Group',group_lib,children=doors)
    children.append(comp('transition',[slide('Slide1',(group,),duration=10)]))
    for label_name in ('+','-','+16:9','-16:9','breadcrumbs'):children.append(simple(label_name))
    for label_name,txt in (('Play_Now','playnow'),('done','done'),('LOWER DONE','done'),('PROGRESS','playnow')):children.append(simple(label_name,txt))
    states=[]
    for state in ('off','over','down'):
        states.append(slide(state,(label('back text'),instance(2,'list_high_250x60',image_lib,x=-7))))
    nested=comp('back',states,x=-200,y=-190)
    nested_wide=instance(4,'back',struct.unpack_from('>I',data,nested+12)[0],x=-305,y=-190)
    children.append(comp('back',[slide('4:3',(nested,)),slide('16:9',(nested_wide,))],x=10,y=8))
    layer=instance(1,'Layer',layer_lib,children=children);active=slide('Slide1',(layer,));presentation=allocate(12)
    pointer(presentation,ring([active]));pointer(presentation+4,active);pointer(4,presentation)
    tail(8,resources);tail(12,libraries);word(16,107);word(20,len(resources));table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table

def files():
    result=visual_files();result['Art/fe/fe_overlay.fen']=scene();result['Art/fe/nav-missing.fen']=scene(missing=True);result['Art/fe/nav-missing-door.fen']=scene(missing_door=True)
    result['Art/fe/MainUI.Dmn']=bundle([(name_hash('frame-image'),texture())])
    hashes=sorted([1]+[name_hash(s.lower()) for s in ('BACK','PLAY_NOW','DONE')])
    strings='AB\0'.encode('utf-16-be');table=b''.join(struct.pack('>II',h,0) for h in hashes)
    for name,lang in (('english',0x7a947b29),('nafrench',0x30d469c4),('naspanish',0x2f242024)):
        result['Art/fe/'+name+'.loc']=struct.pack('>5I',0x4e4c4f43,1,lang,len(hashes),1)+table+strings
    return result
