"""Real NL-backed scene transactions and generated malformed dependency cases."""
from pathlib import Path
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_session_fixture import files

executable=str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix='mscharged-frontend-session-') as folder:
    root=Path(folder)
    for mode in ('success','missing-loc','bad-text','bad-heading','missing-main','bad-main'):
        payloads=files()
        if mode!='success':
            name={'missing-loc':'english.loc','bad-text':'fonts/eurfonttext18.res',
                  'bad-heading':'fonts/eurfontheading36.res','missing-main':'MainUI.Dmn','bad-main':'MainUI.Dmn'}[mode]
            path='Art/fe/'+name
            if mode.startswith('missing'): del payloads[path]
            else: payloads[path]=b'malformed dependency'
        write_disc(root/'session.iso',files=payloads)
        subprocess.run([executable,str(root/'session.iso'),str(root),mode],check=True,timeout=45)
