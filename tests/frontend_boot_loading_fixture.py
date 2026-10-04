"""Authored retail-boot shape, with entirely generated geometry and texture bytes."""
import struct
from frontend_visual_fixture import files as visuals, name_hash
from frontend_image_fixture import bundle, texture


def scene(missing=False, wrong_type=False):
    data=bytearray(0x30);pointers=set()
    def alloc(size):
        at=len(data);data.extend(bytes(size));return at
    def word(at,value):struct.pack_into('>I',data,at,value)
    def pointer(at,value):word(at,value);pointers.add(at)
    def name(at,value):data[at:at+len(value)]=value.encode()
    def ring(items):
        for i,at in enumerate(items):pointer(at,items[(i+1)%len(items)]);pointer(at+4,items[(i-1)%len(items)])
        return items[-1]
    def attrs(at,scale=1):
        struct.pack_into('>3f',data,at+24,scale,scale,1)
        data[at+48:at+53]=bytes([1,255,255,255,255]);struct.pack_into('>2f',data,at+64,1,1)
    libraries=[];resources=[]
    def library(kind,label,scale=1):
        at=alloc(0x88 if kind==2 else 0x80 if kind==3 else 0x7c if kind==1 else 0x78);libraries.append(at)
        attrs(at+8,scale);name(at+0x54,label);word(at+0x50,name_hash(label.lower()));word(at+0x74,kind);return at
    def slide(label,duration=.5,start=0):
        at=alloc(0x48);name(at+32,label);word(at+64,name_hash(label.lower()));struct.pack_into('>2f',data,at+16,start,duration);return at
    def instance(kind,label,lib,resource=None):
        at=alloc(0x114 if kind==3 else 0x98 if kind==2 else 0x90);pointer(at+12,lib);name(at+24,label);word(at+0x38,name_hash(label.lower()))
        struct.pack_into('>f',data,at+20,100);attrs(at+0x3c);word(at+0x88,kind);data[at+0x8e]=1
        if kind==2:
            pointer(at+0x90,resource);word(at+0x94,1)
        return at
    def resource(label,kind=0):
        at=alloc(0x20);resources.append(at);word(at+8,kind);word(at+12,name_hash(label));return at
    resource_ids={label:resource(label) for label in ('boot-en','boot-fr','boot-es','boot-nunchuk','boot-logo')}
    fallback=library(2,'fallback-text');pointer(fallback+0x78,resource('unregistered-boot-font',1))
    paragraph=library(2,'paragraph-text');pointer(paragraph+0x78,resource('fot-rodinprob18',1))
    layer=library(0,'layer')
    image=library(1,'image',128)
    warning=library(3,'warning')
    warning_slides=[slide('Slide1',.25),slide('widescreen',.25)];pointer(warning+0x78,ring(warning_slides));pointer(warning+0x7c,warning_slides[0])
    slides=[]
    for title in ('Slide1','ESRB','strap','nunchuk','NLG','art'):
        at=slide(title,start=.125 if title=='ESRB' else 0);slides.append(at)
        root=instance(1,'Layer',layer);pointer(at+8,ring([root]))
        children=[]
        if not(missing and title=='ESRB'):children.append(instance(4,'no home',warning))
        labels=[]
        if title=='strap':labels=[('strap_us','boot-en'),('strap_16_9_us','boot-en')]
        elif title=='art':labels=[(prefix+language,tex) for prefix in ('strap_','strap_16_9_') for language,tex in (('French','boot-fr'),('Spanish','boot-es'))]
        elif title=='nunchuk':labels=[('nunchuk','boot-nunchuk')]
        elif title in ('ESRB','NLG'):labels=[('logo','boot-logo')]
        for label,res in labels:
            child=instance(2,label,image,resource_ids[res]);children.append(child)
            if wrong_type and label=='strap_us':word(child+0x88,5)
        if title in ('nunchuk','ESRB'):
            child=instance(3,'instruction',fallback if title=='nunchuk' else paragraph)
            text='AB' if title=='nunchuk' else 'A{p}B'
            string=alloc(len(text.encode('utf-16-be'))+2)
            data[string:string+len(text)*2]=text.encode('utf-16-be')
            while len(data)%4:data.append(0)
            pointer(child+0x104,string);struct.pack_into('>2f',data,child+0x98,128,64)
            children.append(child)
        pointer(root+8,ring(children))
    pointer(4,0x18);pointer(8,ring(resources));pointer(12,ring(libraries));word(16,87);word(20,len(resources))
    pointer(0x18,ring(slides));pointer(0x1c,slides[0]);table=b''.join(struct.pack('>I',p) for p in sorted(pointers))
    return struct.pack('>4I',0x46454e4c,1,len(data),len(table))+data+table


def files():
    result=visuals();result.update({'Art/fe/boot_loading.fen':scene(),
        'Art/fe/boot-missing.fen':scene(missing=True)})
    colours=(0x7f,0x40,0x12,0xc1,0x53)
    result['Art/fe/BootLoadingUI.res']=bundle([(name_hash(label),texture(value=value)) for label,value in
        zip(('boot-en','boot-fr','boot-es','boot-nunchuk','boot-logo'),colours)])
    return result
