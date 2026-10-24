#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Makes the Horizon file layer SQLite's default; 0 on success.
int charged_switch_sqlite_vfs_register(void);

#ifdef __cplusplus
}
#endif
