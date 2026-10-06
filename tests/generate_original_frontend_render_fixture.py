#!/usr/bin/env python3
"""Raw font descriptor geometry oracles for whole original render providers."""
from pathlib import Path
import hashlib, json, re
import generate_original_font_loading_fixture as g


def generate(output, owned=None, owned_disc=None):
    g.generate(output, owned, owned_disc)
    manifest=json.loads((output/'manifest.json').read_text())
    rows=manifest['records']; records=[]
    for i,row in enumerate(rows):
        if row['private']:
            raw=(Path(owned)/Path(row['bundle']).name).read_bytes()
            assert hashlib.sha256(raw).hexdigest()==row['bundle_sha256']
            data=g.payloads(raw)[g.name_hash(row['descriptor'])]
        else:
            data=g.descriptor(row['facts']['pages'],{1:'color',2:'greyscale',3:'splitfx'}[row['facts']['texture_type']],row['facts']['height'])
        assert g.data_hash(data)==row['facts']['descriptor_hash']
        lines=data.decode('ascii').splitlines(); version=float(next(s for s in lines if s.startswith('Version ')).split()[1])
        page_size=int(next(s for s in lines if s.startswith('PageSize ')).split()[1])
        metrics=next(s for s in lines if s.startswith('Height ')).split()
        render_height,render_ascent=int(metrics[3]),int(metrics[7])
        x=y=page=0; glyphs={}; kern={}
        for line in lines:
            if line.startswith('PageBreak'): page+=1
            m=re.match(r'^Glyph (.*?) Width (\d+) (\d+) (-?\d+)(.*)$',line)
            if m:
                ch=int(m[1]) if len(m[1])!=1 else ord(m[1])
                adv,width,offset=int(m[2]),int(m[3]),int(m[4]);rh,ra=render_height,render_ascent
                if version<1.1999:
                    if x+width>page_size:
                        x=0;y+=render_height
                        if y+render_height>page_size:x=y=0;page+=1
                else:
                    e=re.search(r'RenderHeight (\d+) RenderAscent (\d+) Pos (\d+) (\d+)',m[5]);assert e,line
                    rh,ra,x,y=map(int,e.groups())
                glyphs[ch]=[page&15,adv&255,width&255,rh&255,ra&255,offset,
                            g.bits(x/page_size),g.bits(y/page_size),g.bits((x+width-1)/page_size),g.bits((y+rh-1)/page_size)]
                x+=width
            m=re.match(r'^Kern (.) (.) (-?\d+)$',line)
            if m:kern[ord(m[1]),ord(m[2])]=int(m[3])
        selected=[glyphs.get(c,glyphs[ord('?')]) for c in (65,66,63)]
        p=Path(row['oracle'])
        r=p.with_suffix('.oracle.render')
        r.write_text(str(kern.get((65,66),0))+'\n'+'\n'.join(' '.join(map(str,q)) for q in selected)+'\n')
        records.append(dict(index=i,descriptor=row['descriptor'],sha256=hashlib.sha256(data).hexdigest(),glyphs=selected,kern_ab=kern.get((65,66),0),oracle_sha256=hashlib.sha256(r.read_bytes()).hexdigest()))
    (output/'render-oracles.json').write_text(json.dumps(dict(scope='Independent raw glyph/kerning/packed-page grammar; owned bytes remain private.',records=records),indent=2)+'\n')

if __name__ == "__main__":
    import sys
    generate(Path(sys.argv[1]),Path(sys.argv[2]) if len(sys.argv)>2 else None,Path(sys.argv[3]) if len(sys.argv)>3 else None)
