#ifndef ACTIVITY_STATE_H
#define ACTIVITY_STATE_H

#include <stdint.h>

/*
 * Shared on-disk format for the input-activity bridge.
 *
 * screen-doctor-activity-helper subscribes to the compositor's
 * ext_idle_notifier_v1 input-idle notification and, while the user is
 * active, keeps refreshing last_activity_monotonic_ms in this file.
 * grab_override.so reads the file inside Time Doctor: when the timestamp
 * is fresh (age <= TTL) it synthesizes XI_RawMotion events so Time
 * Doctor's XInput2 idle monitor sees activity for native Wayland work
 * that never reaches XWayland.
 *
 * The helper never records what the input was (no keycodes, no
 * coordinates) - only the boolean fact that the compositor observed
 * input recently.
 */

#define SD_ACTIVITY_MAGIC 0x53444143u /* "SDAC" */
#define SD_ACTIVITY_VERSION 1u

#define SD_ACTIVITY_FLAG_VALID (1u << 0) /* structure populated at least once */
#define SD_ACTIVITY_FLAG_ACTIVE (1u << 1) /* compositor currently reports "resumed" (not idle) */

typedef struct {
    uint32_t magic;
    uint32_t version;
    /* Monotonic timestamp (CLOCK_MONOTONIC ms) of the most recent moment
     * the compositor reported real input. Refreshed while active. */
    uint64_t last_activity_monotonic_ms;
    /* Monotonic timestamp of this write, so the reader can tell a live
     * helper (file being refreshed) from a dead one (file frozen). */
    uint64_t updated_monotonic_ms;
    uint32_t flags;
    uint32_t reserved;
} sd_activity_state_t;

#endif
