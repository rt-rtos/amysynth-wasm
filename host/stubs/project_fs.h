#pragma once
/* Host shim: project files live in ./proj relative to the sim's cwd. */
#include <stdbool.h>
#include <stddef.h>
#define PROJECT_FS_BASE "proj"
static inline bool project_fs_init(void) { return true; }
static inline bool project_fs_ok(void) { return true; }
static inline bool project_fs_stats(size_t *t, size_t *u) { if (t) *t = 0; if (u) *u = 0; return true; }
