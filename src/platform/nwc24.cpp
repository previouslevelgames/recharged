#include <revolution/nwc24.h>

namespace
{
bool vf_ready = false;
bool nwc24_opened = false;
}

extern "C" void VFInit() { vf_ready = true; }
extern "C" s32 VFIsAvailable() { return vf_ready; }
extern "C" void VFFinalize() { vf_ready = false; }

extern "C" NWC24Err NWC24OpenLib(NWC24Work* work)
{
    if (work == nullptr) return NWC24_ERR_NULL;
    if (nwc24_opened) return NWC24_ERR_LIB_OPENED;
    nwc24_opened = true;
    return NWC24_OK;
}

extern "C" NWC24Err NWC24CloseLib()
{
    if (!nwc24_opened) return NWC24_ERR_LIB_NOT_OPENED;
    nwc24_opened = false;
    return NWC24_OK;
}

extern "C" NWC24Err NWC24GetMyUserId(u64* id)
{
    if (!nwc24_opened) return NWC24_ERR_LIB_NOT_OPENED;
    *id = 2601200516012010ull;
    return NWC24_OK;
}
