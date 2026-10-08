#include "platform/ax_storage_abi.h"
extern "C" __attribute__((visibility("default"))) ChargedAXStorage charged_hbm_storage_zero() {
    return ChargedHBMGetZeroStorage();
}
