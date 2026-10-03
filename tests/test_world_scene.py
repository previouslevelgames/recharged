"""Cross-file world assembly, instance transforms, and optional real GPU preview."""
import json
import math
import re
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import zlib
from disc_fixture import write_disc
from camera_fixture import camera_fixture
from world_scene_fixture import world_scene_fixture

mode, executable = sys.argv[1], str(Path(sys.argv[2]).resolve())
with tempfile.TemporaryDirectory(prefix='mscharged-world-scene-') as directory:
    root = Path(directory)
    resident, temporary = world_scene_fixture()
    if mode == '--reader':
        def run(r=resident, t=temporary, ids=('10', '20'), error=None):
            (root/'res').write_bytes(r)
            (root/'tmp').write_bytes(t)
            result = subprocess.run([executable, str(root/'res'), str(root/'tmp'), *ids], capture_output=True, text=True, timeout=15)
            if error:
                assert result.returncode == 1 and error in result.stderr, (result.stdout, result.stderr)
                return
            assert result.returncode == 0, result.stderr
            return json.loads(result.stdout)
        decoded = run()
        assert (decoded['objects'], decoded['models'], decoded['textures'], decoded['animations']) == (2, 1, 1, 0)
        assert decoded['ids'] == [0x10, 0x20]
        expected = [0, .1, -.5, math.sqrt(3.4**2 + 1.6**2 + 1)/2]
        assert all(abs(a-b)<1e-6 for a,b in zip(decoded['bounds'], expected)), decoded
        assert decoded['positions'] == [-1, -.699999988, 0, 1, -.699999988, 0, 0, .899999976, 0]
        assert run(ids=('20','10'))['ids'] == [0x20,0x10]
        assert run(ids=('10',))['objects'] == 1
        # Original DrawToView replaces the stored packet matrix. It must not be
        # baked and then applied a second time through the instance transform.
        stored = [2,0,0,0, 0,0,3,0, 0,-4,0,0, 100,200,300,1]
        r,t=world_scene_fixture(stored_transform=stored, lit=True)
        local = run(r,t)
        assert local['positions'] == decoded['positions'] and local['bounds'] == decoded['bounds']
        assert local['normals'] == [0,0,1]*3
        r,t=world_scene_fixture(animated=True)
        assert (run(r,t)['textures'],run(r,t)['animations']) == (2,1)
        run(ids=('10','10'), error='Duplicate selected world object ID')
        run(ids=('deadbeef',), error='Selected world object is absent')
        run(ids=('10',)*257, error='1..256 explicit object IDs')
        r,t=world_scene_fixture(missing_model=True)
        run(r,t,error='model 0x87654322: Requested model ID is absent')
        run(resident[:-1], error='chunk exceeds its container')
        run(t=temporary[:-1], error='chunk exceeds its container')
        bad=bytearray(temporary);bad[32:36]=bytes.fromhex('badc0ffe')
        run(t=bad,error='Requested texture')
        bad=bytearray(resident);struct.pack_into('>I',bad,32+8,0x108)
        run(r=bad,error='not a supported static drawable')
        # Scaling and translation are applied only to bounds, never to source geometry.
        bad=bytearray(resident);struct.pack_into('>f',bad,32+0x20,2)
        scaled=run(r=bad,ids=('10',))
        assert abs(scaled['bounds'][0]+.7)<1e-6
        assert abs(scaled['bounds'][3]-math.sqrt(4**2+1.6**2)/2)<1e-6
        assert scaled['positions'] == decoded['positions']
        bad=bytearray(resident);struct.pack_into('>f',bad,32+0x50,1e7)
        run(r=bad,error='Transformed world geometry exceeds its preview range')
        print('World assembly: shared models, texture/IFL dependencies, independent transformed bounds, ownership and explicit rejection checks passed')
    elif mode == '--gpu':
        # Cover the fixed EFB sample grid in both the Z-up orbit and authored view.
        def gpu_fixture(**kwargs):
            return world_scene_fixture(spacing=.3, depth_step=0, **kwargs)
        resident, temporary = gpu_fixture()
        disc, config = root/'world.iso', root/'settings.ini'
        config.write_text('; preserve settings\n[game]\ndisc=world.iso\n')
        original = config.read_bytes()
        base = [executable,'--experimental-scene','--config',str(config),'--frames','30']
        selection = ['--world','/world.tmp.zlib','--world-res','/world.res.zlib','--object-id','10','--object-id','20']
        def write(r=resident,t=temporary,cam=None):
            def compressed(data):
                return struct.pack('>I', len(data)) + zlib.compress(data)
            files={'world.res.zlib':compressed(r),'world.tmp.zlib':compressed(t)}
            if cam is not None:files['camera.cam']=cam
            write_disc(disc,files=files)
        def run(args=selection, code=0, message='Selected static world objects: 2 instances, 1 shared models'):
            r=subprocess.run(base+args,capture_output=True,text=True,timeout=45)
            output=r.stdout+r.stderr
            assert r.returncode == code and message in output, (r.returncode,output)
            assert 'VUID-' not in output and 'Validation Error' not in output, output
            assert config.read_bytes() == original
            if code == 0:
                assert 'Static preview rendered: 30 frames' in output and 'shutdown recovered both game arenas' in output, output
            print(message)
            return output
        write(cam=camera_fixture(preview=True));run();run(selection+['--camera','/camera.cam'])
        run(selection+['--debug-camera'],message='Original DebugCam: SDL keyboard/gamepad controls')
        run(selection+['--debug-camera','--no-world-culling'])
        r,t=gpu_fixture(alpha=True);write(r,t,camera_fixture(preview=True));run(selection+['--camera','/camera.cam'])
        r,t=gpu_fixture(animated=True);write(r,t,camera_fixture(preview=True));run(selection+['--camera','/camera.cam'],message='2 textures, 1 texture animations; original radius')
        # Applying this stored translation would move both objects out of view.
        r,t=gpu_fixture(stored_transform=[1,0,0,0, 0,1,0,0, 0,0,1,0, 100,200,300,1])
        write(r,t,camera_fixture(preview=True));run(selection+['--camera','/camera.cam'])

        # The authored camera must control the actual scene submission. Move
        # the second instance fully outside it while retaining a visible first.
        r,t=gpu_fixture();r=bytearray(r)
        struct.pack_into('>f',r,32+0x70+0x50,100)
        write(r,t,camera_fixture(preview=True))
        culled=run(selection+['--camera','/camera.cam'])
        reference=run(selection+['--camera','/camera.cam','--no-world-culling'])
        assert 'Static world submission totals: 30 / 60 objects, 30 packets.' in culled, culled
        assert 'Static world submission totals: 60 / 60 objects, 60 packets.' in reference, reference
        draws=lambda text:int(re.search(r'30 frames, (\d+) GX draw calls',text)[1])
        assert 0<draws(culled)<draws(reference), (culled,reference)
        run(['--no-world-culling'],2,'requires a world object selection')
        run(selection+['--model-id','87654321'],2,'cannot be combined')
        run(['--object-id','10'],2,'World objects require')
        run(selection+['--object-id','10'],1,'Duplicate selected world object ID')
        run(selection+['--object-id','deadbeef'],1,'Selected world object is absent')
        r,t=world_scene_fixture(missing_model=True);write(r,t);run(code=1,message='Requested model ID is absent')
        write(t=temporary[:-1]);run(code=1,message='chunk exceeds its container')
        print('Synthetic static world scene GPU and unchanged-settings checks passed')
    else:
        raise ValueError(mode)
