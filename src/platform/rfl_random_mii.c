#include "platform/rfl_random_mii.h"
#include "platform/rfl_character_transport.h"

#include <RVLFaceLib/RFLi_MakeRandomFace.h>
#include <string.h>

/* RFLiCharInfo to its record value, as RFL_Database.c's convertInfo2RawCore_
 * (static there) converts it. */
static void InfoToData(const RFLiCharInfo* info, RFLiCharData* data) {
    data->faceType = info->faceline.type;
    data->faceColor = info->faceline.color;
    data->faceTex = info->faceline.texture;

    data->hairType = info->hair.type;
    data->hairColor = info->hair.color;
    data->hairFlip = info->hair.flip;

    data->eyeType = info->eye.type;
    data->eyeColor = info->eye.color;
    data->eyeScale = info->eye.scale;
    data->eyeRotate = info->eye.rotate;
    data->eyeX = info->eye.x;
    data->eyeY = info->eye.y;

    data->eyebrowType = info->eyebrow.type;
    data->eyebrowColor = info->eyebrow.color;
    data->eyebrowScale = info->eyebrow.scale;
    data->eyebrowRotate = info->eyebrow.rotate;
    data->eyebrowX = info->eyebrow.x;
    data->eyebrowY = info->eyebrow.y;

    data->noseType = info->nose.type;
    data->noseScale = info->nose.scale;
    data->noseY = info->nose.y;

    data->mouthType = info->mouth.type;
    data->mouthColor = info->mouth.color;
    data->mouthScale = info->mouth.scale;
    data->mouthY = info->mouth.y;

    data->mustacheType = info->beard.mustache;
    data->beardType = info->beard.type;
    data->beardColor = info->beard.color;
    data->beardScale = info->beard.scale;
    data->beardY = info->beard.y;

    data->glassType = info->glass.type;
    data->glassColor = info->glass.color;
    data->glassScale = info->glass.scale;
    data->glassY = info->glass.y;

    data->moleType = info->mole.type;
    data->moleScale = info->mole.scale;
    data->moleX = info->mole.x;
    data->moleY = info->mole.y;

    data->height = info->body.height;
    data->build = info->body.build;

    data->sex = info->personal.sex;
    data->birthMonth = info->personal.bmonth;
    data->birthDay = info->personal.bday;
    data->favoriteColor = info->personal.color;
    data->favorite = info->personal.favorite;
    data->localonly = info->personal.localOnly;
}

void ChargedMakeRandomMiiRecord(const unsigned short* name, const unsigned char* create_id, void* record) {
    RFLiCharInfo info;
    RFLiCharData data;
    unsigned i;

    memset(&info, 0, sizeof(info));
    memset(&data, 0, sizeof(data));
    RFLi_MakeRandomFace(&info, RFLSex_All, RFLAge_All, RFLRace_All);
    InfoToData(&info, &data);
    for (i = 0; i < RFL_NAME_LEN; ++i) {
        data.name[i] = name[i];
    }
    memcpy(&data.createID, create_id, sizeof(data.createID));
    ChargedEncodeRFLCharRecordBE(&data, record);
}
