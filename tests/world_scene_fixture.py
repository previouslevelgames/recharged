"""Synthetic resident/temporary worlds with shared static geometry."""
import struct
from scene_fixture import make_assets


def chunk(tag, data):
    return struct.pack('>II', tag, len(data)) + data + b'\0' * (-len(data) % 4)


def world_scene_fixture(ids=(0x10, 0x20), missing_model=False, alpha=False, animated=False,
                        spacing=.7, depth_step=1, stored_transform=None, lit=False):
    model, textures = make_assets(lit=lit)
    if stored_transform is not None:
        model = bytearray(model)
        offset = model.index(struct.pack('>II', 0x1b002, 64)) + 8
        struct.pack_into('>16f', model, offset, *stored_transform)
    if alpha:
        textures = bytearray(textures)
        textures[43] = 8
        for i in range(16):
            textures[64+i*2] = 128
    if animated:
        from scene_fixture import animate_texture_bundle
        textures = animate_texture_bundle(textures)
    temporary = chunk(0x80000001, chunk(0x24100, textures) + chunk(0x8001b100, model))
    objects = bytearray(16)
    struct.pack_into('>I', objects, 0, len(ids))
    for i, identifier in enumerate(ids):
        record = bytearray(0x70)
        struct.pack_into('>4I', record, 0, 0, identifier, 0x101, 3)
        struct.pack_into('>I', record, 0x14, 0xffffffff)
        transform = [1,0,0,0, 0,1,0,0, 0,0,1,0, -spacing+2*spacing*i,0,-depth_step*float(i),1]
        struct.pack_into('>16f', record, 0x20, *transform)
        struct.pack_into('>fI', record, 0x60, 2, 0x87654322 if missing_model else 0x87654321)
        objects += record
    # Root and child headers put the object stream at absolute 16-byte alignment.
    resident = chunk(0x80000001, struct.pack('>II', 0x04026000, len(objects)) + objects)
    return bytes(resident), bytes(temporary)
