#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* One new Mii as its 74-byte Wii record (RFL_DB.dat): the Wii's own random
 * face (RFLi_MakeRandomFace, RFL_MakeRandomFace.c) with the given name (10
 * UTF-16 cells, 0 after the last) and create ID (8 Wii bytes). The face's
 * draws come from ChargedMiiRandomTick, which the caller defines: the Wii
 * code draws from OSGetTick, renamed to it for this unit. */
void ChargedMakeRandomMiiRecord(const unsigned short* name, const unsigned char* create_id, void* record);

unsigned int ChargedMiiRandomTick(void);

#ifdef __cplusplus
}
#endif
