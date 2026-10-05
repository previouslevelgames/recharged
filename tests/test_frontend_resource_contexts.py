"""Actual source-state/context ordering over generated original-format NL assets."""
from pathlib import Path
import subprocess, sys, tempfile
from disc_fixture import write_disc
from frontend_resource_context_fixture import files

executable=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='mscharged-fe-resource-context-') as folder:
    root=Path(folder)
    for mode in ('success','missing-loc','bad-font','bad-images','short-images'):
        payloads=files()
        if mode=='missing-loc':del payloads['Art/fe/english.loc']
        if mode=='bad-font':payloads['Art/fe/fonts/eurfonttext18.res']=b'bad font'
        if mode=='bad-images':payloads['Art/fe/MainUI.Dmn']=b'malformed but nonempty image bundle'
        if mode=='short-images':payloads['Art/fe/MainUI.Dmn']=b'too short'
        write_disc(root/'context.iso',files=payloads,fst_capacity=0x1000,partition_size=0x10000)
        subprocess.run([executable,str(root/'context.iso'),str(root),mode],check=True,timeout=45)
