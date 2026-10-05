"""Independent raw-code-unit oracle for original NL/MSL wrapper behavior."""
from pathlib import Path
import json, subprocess, sys
exe=Path(sys.argv[1]).resolve()
r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=15)
assert r.returncode==0,(r.returncode,r.stderr)
assert r.stderr=="Actual original print source: font 37 0x2a\n",r.stderr
rows={}
for line in r.stdout.splitlines():
    name,ret,*values=line.split()
    assert name not in rows,name
    rows[name]=(int(ret),list(map(int,values)))
checks=0

def expected(name,text,count=80,width=1,length=96,ret=None,initial=None,truncated=False,terminate=True):
    global checks
    marker=33 if width==1 else 0x55aa
    data=[marker]*length
    if initial is not None:
        data[1:1+len(initial)]=initial
    codes=list(text if isinstance(text,list) else map(ord,text))
    if ret is None:ret=len(codes)
    if terminate:
        payload=codes[:max(count-1,0)]+[0]
        data[1:1+len(payload)]=payload
        # Original nlSNPrintf/nlVSNPrintf themselves always write this byte,
        # beyond the underlying formatter's terminator at the actual end.
        data[count]=0
    actual=rows.pop(name)
    assert actual[0]==ret,(name,actual[0],ret)
    assert actual[1]==data,(name,[(i,a,b)for i,(a,b)in enumerate(zip(actual[1],data))if a!=b])
    checks+=1+len(data)

expected('narrow_alias','prefix_',8,length=12,ret=10,initial=list(b'prefix\0'))
# Original nlVSNPrintf passes size-1: one byte less reaches libc/MSL.
expected('narrow_v_alias',list(b'prefix\0'),8,length=12,ret=10,initial=list(b'prefix\0'))
expected('narrow_nul',[0,ord('X')],8,length=12)
expected('narrow_empty','',8,length=12)
expected('narrow_one','',1,length=12,ret=4)
expected('narrow_v_one','',1,length=12,ret=4)
expected('narrow_values','-31/4026531841/0x2a/3.50/font')
expected('narrow_font','fe/fonts/eurfonttext18_1',initial=list(b'fe/fonts/eurfonttext18\0'))
expected('narrow_suffix','child:002a',initial=list(b'parent/child\0'))
expected('host_count_zero','',0,length=12,ret=8,terminate=False)
expected('wide_literal','A\u03a9\u6f22%',width=2)
# Original C-locale no-conversion maps each narrow byte to one Wii code unit.
expected('wide_strings','[\u00c4\u00e9][M\u03a9\u6f22]',width=2)
expected('wide_integer','-31/4026531841/0x2a/-1/65535',width=2)
expected('wide_padding','[00042][abc     ]',width=2)
expected('wide_characters','\u03a9/!/\u6f22',width=2)
expected('wide_decimal','3.50/-0.0/1.25e+02/0.125',width=2)
expected('wide_hex','0x1.c00p+1/-0X1.000P-1',width=2)
expected('wide_nonfinite','inf/-INF/nan',width=2)
expected('wide_native_varargs','-31/4026531841/1234567890123/3.500000',width=2)
expected('wide_empty','',width=2)
expected('wide_nul',[0,ord('X')],width=2)
# MSL wide formatter returns -1 when its requested capacity is insufficient.
expected('wide_truncated','prefix_',8,width=2,ret=-1)
expected('wide_one','',1,width=2,ret=-1)
expected('wide_alias','prefix_1',width=2,initial=list(map(ord,'prefix\0')))
expected('wide_count','ABCD',width=2)
assert rows.pop('wide_counts')==(2,[3,4]);checks+=3
assert not rows,rows
print(json.dumps({'status':'pass','records':26,'assertions':checks,'scope':'whole original NL narrow/wide wrappers and genuine isolated MSL formatter; no font readiness or rendering'}))
