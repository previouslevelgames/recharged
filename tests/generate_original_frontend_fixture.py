#!/usr/bin/env python3
"""Independent Wii32 FEN encoder. No native decoder/profile code imported."""
from pathlib import Path
import argparse,hashlib,json,struct
parser=argparse.ArgumentParser()
parser.add_argument('output',type=Path)
out=parser.parse_args().output;out.mkdir(parents=True,exist_ok=True)
data=bytearray();offsets={};slots=[]

def alloc(name,size,alignment=4):
    while len(data)%alignment:data.append(0)
    offsets[name]=len(data);data.extend(bytes(size));return offsets[name]
def word(at,value):struct.pack_into('>I',data,at,value&0xffffffff)
def half(at,value):struct.pack_into('>H',data,at,value&0xffff)
def number(at,value):struct.pack_into('>f',data,at,value)
def ptr(at,name=None,listed=True):
    word(at,0xffffffff if name is None and listed else 0 if name is None else offsets[name])
    if listed:slots.append(at)
def name(at,value):
    encoded=value.encode('ascii');assert len(encoded)<32;data[at:at+len(encoded)+1]=encoded+b'\0'
def ring(names,link=0):
    for i,current in enumerate(names):
        ptr(offsets[current]+link,names[(i+1)%len(names)]);ptr(offsets[current]+link+4,names[(i-1)%len(names)])
def attrs(at):
    for i,value in enumerate([1.25,-2.5,-0.,0.1,0.2,0.3,1.,2.,3.,4.,5.,6.]):number(at+4*i,value)
    data[at+48:at+56]=bytes([1,10,20,30,40,0xaa,0xbb,0xcc])
    for i,value in enumerate([0.125,0.25,0.5,0.75]):number(at+56+4*i,value)

alloc('package',24);alloc('presentation',12)
alloc('texture',32);alloc('font',28)
for key,size in [('lib_layer',120),('lib_image',124),('lib_text',136),('lib_component',128),('lib_group',120)]:alloc(key,size)
for key,size in [('layer',144),('image',152),('text',276),('component',144),('group',144)]:alloc(key,size)
for key in ['slide_a','slide_b','slide_component']:alloc(key,72)
for key in ['vector_anim','float_anim']:alloc(key,28)
for key in ['vector0','vector1']:alloc(key,56)
for key in ['float0','float1']:alloc(key,24)
alloc('string',12,2);offsets['suffix']=offsets['string']+2
alloc('matrix',64)

p=offsets['package'];ptr(p);ptr(p+4,'presentation');ptr(p+8,'font');ptr(p+12,'lib_group');word(p+16,0xf1234567);word(p+20,2)
p=offsets['presentation'];ptr(p,'slide_b');ptr(p+4,'slide_a');number(p+8,0.)
ring(['texture','font'])
for key,kind,hash_id in [('texture',0,0xf0000001),('font',1,0xf0000002)]:
    p=offsets[key];word(p+8,kind);word(p+12,hash_id);data[p+16:p+20]=bytes([0,0xa1,0xb2,0xc3]);word(p+20,0xfffffffd)
p=offsets['texture'];word(p+24,0xfa123456);half(p+28,256);half(p+30,128)
ptr(offsets['font']+24,None,False)
libs=['lib_layer','lib_image','lib_text','lib_component','lib_group'];ring(libs)
for kind,key in enumerate(libs):
    p=offsets[key];attrs(p+8);word(p+0x50,0xfab00000+kind);name(p+0x54,key);word(p+0x74,kind)
ptr(offsets['lib_image']+120,'texture');ptr(offsets['lib_text']+120,'font')
p=offsets['lib_text'];data[p+124:p+128]=bytes([4,3,2,1]);number(p+128,1280.);number(p+132,480.)
ptr(offsets['lib_component']+120,'slide_component');ptr(offsets['lib_component']+124,'slide_component')

ring(['layer']);ring(['image','text']);ring(['component']);ring(['group'])
for key,kind,lib,children in [('layer',1,'lib_layer','text'),('image',2,'lib_image',None),('text',3,'lib_text',None),('component',4,'lib_component',None),('group',5,'lib_group',None)]:
    p=offsets[key];ptr(p+8,children);ptr(p+12,lib);number(p+16,0.);number(p+20,9.);name(p+24,key);word(p+56,0xc1230000+kind);attrs(p+60);word(p+132,0x123);word(p+136,kind);half(p+140,0xabcd);data[p+142:p+144]=bytes([1,0xd9])
ptr(offsets['image']+144,'texture');word(offsets['image']+148,0xf2345678)
p=offsets['text'];word(p+144,0xff012345);data[p+148:p+152]=bytes([0xde,0xad,0xbe,0xef]);number(p+152,640.);number(p+156,200.);word(p+160,0xf1234560)
ptr(p+164,None,False);ptr(p+168,'string');ptr(p+172,'matrix');word(p+176,0xa9876543);half(p+180,3);half(p+182,-17)
for i in range(17):half(p+184+4*i,-123+i);half(p+186+4*i,i*3)
ptr(p+252,None);word(p+256,0xf0123456);ptr(p+260,'suffix');data[p+264:p+266]=bytes([1,0x7f])
for i,v in enumerate([11,22,333,444]):half(p+266+2*i,v)
data[p+274:p+276]=bytes([0x99,0x88])

ring(['slide_a','slide_b']);ring(['slide_component'])
for key,children,animation,mode in [('slide_a','layer','float_anim',1),('slide_b','component',None,0),('slide_component','group',None,2)]:
    p=offsets[key];ptr(p+8,children);ptr(p+12,animation);number(p+16,0.);number(p+20,2.);number(p+24,0.);word(p+28,mode);name(p+32,key);word(p+64,{'slide_a':0x11111111,'slide_b':0x22222222,'slide_component':0x33333333}[key]);data[p+68:p+72]=bytes([0,0x66,0x77,0x88])
ring(['vector_anim','float_anim'],4)
for key,cast,target,typ,keys in [('vector_anim',1,'image',1,'vector1'),('float_anim',0,'text',6,'float1')]:
    p=offsets[key];word(p,0x81234567);ptr(p+12,target);half(p+16,cast);data[p+18:p+20]=b'\xa5\x5a';word(p+20,typ);ptr(p+24,keys)
ring(['vector0','vector1'],48);ring(['float0','float1'],16)
for key,frame in [('vector0',0),('vector1',1)]:
    p=offsets[key]
    for channel in range(3):
        for part,value in enumerate([float(channel+1+frame*10),float(channel+1),float(channel+2),float(frame)]):number(p+16*channel+4*part,value)
for key,frame in [('float0',0),('float1',1)]:
    p=offsets[key]
    for part,value in enumerate([64.+64.*frame,64.,96.,float(frame)]):number(p+4*part,value)
for i,unit in enumerate([ord('A'),ord('B'),0xd83d,0xde00,ord('Z'),0]):half(offsets['string']+2*i,unit)
for i in range(16):number(offsets['matrix']+4*i,i-4.5)
while len(data)%4:data.append(0)
assert len(slots)==len(set(slots))
# Deliberately reversed source table order, preserving all aliases and nulls.
table=b''.join(struct.pack('>I',at) for at in reversed(slots))
raw=struct.pack('>4sIII',b'FENL',1,len(data),len(table))+data+table
(out/'generated.fen').write_bytes(raw)
oracle={'encoder':'Independent big-endian Python literal Wii schema; no native converter/profile implementation.','bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest(),'offsets':offsets,'relocation_order':list(reversed(slots)),'source_runtime_checks':{'half_second_fade':0.5,'half_second_slide':1.,'image_position':[11.,12.,13.],'text_alpha':128,'subsequent_fade':0.625,'subsequent_slide_time':0.75,'interpolated_image':[5.640625,6.640625,7.640625],'interpolated_opacity_byte':104,'original_bezier_mu1':3.0}}
(out/'generated-oracle.json').write_text(json.dumps(oracle,indent=2)+'\n')
header='#pragma once\n#include <cstdint>\n'
for key,at in offsets.items():header+=f'inline constexpr std::uint32_t gen_{key}={at};\n'
(out/'generated_offsets.h').write_text(header)
print('Independent generated raw FEN:',len(raw),'bytes,',len(slots),'relocations,',len(offsets),'named records including suffix alias')
