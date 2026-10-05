from pathlib import Path
import argparse,json,struct,subprocess,tempfile

def execute(executable,output,name,header):
    levels,fmt=struct.unpack_from('>2I',header);width,height=struct.unpack_from('>2H',header,14);entries=struct.unpack_from('>I',header,20)[0]
    gx=(4,5,14,6,1,0,1,3,9)[fmt]
    flags=(0 if fmt==8 else 2)|(levels>1)
    mode0=(0x190 if levels==1 else 0x1b0 if fmt==8 else 0x1d0)
    mode1=min(levels-1,10)*16*256
    image0=0xff000000|(width-1)&1023|((height-1)&1023)<<10|(gx&15)<<20
    key,offset,length=0xe34a7201,0x10203040,0x89abcdef
    raw=header+struct.pack('>8I',0x726c7462,0x12345678,0,0,key,offset,length,0)+bytes(range(256))
    raw_path=output/(name+'.raw');raw_path.write_bytes(raw)
    oracle=output/(name+'.oracle');oracle.write_text(' '.join(map(str,[levels,fmt,width,height,entries,header[12],gx,mode0,mode1,int(flags),image0,key,offset,length]))+'\n')
    command=[str(executable),str(raw_path),str(oracle)];r=subprocess.run(command,capture_output=True,text=True,timeout=20)
    (output/(name+'.log')).write_text(r.stdout+r.stderr)
    if r.returncode:raise RuntimeError(name+'\n'+r.stdout+r.stderr)
    return {'name':name,'command':command,'exit':r.returncode,'output':r.stdout,'levels':levels,'format':fmt,'width':width,'height':height,'palette_entries':entries}

def run(executable,output,owned_bundles):
    output.mkdir(exist_ok=True);results=[]
    for fmt in range(9):
        for levels in (1,3,12):
            raw=bytearray(32);struct.pack_into('>2I',raw,0,levels,fmt);raw[8:12]=bytes((5,6,7,8));raw[12]=1
            struct.pack_into('>2H',raw,14,64,32);struct.pack_into('>I',raw,20,256 if fmt==8 else 0)
            results.append(execute(executable,output,f'format-{fmt}-levels-{levels}',bytes(raw)))
    # Raw owned Bundle141 font records are decoded independently here; no
    # production font/resource manager is substituted by this SDK-only test.
    for number,bundle in enumerate(owned_bundles):
        name=f'owned-{number}'
        raw=Path(bundle).read_bytes();sector,count,directory,_=struct.unpack_from('>4I',raw)
        for i in range(count):
            key,block,size=struct.unpack_from('>3I',raw,directory*sector+12*i);record=raw[block*sector:block*sector+size]
            if record.startswith(b'NLG Font'):continue
            results.append(execute(executable,output,name+'-'+f'{key:08x}',record[:32]))
    (output/'results.json').write_text(json.dumps(results,indent=2)+'\n');print('Real SDK texture ABI fixtures passed:',len(results))
if __name__=='__main__':
    parser=argparse.ArgumentParser(description='Raw texture words and actual SDK metadata only; no original game readiness')
    parser.add_argument('executable')
    parser.add_argument('--owned-bundle',action='append',default=[])
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if args.output:run(Path(args.executable).resolve(),args.output.resolve(),args.owned_bundle)
    else:
        with tempfile.TemporaryDirectory(prefix='original-font-texture-abi-') as directory:
            run(Path(args.executable).resolve(),Path(directory),args.owned_bundle)
