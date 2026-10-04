"""Synthetic NIS metadata and bytecode containers; no retail bytes."""
import struct


def dictionary(name="Opening", actors=8):
    return (f"name {name}\nsize 1024\nhas_ball 1\nnum_animations {actors}\nnum_cameras 2\n"
            "center 0, 0, 0\nmin_bounds -1, -2, -3\nmax_bounds 1, 2, 3\n"
            + "begin_pos 1, 2, 3\n" * actors
            + "num_anim_proxies 1\nanim_proxy_name crowd\nanim_proxy_position 2 3\nanim_proxy_direction 65535\n").encode()


def bootstrap_files():
    script = struct.pack(">18I", 0xe11c2112, *([0] * 17))
    return {"Art/scripts/nis_triggers.byte_code": script,
            "Art/scripts/nis_anim_proxy.byte_code": script,
            "Art/nis/do_not_mirror.txt": b"# synthetic\r\nOpening\r\nOther",
            "Art/nis/do_not_showPiP.txt": b"Opening\n",
            "Art/nis/nis_dict.txt": dictionary()}
