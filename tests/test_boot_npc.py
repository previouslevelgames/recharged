#!/usr/bin/env python3
"""Synthetic NPC resources and source-ordered boot bytecode; no game data."""
from pathlib import Path
import struct,subprocess,sys,tempfile,zlib
from disc_fixture import write_disc

def words(*values):return struct.pack('>'+str(len(values))+'I',*values)
def script(names=(('npc_a',False),('npc_b',True)),tail=None):
    strings=bytearray();code=[8<<11|4]
    for name,persistent in names:
        offset=len(strings);strings.extend(name.encode()+b'\0');code += [3<<11|offset,2<<11|int(persistent),8<<11|1]
    # Original relative branches: skip begin/finish to select; true goes back3.
    code += tail if tail is not None else [5<<11|3,8<<11|138,8<<11|76,8<<11|108,6<<11|3,8<<11|81,10<<11]
    return words(0xe11c2112,1,0,0,0,len(code)*2,len(strings),0,0,0,0,0,0,0,0,0,0,0)+struct.pack('>IIHBB',0xb53474ff,0,2,0,0)+struct.pack('>'+str(len(code))+'H',*code)+strings

def chunk_offset(data,wanted):
    def walk(begin,end):
        at=begin
        while at<end:
            kind,size=struct.unpack_from('>II',data,at)
            if kind==wanted:return at+8
            if kind&0x80000000:
                found=walk(at+8,at+8+size)
                if found is not None:return found
            at=(at+8+size+3)&~3
        return None
    return walk(0,len(data))

def fixture(directory):
    data={}
    for name in ('npc_a','npc_b'):
        data[f'art/animation/{name}.shier']=(directory/f'{name}.shier').read_bytes()
        for kind in ('rlg','rlt'):data[f'art/characters/npcs/{name}/{name}.{kind}']=(directory/f'{name}.{kind}').read_bytes()
    animation=(directory/'npc_b.sanim').read_bytes()
    data['art/animation/npc_b.sanim.zlib']=words(len(animation))+zlib.compress(animation)
    return data

def main(executable,out):
    out.mkdir(parents=True,exist_ok=True);assets=out/'assets';subprocess.run([str(executable),'--fixtures',str(assets)],check=True)
    modes=('valid','cancel','missing_hierarchy','missing_textures','missing_models','empty_animation','corrupt_animation','wrong_hierarchy','truncated_model','unsupported','collision','oom','slots','begin_before_select','finish_before_begin','double_begin','select_pending','invalid_name','empty_names')
    for mode in modes:
        data=fixture(assets);code=script()
        if mode=='missing_hierarchy':del data['art/animation/npc_b.shier']
        if mode in ('missing_textures','missing_models'):
            kind='rlt' if mode=='missing_textures' else 'rlg';del data[f'art/characters/npcs/npc_b/npc_b.{kind}']
        if mode in ('empty_animation','corrupt_animation'):data['art/animation/npc_b.sanim.zlib']=b'' if mode=='empty_animation' else words(100)+b'bad zlib'
        if mode=='wrong_hierarchy':data['art/animation/npc_b.shier']=data['art/animation/npc_a.shier']
        if mode=='truncated_model':data['art/characters/npcs/npc_b/npc_b.rlg']=data['art/characters/npcs/npc_b/npc_b.rlg'][:-1]
        if mode=='unsupported':
            key='art/characters/npcs/npc_a/npc_a.rlg';model=bytearray(data[key]);struct.pack_into('>I',model,chunk_offset(model,0x1b004)+16,0x22cadb20);data[key]=model
        if mode=='collision':data['art/characters/npcs/npc_a/npc_a.rlt']=data['art/characters/npcs/npc_b/npc_b.rlt']
        if mode=='begin_before_select':code=script(tail=[8<<11|138,10<<11])
        if mode=='finish_before_begin':code=script(tail=[8<<11|108,8<<11|76,10<<11])
        if mode=='double_begin':code=script(tail=[8<<11|108,8<<11|138,8<<11|138,10<<11])
        if mode=='select_pending':code=script(tail=[8<<11|108,8<<11|138,8<<11|108,10<<11])
        if mode=='invalid_name':code=script((('../npc_b',True),))
        if mode=='empty_names':code=script((('',True),))
        disc=out/(mode+'.iso');bytecode=out/(mode+'.byte_code');write_disc(disc,files=data,fst_capacity=0x800,partition_size=0x10000);bytecode.write_bytes(code)
        subprocess.run([str(executable),str(disc),str(bytecode),mode],check=True,timeout=35)
        print(mode+' passed',flush=True)
if __name__=='__main__':
    exe=Path(sys.argv[1]).resolve()
    if len(sys.argv)>2:main(exe,Path(sys.argv[2]))
    else:
        with tempfile.TemporaryDirectory(prefix='charged-boot-npc-') as out:main(exe,Path(out))
