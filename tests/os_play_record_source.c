#include "src/RVL_SDK/os/OSPlayRecord.c"
#include <string.h>
/* Read-only probes around the whole original TU; no callback/state setters. */
__attribute__((visibility("default"))) u32 fixture_play_checksum(const void* p) {
    return RecordCheckSum((const OSPlayRecord*)p);
}
__attribute__((visibility("default"))) int fixture_play_state(void) { return PlayRecordState; }
__attribute__((visibility("default"))) s64 fixture_play_last_close(void) { return PlayRecordLastCloseTime; }
__attribute__((visibility("default"))) void fixture_play_copy(void* p) { memcpy(p,&PlayRecord,sizeof PlayRecord); }
__attribute__((visibility("default"))) int fixture_play_alarm_pending(void) { return PlayRecordAlarm.handler != NULL; }
