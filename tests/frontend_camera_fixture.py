"""Read the pinned original catalog; generate only original synthetic CAM bytes."""
from pathlib import Path
import re
from camera_fixture import camera_fixture


def frontend_camera_catalog():
    source=(Path(__file__).resolve().parents[1]/'extern/mscharged-decomp/src/Game/Camera/CameraMan.cpp').read_text()
    marker='CameraAnimationLoadInfo frontEndCameraAnimations[] = {'
    if source.count(marker)!=1:
        raise ValueError('Pinned source has no unique original frontend camera table')
    block=source.split(marker,1)[1].split('};',1)[0]
    entries=re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*\}',block)
    if len(entries)!=37:
        raise ValueError('Review the changed original frontend camera catalog')
    return entries


def frontend_camera_files():
    # The retail root is Art; original source paths use art. Exercise NL/DVD's
    # real case-insensitive path handling rather than rewriting port requests.
    return {'Art/'+path.split('/',1)[1]:camera_fixture() for path,_ in frontend_camera_catalog()}
