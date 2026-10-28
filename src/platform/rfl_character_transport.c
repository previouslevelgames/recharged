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

static void WriteBE16(u8* bytes, u16 value) {
    bytes[0] = (u8)(value >> 8);
    bytes[1] = (u8)value;
}

void ChargedEncodeRFLCharRecordBE(const RFLiCharData* input, void* raw) {
    u8* bytes = (u8*)raw;
    unsigned i;
    /* Body octets and create-ID payload are bytes in both representations. */
    bytes[22] = input->height;
    bytes[23] = input->build;
    memcpy(bytes + 24, &input->createID, sizeof(input->createID));
    WriteBE16(bytes + 0,
              (u16)(((u16)(input->padding0 << 15) & 0x8000u)
                    | ((u16)(input->sex << 14) & 0x4000u)
                    | ((u16)(input->birthMonth << 10) & 0x3c00u)
                    | ((u16)(input->birthDay << 5) & 0x3e0u)
                    | ((u16)(input->favoriteColor << 1) & 0x1eu)
                    | ((u16)(input->favorite << 0) & 0x1u)));
    WriteBE16(bytes + 32,
              (u16)(((u16)(input->faceType << 13) & 0xe000u)
                    | ((u16)(input->faceColor << 10) & 0x1c00u)
                    | ((u16)(input->faceTex << 6) & 0x3c0u)
                    | ((u16)(input->padding2 << 3) & 0x38u)
                    | ((u16)(input->localonly << 2) & 0x4u)
                    | ((u16)(input->type << 0) & 0x3u)));
    WriteBE16(bytes + 34,
              (u16)(((u16)(input->hairType << 9) & 0xfe00u)
                    | ((u16)(input->hairColor << 6) & 0x1c0u)
                    | ((u16)(input->hairFlip << 5) & 0x20u)
                    | ((u16)(input->padding3 << 0) & 0x1fu)));
    WriteBE16(bytes + 36,
              (u16)(((u16)(input->eyebrowType << 11) & 0xf800u)
                    | ((u16)(input->eyebrowRotate << 6) & 0x7c0u)
                    | ((u16)(input->padding4 << 0) & 0x3fu)));
    WriteBE16(bytes + 38,
              (u16)(((u16)(input->eyebrowColor << 13) & 0xe000u)
                    | ((u16)(input->eyebrowScale << 9) & 0x1e00u)
                    | ((u16)(input->eyebrowY << 4) & 0x1f0u)
                    | ((u16)(input->eyebrowX << 0) & 0xfu)));
    WriteBE16(bytes + 40,
              (u16)(((u16)(input->eyeType << 10) & 0xfc00u)
                    | ((u16)(input->eyeRotate << 5) & 0x3e0u)
                    | ((u16)(input->eyeY << 0) & 0x1fu)));
    WriteBE16(bytes + 42,
              (u16)(((u16)(input->eyeColor << 13) & 0xe000u)
                    | ((u16)(input->eyeScale << 9) & 0x1e00u)
                    | ((u16)(input->eyeX << 5) & 0x1e0u)
                    | ((u16)(input->padding5 << 0) & 0x1fu)));
    WriteBE16(bytes + 44,
              (u16)(((u16)(input->noseType << 12) & 0xf000u)
                    | ((u16)(input->noseScale << 8) & 0xf00u)
                    | ((u16)(input->noseY << 3) & 0xf8u)
                    | ((u16)(input->padding6 << 0) & 0x7u)));
    WriteBE16(bytes + 46,
              (u16)(((u16)(input->mouthType << 11) & 0xf800u)
                    | ((u16)(input->mouthColor << 9) & 0x600u)
                    | ((u16)(input->mouthScale << 5) & 0x1e0u)
                    | ((u16)(input->mouthY << 0) & 0x1fu)));
    WriteBE16(bytes + 48,
              (u16)(((u16)(input->glassType << 12) & 0xf000u)
                    | ((u16)(input->glassColor << 9) & 0xe00u)
                    | ((u16)(input->glassScale << 5) & 0x1e0u)
                    | ((u16)(input->glassY << 0) & 0x1fu)));
    WriteBE16(bytes + 50,
              (u16)(((u16)(input->mustacheType << 14) & 0xc000u)
                    | ((u16)(input->beardType << 12) & 0x3000u)
                    | ((u16)(input->beardColor << 9) & 0xe00u)
                    | ((u16)(input->beardScale << 5) & 0x1e0u)
                    | ((u16)(input->beardY << 0) & 0x1fu)));
    WriteBE16(bytes + 52,
              (u16)(((u16)(input->moleType << 15) & 0x8000u)
                    | ((u16)(input->moleScale << 11) & 0x7800u)
                    | ((u16)(input->moleY << 6) & 0x7c0u)
                    | ((u16)(input->moleX << 1) & 0x3eu)
                    | ((u16)(input->padding8 << 0) & 0x1u)));
    for (i = 0; i < RFL_NAME_LEN; ++i) {
        WriteBE16(bytes + 2 + 2 * i, input->name[i]);
    }
    for (i = 0; i < RFL_CREATOR_LEN; ++i) {
        WriteBE16(bytes + 54 + 2 * i, input->creatorName[i]);
    }
}
