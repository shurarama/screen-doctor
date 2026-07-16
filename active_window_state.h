#ifndef ACTIVE_WINDOW_STATE_H
#define ACTIVE_WINDOW_STATE_H

#include <stdint.h>

#define SD_ACTIVE_WINDOW_MAGIC 0x53444157u /* "SDAW" */
#define SD_ACTIVE_WINDOW_VERSION 1u
#define SD_ACTIVE_WINDOW_TITLE_MAX 256
#define SD_ACTIVE_WINDOW_CLASS_MAX 128
#define SD_ACTIVE_WINDOW_DESKTOP_MAX 256
#define SD_ACTIVE_WINDOW_ID_MAX 80

#define SD_ACTIVE_WINDOW_FLAG_VALID (1u << 0)
#define SD_ACTIVE_WINDOW_FLAG_NORMAL (1u << 1)
#define SD_ACTIVE_WINDOW_FLAG_HAS_PID (1u << 2)

#define SD_SYNTHETIC_ACTIVE_WINDOW 0x7f534401u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint64_t updated_monotonic_ms;
    uint32_t pid;
    uint32_t flags;
    char title[SD_ACTIVE_WINDOW_TITLE_MAX];
    char resource_class[SD_ACTIVE_WINDOW_CLASS_MAX];
    char resource_name[SD_ACTIVE_WINDOW_CLASS_MAX];
    char desktop_file[SD_ACTIVE_WINDOW_DESKTOP_MAX];
    char internal_id[SD_ACTIVE_WINDOW_ID_MAX];
} sd_active_window_state_t;

#endif
