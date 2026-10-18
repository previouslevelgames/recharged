#include <revolution/ncd.h>
#include <string.h>

NCDErr NCDGetCurrentIpConfig(NCDIpConfig *ipConfig)
{
    memset(ipConfig, 0, sizeof(NCDIpConfig));
    return NCD_RESULT_SUCCESS;
}

NCDErr NCDGetCurrentIfConfig(NCDIfConfig *ifConfig)
{
    memset(ifConfig, 0, sizeof(NCDIfConfig));
    return NCD_RESULT_SUCCESS;
}

NCDErr NCDiGetWirelessMacAddress(u8 macAddr[6])
{
    const static u8 exampleMacAddr[6] = {0x02, 0x23, 0x45, 0x67, 0x89, 0xab};
    memcpy(macAddr, exampleMacAddr, sizeof(exampleMacAddr));
    return NCD_RESULT_SUCCESS;
}

// Bitmasks of the enabled connection slots by medium: wired, wireless and
// USB access point. The host network is reported as connection 1, wired,
// like NCDGetCurrentIfConfig; NET only reads it to form error codes.
NCDErr NCDiGetEnabledConfigList(u32* list0, u32* list1, u32* list2)
{
    *list0 = 1;
    *list1 = 0;
    *list2 = 0;
    return NCD_RESULT_SUCCESS;
}
