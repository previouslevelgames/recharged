#!/usr/bin/env python3
"""Original catalog parity and transactional37-camera startup using synthetic tracks."""
from pathlib import Path
import subprocess
import sys
import tempfile
from camera_fixture import camera_fixture
from disc_fixture import write_disc
from frontend_camera_fixture import frontend_camera_catalog, frontend_camera_files

executable=str(Path(sys.argv[1]).resolve())
catalog=frontend_camera_catalog()
actual=subprocess.run([executable,'--catalog'],capture_output=True,text=True,timeout=10)
assert actual.returncode==0,actual.stderr
assert [tuple(line.split('\t')) for line in actual.stdout.splitlines()]==catalog,'Native frontend catalog differs from pinned original filenames/aliases/order'
print('Native frontend catalog exactly matches all37original filename/alias pairs and ordering')
with tempfile.TemporaryDirectory(prefix='mscharged-frontend-camera-') as folder:
    root=Path(folder)
    for mode in ('success','missing-first','missing-last','malformed-last'):
        files=frontend_camera_files()
        if mode!='success':
            path,_=catalog[0 if mode=='missing-first' else -1]
            key='Art/'+path.split('/',1)[1]
            if mode=='malformed-last':files[key]=camera_fixture()[:-1]
            else:del files[key]
        write_disc(root/'catalog.iso',files=files,fst_capacity=0x1000)
        result=subprocess.run([executable,str(root/'catalog.iso'),str(root),mode],timeout=12)
        if result.returncode:sys.exit(result.returncode)
