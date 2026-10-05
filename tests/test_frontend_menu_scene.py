"""Opt-in owned-disc Vulkan flow; isolate all generated settings and logs."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile

executable = Path(sys.argv[1]).resolve()
config = Path(sys.argv[2]).resolve()
original = config.read_bytes()
with tempfile.TemporaryDirectory(prefix='charged-menu-vulkan-') as folder:
    isolated = Path(folder) / executable.name
    shutil.copyfile(executable, isolated)
    isolated.chmod(0o755)
    result = subprocess.run([str(isolated), str(config)], capture_output=True,
                            text=True, timeout=180,
                            env={**os.environ, 'SDL_AUDIODRIVER': 'dummy',
                                 'VK_INSTANCE_LAYERS': 'VK_LAYER_KHRONOS_validation'})
    output = result.stdout + result.stderr
    print(output)
    assert config.read_bytes() == original, 'The input INI was modified'
    assert result.returncode == 0, (result.returncode, output)
    assert 'VUID-' not in output and 'Validation Error' not in output, output
    assert 'shutdown recovered both game arenas' in output, output
    assert 'Vulkan Main/Options/Audio/Visual flow passed' in output, output
