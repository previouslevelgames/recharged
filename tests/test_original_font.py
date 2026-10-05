"""Generated font descriptors for the original source parser; no retail bytes."""
from pathlib import Path
import importlib.util
import json
import re
import struct
import subprocess
import sys
import tempfile

from disc_fixture import write_disc
from frontend_visual_fixture import name_hash

def f32(value):return struct.unpack('<f',struct.pack('<f',value))[0]
def bits(value):return struct.unpack('<I',struct.pack('<f',value))[0]
def char(token):return ord(token) if len(token)==1 else int(token)

def descriptor_oracle(data,base,alias,destination):
    glyphs={};pairs=[];page=0;x=y=0;version=0
    for line in data.decode('ascii').splitlines():
        if line.startswith('Version '):version=float(line.split()[1])
        elif line.startswith('PageSize '):
            _,size,_,pages,_,typ,_,distribution=line.split();size=int(size);pages=int(pages)
        elif line.startswith('Height '):
            fields=line.split();height,renderheight,ascent,renderascent,leading=map(int,fields[1::2])
        elif line.startswith('CharSpacing '):
            fields=line.split();spacing=f32(int(fields[1])/100);lineheight=f32(int(fields[3])/100)
        elif line=='PageBreak':page+=1
        elif line.startswith('Glyph '):
            matched=re.fullmatch(r'Glyph (.+) Width (\d+) (\d+) (-?\d+)(?: RenderHeight (\d+) RenderAscent (\d+) Pos (\d+) (\d+))?',line)
            if not matched:raise ValueError(line)
            token,advance,width,offset,gh,ga,xx,yy=matched.groups();code=char(token)
            advance,width,offset=int(advance),int(width),int(offset)
            if version<1.2:
                if x+width>size:
                    x=0;y+=renderheight
                    if y+renderheight>size:x=y=0;page+=1
                gh,ga=renderheight,renderascent
            else:gh,ga,x,y=map(int,(gh,ga,xx,yy))
            # Independent descriptor geometry: inverse and per-coordinate
            # operations use single precision, as the source's Wii scalars do.
            inverse=f32(1/size);u=f32(x*inverse);v=f32(y*inverse)
            glyphs[code]=dict(code=code,advance=advance,width=width,height=gh,ascent=ga,offset=offset,page=page,kern=0,
                uv=[bits(u),bits(v),bits(f32(u+f32((width-1)*inverse))),bits(f32(v+f32((gh-1)*inverse)))])
            x+=width
        elif line.startswith('Kern '):
            fields=line[5:].split(' ');basechar=char(fields[0]);glyphs[basechar]['kern']=1
            for b,k in zip(fields[1::2],fields[2::2]):pairs.append((basechar,char(b),int(k)))
    ordered=sorted(glyphs);pairs.sort(key=lambda p:(p[0]<<16)|p[1])
    lookup={(a,b):k for a,b,k in pairs}
    def width(text):
        result=0;prev=0
        for c in text:
            glyph=glyphs.get(c,glyphs[ord('?')]);value=(glyph['advance']+glyph['offset']+lookup.get((prev,c),0))&0xffffffff
            result=(result+int(f32(f32(value)*spacing)))&0xffffffff;prev=c
        return result
    queries=[]
    # Independent fixed equations; do not reproduce the source wrapping loop.
    for text in ('AB','BA','A A','A?B','abc','Strikers'):
        value=width(map(ord,text));bound=max(4096,value+1)
        queries.append((bound,1,0,value,1,int(f32(height*spacing)),list(map(ord,text))))
        queries.append((bound,0,1,value,1,int(f32(height*spacing)),list(map(ord,text))))
    # The reconstructed source deliberately excludes an exact maximum row
    # width from GetStringWidth while GetStringLineCount remains one.
    text=list(map(ord,'AB'));value=width(text)
    queries.append((value,0,1,0,1,int(f32(height*spacing)),text))
    # Source escape traversal skips colour identifiers when measuring text.
    # The escape constructor itself must retain the original BE identifier.
    text='A{clr:ff0011}B';value=width(map(ord,'AB'))
    queries.append((4096,1,0,value,1,int(f32(height*spacing)),list(map(ord,text))))
    for text in ('AB','BA'):
        original_width=width(map(ord,text));with_nbs=list(map(ord,text[0]+'{nbs}'+text[1]))
        queries.append((4096,1,0,original_width,1,int(f32(height*spacing)),with_nbs))
    lines=[f'{pages} {size} {dict(color=1,greyscale=2,split=3)[typ]} {dict(english=1,inorder=2)[distribution]} {height} {ascent} {leading} {bits(spacing)} {bits(lineheight)}']
    for i in range(pages):lines.append(f'{name_hash(base+"_"+str(i+1))} {name_hash(base+"_"+str(i+1)+"e")}')
    lines.append(str(len(ordered)))
    for c in ordered:
        g=glyphs[c];lines.append(' '.join(map(str,[c,g['advance'],g['width'],g['height'],g['ascent'],g['offset'],g['page'],g['kern'],*g['uv']])))
    lines.append(str(len(pairs)));lines.extend(' '.join(map(str,p)) for p in pairs)
    lines.append(str(len(queries)))
    for bound,single,wrap,w,l,h,text in queries:lines.append(' '.join(map(str,[bound,single,wrap,w,l,h,len(text),*text])))
    destination.write_text('\n'.join(lines)+'\n')
    return {'glyph_count':len(ordered),'kerning_count':len(pairs),'queries':len(queries)}

def nl_bundle(base,desc):
    records=[(name_hash(base),desc)]
    raw=bytearray(64);struct.pack_into('>4I',raw,0,32,len(records),1,2)
    for i,(key,data) in enumerate(records):
        raw.extend(b'\0'*(-len(raw)%32));struct.pack_into('>3I',raw,32+i*12,key,len(raw)//32,len(data));raw.extend(data)
    return raw

def run(executable,output,owned=None,bundle_reader=None):
    output.mkdir(exist_ok=True);results=[]
    def execute(disc,nl_path,base,alias,oracle,name):
        command=[str(executable),str(disc),nl_path,base,alias,str(oracle)]
        result=subprocess.run(command,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=45)
        (output/(name+'.log')).write_text(result.stdout)
        results.append({'name':name,'command':command,'exit':result.returncode,'output':result.stdout})
        if result.returncode:raise RuntimeError(name+'\n'+result.stdout)
    descriptions={
        'packed-kerning':('NLG Font Description file\r\nVersion 1.1\r\nPageSize 32 PageCount 2 TexType color Distribution english\r\n'
            'Height 12 RenderHeight 16 Ascent 9 RenderAscent 11 IL 1\r\nCharSpacing 125 LineHeight 200\r\n'
            'Glyph ? Width 8 8 0\r\nGlyph A Width 10 8 1\r\nGlyph B Width 12 8 -1\r\nGlyph 32 Width 4 0 0\r\n'
            'Glyph 233 Width 12 8 -1\r\nGlyph 338 Width 10 8 0\r\nKern A B -2 ? 1\r\nKern B A -1\r\nEND'),
        'split-pages':('NLG Font Description file\r\nVersion 1.1\r\nPageSize 32 PageCount 2 TexType split Distribution inorder\r\n'
            'Height 12 RenderHeight 16 Ascent 9 RenderAscent 11 IL 1\r\nCharSpacing 100 LineHeight 100\r\n'
            'Glyph ? Width 8 8 0\r\nGlyph A Width 10 8 0\r\nGlyph 32 Width 4 0 0\r\nPageBreak\r\nGlyph B Width 12 8 0\r\nEND'),
        'authored-12':('NLG Font Description file\r\nVersion 1.2\r\nPageSize 64 PageCount 1 TexType greyscale Distribution english\r\n'
            'Height 16 RenderHeight 20 Ascent 11 RenderAscent 14 IL 2\r\nCharSpacing 100 LineHeight 125\r\n'
            'Glyph ? Width 10 8 0 RenderHeight 15 RenderAscent 12 Pos 4 9\r\n'
            'Glyph A Width 12 10 2 RenderHeight 18 RenderAscent 14 Pos 22 13\r\n'
            'Glyph B Width 12 11 -1 RenderHeight 19 RenderAscent 15 Pos 37 17\r\n'
            'Glyph 32 Width 4 0 0 RenderHeight 0 RenderAscent 0 Pos 0 0\r\n'
            'Glyph 233 Width 12 11 0 RenderHeight 18 RenderAscent 15 Pos 18 44\r\nEND'),
    }
    for name,description in descriptions.items():
        base='fe/fonts/'+name;alias='text';raw=nl_bundle(base,description.encode());disc=output/(name+'.iso');write_disc(disc,files={'Art/font.res':raw})
        oracle=output/(name+'.oracle');facts=descriptor_oracle(description.encode(),base,alias,oracle)
        execute(disc,'/Art/font.res',base,alias,oracle,name);results[-1].update(facts)
    if owned:
        for name in ('eurfonttext18','eurfontheading36'):
            raw_path=output/(name+'.owned.res')
            subprocess.run([str(bundle_reader),'--dump',str(owned),'/Art/fe/fonts/'+name+'.res',str(raw_path)],check=True,timeout=45)
            raw=raw_path.read_bytes();sec,count,directory,_=struct.unpack_from('>4I',raw);base='fe/fonts/'+name
            for i in range(count):
                key,block,size=struct.unpack_from('>3I',raw,directory*sec+12*i)
                if key==name_hash(base):description=raw[block*sec:block*sec+size];break
            oracle=output/(name+'.oracle');facts=descriptor_oracle(description,base,'text' if 'text' in name else 'heading',oracle)
            execute(owned,'/Art/'+base+'.res',base,'text' if 'text' in name else 'heading',oracle,name);results[-1].update(facts)
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n');print('Source font parser/metrics fixtures passed:',len(results))

if __name__=='__main__':
    executable=Path(sys.argv[1]).resolve()
    owned=Path(sys.argv[3]).resolve() if len(sys.argv)==6 and sys.argv[2]=='--owned' else None
    bundle_reader=Path(sys.argv[5]).resolve() if owned and sys.argv[4]=='--bundle-reader' else None
    with tempfile.TemporaryDirectory(prefix='charged-original-font-') as folder:
        run(executable,Path(folder),owned,bundle_reader)
