#include "platform/rfl_character_transport.h"
#include <string.h>

/* Field masks/offsets are actual pinned GC/3.0a5 RFLiCharData ELF cells.
 * Field assignment projects that serialized ABI onto native compiler
 * bitfields; original RFL value conversion methods remain authoritative. */
_Static_assert(sizeof(RFLiCharData) == 74, "RFL source record stride");
_Static_assert(sizeof(RFL_WCHAR) == 2, "RFL source name cell stride");

static u16 ReadBE16(const u8* bytes) {
    return (u16)(((u16)bytes[0] << 8) | (u16)bytes[1]);
}

void ChargedDecodeRFLCharRecordBE(const void* raw, RFLiCharData* output) {
    const u8* bytes = (const u8*)raw;
    unsigned i;
    /* Retain uninterpreted bytes, body octets and create-ID payload. */
    memcpy(output, bytes, sizeof(*output));
    output->padding0 = (u16)((ReadBE16(bytes + 0) & 0x8000u) >> 15);
    output->sex = (u16)((ReadBE16(bytes + 0) & 0x4000u) >> 14);
    output->birthMonth = (u16)((ReadBE16(bytes + 0) & 0x3c00u) >> 10);
    output->birthDay = (u16)((ReadBE16(bytes + 0) & 0x3e0u) >> 5);
    output->favoriteColor = (u16)((ReadBE16(bytes + 0) & 0x1eu) >> 1);
    output->favorite = (u16)((ReadBE16(bytes + 0) & 0x1u) >> 0);
    output->faceType = (u16)((ReadBE16(bytes + 32) & 0xe000u) >> 13);
    output->faceColor = (u16)((ReadBE16(bytes + 32) & 0x1c00u) >> 10);
    output->faceTex = (u16)((ReadBE16(bytes + 32) & 0x3c0u) >> 6);
    output->padding2 = (u16)((ReadBE16(bytes + 32) & 0x38u) >> 3);
    output->localonly = (u16)((ReadBE16(bytes + 32) & 0x4u) >> 2);
    output->type = (u16)((ReadBE16(bytes + 32) & 0x3u) >> 0);
    output->hairType = (u16)((ReadBE16(bytes + 34) & 0xfe00u) >> 9);
    output->hairColor = (u16)((ReadBE16(bytes + 34) & 0x1c0u) >> 6);
    output->hairFlip = (u16)((ReadBE16(bytes + 34) & 0x20u) >> 5);
    output->padding3 = (u16)((ReadBE16(bytes + 34) & 0x1fu) >> 0);
    output->eyebrowType = (u16)((ReadBE16(bytes + 36) & 0xf800u) >> 11);
    output->eyebrowRotate = (u16)((ReadBE16(bytes + 36) & 0x7c0u) >> 6);
    output->padding4 = (u16)((ReadBE16(bytes + 36) & 0x3fu) >> 0);
    output->eyebrowColor = (u16)((ReadBE16(bytes + 38) & 0xe000u) >> 13);
    output->eyebrowScale = (u16)((ReadBE16(bytes + 38) & 0x1e00u) >> 9);
    output->eyebrowY = (u16)((ReadBE16(bytes + 38) & 0x1f0u) >> 4);
    output->eyebrowX = (u16)((ReadBE16(bytes + 38) & 0xfu) >> 0);
    output->eyeType = (u16)((ReadBE16(bytes + 40) & 0xfc00u) >> 10);
    output->eyeRotate = (u16)((ReadBE16(bytes + 40) & 0x3e0u) >> 5);
    output->eyeY = (u16)((ReadBE16(bytes + 40) & 0x1fu) >> 0);
    output->eyeColor = (u16)((ReadBE16(bytes + 42) & 0xe000u) >> 13);
    output->eyeScale = (u16)((ReadBE16(bytes + 42) & 0x1e00u) >> 9);
    output->eyeX = (u16)((ReadBE16(bytes + 42) & 0x1e0u) >> 5);
    output->padding5 = (u16)((ReadBE16(bytes + 42) & 0x1fu) >> 0);
    output->noseType = (u16)((ReadBE16(bytes + 44) & 0xf000u) >> 12);
    output->noseScale = (u16)((ReadBE16(bytes + 44) & 0xf00u) >> 8);
    output->noseY = (u16)((ReadBE16(bytes + 44) & 0xf8u) >> 3);
    output->padding6 = (u16)((ReadBE16(bytes + 44) & 0x7u) >> 0);
    output->mouthType = (u16)((ReadBE16(bytes + 46) & 0xf800u) >> 11);
    output->mouthColor = (u16)((ReadBE16(bytes + 46) & 0x600u) >> 9);
    output->mouthScale = (u16)((ReadBE16(bytes + 46) & 0x1e0u) >> 5);
    output->mouthY = (u16)((ReadBE16(bytes + 46) & 0x1fu) >> 0);
    output->glassType = (u16)((ReadBE16(bytes + 48) & 0xf000u) >> 12);
    output->glassColor = (u16)((ReadBE16(bytes + 48) & 0xe00u) >> 9);
    output->glassScale = (u16)((ReadBE16(bytes + 48) & 0x1e0u) >> 5);
    output->glassY = (u16)((ReadBE16(bytes + 48) & 0x1fu) >> 0);
    output->mustacheType = (u16)((ReadBE16(bytes + 50) & 0xc000u) >> 14);
    output->beardType = (u16)((ReadBE16(bytes + 50) & 0x3000u) >> 12);
    output->beardColor = (u16)((ReadBE16(bytes + 50) & 0xe00u) >> 9);
    output->beardScale = (u16)((ReadBE16(bytes + 50) & 0x1e0u) >> 5);
    output->beardY = (u16)((ReadBE16(bytes + 50) & 0x1fu) >> 0);
    output->moleType = (u16)((ReadBE16(bytes + 52) & 0x8000u) >> 15);
    output->moleScale = (u16)((ReadBE16(bytes + 52) & 0x7800u) >> 11);
    output->moleY = (u16)((ReadBE16(bytes + 52) & 0x7c0u) >> 6);
    output->moleX = (u16)((ReadBE16(bytes + 52) & 0x3eu) >> 1);
    output->padding8 = (u16)((ReadBE16(bytes + 52) & 0x1u) >> 0);
    for (i = 0; i < RFL_NAME_LEN; ++i) {
        output->name[i] = ReadBE16(bytes + 2 + 2 * i);
    }
    for (i = 0; i < RFL_CREATOR_LEN; ++i) {
        output->creatorName[i] = ReadBE16(bytes + 54 + 2 * i);
    }
}
