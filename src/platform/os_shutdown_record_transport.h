#pragma once
#include <revolution/nand.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed Wii byte records. These functions contain no checksum algorithm,
 * file selection, play-record state or source readiness decision. */
void mscharged_os_encode_state_record(void* wire, const void* native);
void mscharged_os_decode_state_record(void* native, const void* wire);
void mscharged_os_encode_play_record(void* wire, const void* native);
void mscharged_os_decode_play_record(void* native, const void* wire);
u32 mscharged_os_state_checksum_word(const void* native, const void* word);
u32 mscharged_os_play_checksum_word(const void* native, const void* word);

s32 mscharged_os_state_nand_read(NANDFileInfo* file, void* native, u32 bytes);
s32 mscharged_os_state_nand_write(NANDFileInfo* file, const void* native, u32 bytes);
s32 mscharged_os_play_nand_write(NANDFileInfo* file, const void* native, u32 bytes);
s32 mscharged_os_play_nand_read_async(NANDFileInfo* file, void* native, u32 bytes,
    NANDAsyncCallback callback, NANDCommandBlock* block);
s32 mscharged_os_play_nand_write_async(NANDFileInfo* file, const void* native, u32 bytes,
    NANDAsyncCallback callback, NANDCommandBlock* block);
/* Read-only transport ownership; zero does not imply source/game readiness. */
size_t mscharged_os_record_pending_count(void);

#ifdef __cplusplus
}
#endif
