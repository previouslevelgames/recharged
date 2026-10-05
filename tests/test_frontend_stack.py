"""Source-named scene associations over generated Wii files; no retail bytes."""
from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
from disc_fixture import write_disc
from frontend_handler_fixture import files, notification

executable = str(Path(sys.argv[1]).resolve())
# Independent audit of all 107 unpatched SceneEntryTable records at 9f985f9a:
# numeric SceneList value + ':' + literal filename (or '-') + newline. This
# fingerprint includes null slots and aliases, not just title/main spot checks.
catalog = subprocess.check_output([executable, '--catalog'], timeout=15)
assert hashlib.sha256(catalog).hexdigest() == '3911aee4aa4d672093e1f1d46df1fbed5d5687634bed15f71ab5b7bc9aba9a95'

with tempfile.TemporaryDirectory(prefix='mscharged-frontend-stack-') as directory:
    root = Path(directory)
    payloads = files()
    for name in ('sms2_start.fen', 'main_menu_v3.fen'):
        payloads['Art/fe/' + name] = notification()
    payloads['Art/fe/options_main_menu.fen'] = b'invalid native scene header and payload'
    write_disc(root / 'stack.iso', files=payloads)
    subprocess.run([executable, str(root / 'stack.iso'),
                    str(root), 'generated'], check=True, timeout=90)
