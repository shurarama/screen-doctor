/*
 * screen-doctor-activity-helper
 *
 * Bridges real input activity from the Wayland compositor to the
 * grab_override.so input-activity synthesizer. It subscribes to
 * ext_idle_notifier_v1 (input-idle variant when available) and, while the
 * user is active, keeps refreshing a small state file that the preload
 * reads inside Time Doctor.
 *
 * No Wayland development packages are required: the core protocol
 * interfaces (wl_registry, wl_seat, wl_display) are resolved from the
 * runtime libwayland-client, and only the two ext_idle_* interfaces are
 * defined by hand below. This keeps the tool in the same zero-dependency,
 * self-contained spirit as the rest of screen-doctor.
 *
 * Privacy: the helper records only the boolean fact that input occurred,
 * never keycodes or coordinates.
 */
#define _GNU_SOURCE
#include "activity_state.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- Minimal libwayland-client ABI (no dev headers available) --------- */

struct wl_display;
struct wl_proxy;

struct wl_message {
    const char *name;
    const char *signature;
    const struct wl_interface **types;
};

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};

/* Marshalling flag: mark a request as a destructor. */
#define WL_MARSHAL_FLAG_DESTROY (1 << 0)

extern struct wl_display *wl_display_connect(const char *name);
extern void wl_display_disconnect(struct wl_display *display);
extern int wl_display_get_fd(struct wl_display *display);
extern int wl_display_roundtrip(struct wl_display *display);
extern int wl_display_dispatch(struct wl_display *display);
extern int wl_display_dispatch_pending(struct wl_display *display);
extern int wl_display_flush(struct wl_display *display);
extern struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
                                               const struct wl_interface *interface,
                                               uint32_t version, uint32_t flags, ...);
extern int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void), void *data);
extern void wl_proxy_destroy(struct wl_proxy *proxy);
extern uint32_t wl_proxy_get_version(struct wl_proxy *proxy);

/* Core interfaces provided by libwayland-client. */
extern const struct wl_interface wl_registry_interface;
extern const struct wl_interface wl_seat_interface;

/* wl_display / wl_registry opcodes (stable). */
#define WL_DISPLAY_GET_REGISTRY 1
#define WL_REGISTRY_BIND 0

/* ---- ext-idle-notify-v1 protocol (hand-authored) ---------------------- */

/* ext_idle_notifier_v1 request opcodes. */
#define EXT_IDLE_NOTIFIER_V1_DESTROY 0
#define EXT_IDLE_NOTIFIER_V1_GET_IDLE_NOTIFICATION 1
#define EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION 2 /* since v2 */

/* ext_idle_notification_v1 request opcode. */
#define EXT_IDLE_NOTIFICATION_V1_DESTROY 0

static const struct wl_interface ext_idle_notification_v1_interface;
static const struct wl_interface ext_idle_notifier_v1_interface;

/* Argument type tables. Index 0 = notification new_id, 1 = timeout (uint,
 * no interface), 2 = wl_seat object. */
static const struct wl_interface *idle_notifier_arg_types[] = {
    &ext_idle_notification_v1_interface,
    NULL,
    &wl_seat_interface,
};

static const struct wl_message ext_idle_notifier_v1_requests[] = {
    { "destroy", "", idle_notifier_arg_types },
    { "get_idle_notification", "nuo", idle_notifier_arg_types },
    { "get_input_idle_notification", "2nuo", idle_notifier_arg_types },
};

static const struct wl_interface ext_idle_notifier_v1_interface = {
    "ext_idle_notifier_v1", 2,
    3, ext_idle_notifier_v1_requests,
    0, NULL,
};

static const struct wl_message ext_idle_notification_v1_requests[] = {
    { "destroy", "", idle_notifier_arg_types },
};

static const struct wl_message ext_idle_notification_v1_events[] = {
    { "idled", "", idle_notifier_arg_types },
    { "resumed", "", idle_notifier_arg_types },
};

static const struct wl_interface ext_idle_notification_v1_interface = {
    "ext_idle_notification_v1", 2,
    1, ext_idle_notification_v1_requests,
    2, ext_idle_notification_v1_events,
};

struct ext_idle_notification_v1_listener {
    void (*idled)(void *data, struct wl_proxy *notification);
    void (*resumed)(void *data, struct wl_proxy *notification);
};

struct wl_registry_listener {
    void (*global)(void *data, struct wl_proxy *registry, uint32_t name,
                   const char *interface, uint32_t version);
    void (*global_remove)(void *data, struct wl_proxy *registry, uint32_t name);
};

/* ---- Helper state ----------------------------------------------------- */

#define SD_IDLE_TIMEOUT_MS 1000u  /* compositor reports idle after this quiet gap */
#define SD_REFRESH_MS 500          /* how often we re-stamp the file while active */

static char state_path[PATH_MAX];
static int debug_enabled;
static volatile sig_atomic_t running = 1;

static struct wl_proxy *seat_proxy;
static struct wl_proxy *notifier_proxy;
static uint32_t notifier_name, seat_name;
static uint32_t notifier_version;
static int active = 1; /* optimistic: assume working at startup until first idled */

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int env_equals(const char *name, const char *expected) {
    const char *value = getenv(name);
    return value && strcmp(value, expected) == 0;
}

static int string_nonempty(const char *v) { return v && v[0] != '\0'; }

static void debug_log(const char *fmt, ...) {
    if (!debug_enabled) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void on_signal(int sig) {
    (void)sig;
    running = 0;
}

static void resolve_state_path(void) {
    const char *override = getenv("SCREEN_DOCTOR_ACTIVITY_STATE");
    if (!string_nonempty(override)) override = getenv("GRAB_OVERRIDE_ACTIVITY_STATE");
    if (string_nonempty(override)) {
        snprintf(state_path, sizeof(state_path), "%s", override);
        return;
    }
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!string_nonempty(runtime_dir)) {
        state_path[0] = '\0';
        return;
    }
    snprintf(state_path, sizeof(state_path), "%s/screen-doctor/activity.bin", runtime_dir);
}

static int write_all(int fd, const void *data, size_t len) {
    const unsigned char *bytes = data;
    while (len > 0) {
        ssize_t written = write(fd, bytes, len);
        if (written < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (written == 0) return -1;
        bytes += written;
        len -= (size_t)written;
    }
    return 0;
}

static void mkdir_parents(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
}

static int write_state(int is_active, uint64_t last_activity_ms) {
    if (!string_nonempty(state_path)) return -1;

    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", state_path);
    char tmp_path[PATH_MAX + 64];
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        mkdir_parents(dir);
        mkdir(dir, 0700);
        chmod(dir, 0700);
        snprintf(tmp_path, sizeof(tmp_path), "%s/.activity.bin.%d.tmp", dir, (int)getpid());
    } else {
        snprintf(tmp_path, sizeof(tmp_path), ".activity.bin.%d.tmp", (int)getpid());
    }

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return -1;

    sd_activity_state_t st;
    memset(&st, 0, sizeof(st));
    st.magic = SD_ACTIVITY_MAGIC;
    st.version = SD_ACTIVITY_VERSION;
    st.last_activity_monotonic_ms = last_activity_ms;
    st.updated_monotonic_ms = monotonic_ms();
    st.flags = SD_ACTIVITY_FLAG_VALID | (is_active ? SD_ACTIVITY_FLAG_ACTIVE : 0);

    int ok = 0;
    if (write_all(fd, &st, sizeof(st)) != 0) ok = -1;
    if (ok == 0 && fsync(fd) != 0) ok = -1;
    if (close(fd) != 0) ok = -1;
    if (ok == 0 && rename(tmp_path, state_path) != 0) ok = -1;
    if (ok != 0) unlink(tmp_path);
    return ok;
}

/* ---- Wayland event handlers ------------------------------------------ */

static void notification_idled(void *data, struct wl_proxy *n) {
    (void)data;
    (void)n;
    active = 0;
    debug_log("activity-helper: idled (no input for %ums)", SD_IDLE_TIMEOUT_MS);
    write_state(0, monotonic_ms());
}

static void notification_resumed(void *data, struct wl_proxy *n) {
    (void)data;
    (void)n;
    active = 1;
    debug_log("activity-helper: resumed (input detected)");
    write_state(1, monotonic_ms());
}

static const struct ext_idle_notification_v1_listener notification_listener = {
    notification_idled,
    notification_resumed,
};

static void registry_global(void *data, struct wl_proxy *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, "wl_seat") == 0 && !seat_proxy) {
        uint32_t bind_version = version < 4 ? version : 4;
        seat_proxy = wl_proxy_marshal_flags(registry, WL_REGISTRY_BIND, &wl_seat_interface,
                                            bind_version, 0, name, wl_seat_interface.name,
                                            bind_version, NULL);
        seat_name = name;
        debug_log("activity-helper: bound wl_seat v%u", bind_version);
    } else if (strcmp(interface, "ext_idle_notifier_v1") == 0 && !notifier_proxy) {
        notifier_version = version < 2 ? version : 2;
        notifier_proxy = wl_proxy_marshal_flags(registry, WL_REGISTRY_BIND,
                                                &ext_idle_notifier_v1_interface,
                                                notifier_version, 0, name,
                                                ext_idle_notifier_v1_interface.name,
                                                notifier_version, NULL);
        notifier_name = name;
        debug_log("activity-helper: bound ext_idle_notifier_v1 v%u", notifier_version);
    }
}

static void registry_global_remove(void *data, struct wl_proxy *registry, uint32_t name) {
    (void)data;
    (void)registry;
    if (name == notifier_name || name == seat_name) {
        debug_log("activity-helper: required global removed, exiting");
        running = 0;
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove,
};

int main(void) {
    debug_enabled = env_equals("SCREEN_DOCTOR_HELPER_DEBUG", "1");
    resolve_state_path();
    if (!string_nonempty(state_path)) {
        fprintf(stderr, "screen-doctor-activity-helper: XDG_RUNTIME_DIR unset and no state override\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "screen-doctor-activity-helper: cannot connect to Wayland display\n");
        return 1;
    }

    struct wl_proxy *registry = wl_proxy_marshal_flags((struct wl_proxy *)display,
                                                       WL_DISPLAY_GET_REGISTRY,
                                                       &wl_registry_interface,
                                                       wl_proxy_get_version((struct wl_proxy *)display),
                                                       0, NULL);
    wl_proxy_add_listener(registry, (void (**)(void))&registry_listener, NULL);
    wl_display_roundtrip(display); /* fire registry_global for existing globals */

    if (!notifier_proxy || !seat_proxy) {
        fprintf(stderr, "screen-doctor-activity-helper: compositor lacks %s\n",
                !notifier_proxy ? "ext_idle_notifier_v1" : "wl_seat");
        wl_display_disconnect(display);
        return 1;
    }

    uint32_t opcode = (notifier_version >= 2)
                          ? EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION
                          : EXT_IDLE_NOTIFIER_V1_GET_IDLE_NOTIFICATION;
    struct wl_proxy *notification = wl_proxy_marshal_flags(notifier_proxy, opcode,
                                                           &ext_idle_notification_v1_interface,
                                                           wl_proxy_get_version(notifier_proxy),
                                                           0, NULL, SD_IDLE_TIMEOUT_MS, seat_proxy);
    if (!notification) {
        fprintf(stderr, "screen-doctor-activity-helper: failed to create idle notification\n");
        wl_display_disconnect(display);
        return 1;
    }
    wl_proxy_add_listener(notification, (void (**)(void))&notification_listener, NULL);
    wl_display_flush(display);

    fprintf(stderr, "screen-doctor-activity-helper: watching input via %s, state=%s\n",
            (opcode == EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION)
                ? "ext_idle_notifier_v1.get_input_idle_notification"
                : "ext_idle_notifier_v1.get_idle_notification",
            state_path);

    /* Assume active at startup so genuine work is covered immediately; the
     * first idled event (after SD_IDLE_TIMEOUT_MS of quiet) corrects it. */
    uint64_t now = monotonic_ms();
    write_state(active, now);
    uint64_t last_write_ms = now;

    int fd = wl_display_get_fd(display);
    while (running) {
        wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct pollfd pfd = { fd, POLLIN, 0 };
        int timeout = active ? SD_REFRESH_MS : -1;
        int r = poll(&pfd, 1, timeout);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(display) < 0) {
                debug_log("activity-helper: display dispatch error, exiting");
                break;
            }
        }

        if (active) {
            now = monotonic_ms();
            if (now - last_write_ms >= SD_REFRESH_MS) {
                write_state(1, now);
                last_write_ms = now;
            }
        }
    }

    /* Leave a fresh "idle" stamp so the preload stops synthesizing promptly. */
    write_state(0, monotonic_ms());
    wl_proxy_destroy(notification);
    wl_display_disconnect(display);
    return 0;
}
