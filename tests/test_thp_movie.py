import os,sys,tempfile,subprocess
from pathlib import Path
from disc_fixture import write_disc
from thp_movie_fixture import files,video

def main():
    exe=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='charged-thp-') as tmp:
        root=Path(tmp)
        # The common disc fixture owns FST/alignment independently of THP data.
        write_disc(root/'test.iso',files=files(),fst_capacity=0x1000)
        (root/'zero.video').write_bytes(video())
        (root/'dc.video').write_bytes(video(8))
        (root/'overflow.video').write_bytes(video(width=1024,height=8192,overflow=True))
        result=subprocess.run([str(exe),str(root/'test.iso'),str(root),'generated'],timeout=85)
        return result.returncode
if __name__=='__main__':raise SystemExit(main())
