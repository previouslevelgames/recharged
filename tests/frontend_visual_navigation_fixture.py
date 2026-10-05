"""Generated scene15/NAV resources; no retail bytes."""
import struct
from frontend_visual_options_fixture import files as visual_files
from frontend_navigation_done_fixture import with_navigation

def files(overlap=False):
    result=with_navigation(visual_files())
    if overlap:
        # Independently place level4 over the original fixed Done rectangle.
        # Its group adds (10,40), so asset(-10,-252) gives center(0,-212).
        data=bytearray(result['Art/fe/options_visual_options.fen'])
        candidates=[];start=0
        while True:
            marker=data.find(b'BUTTON_4\0',start)
            if marker<0:break
            instance=marker-0x18
            if instance>=16 and instance+0x8c<=len(data) and struct.unpack_from('>I',data,instance+0x88)[0]==4:candidates.append(instance)
            start=marker+1
        assert len(candidates)==1
        instance=candidates[0]
        struct.pack_into('>2f',data,instance+0x3c,-10,-252)
        result['Art/fe/options_visual_options.fen']=bytes(data)
    return result
