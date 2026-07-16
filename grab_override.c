#define _GNU_SOURCE
#include "active_window_state.h"

#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <gio/gio.h>
#include <dlfcn.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <png.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>
#include <stdint.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/epoll.h>

#ifndef __has_include
#define __has_include(x) 0
#endif

#if __has_include(<X11/extensions/XInput2.h>)
#include <X11/extensions/XInput2.h>
#else
#include <X11/extensions/XI2.h>
typedef struct {
    int deviceid;
    int mask_len;
    unsigned char *mask;
} XIEventMask;

typedef struct {
    int mask_len;
    unsigned char *mask;
    double *values;
} XIValuatorState;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    int extension;
    int evtype;
    Time time;
    int deviceid;
    int sourceid;
    int detail;
    int flags;
    XIValuatorState valuators;
    double *raw_values;
} XIRawEvent;
#endif

/*
 * LD_PRELOAD: optionally intercept xcb_copy_area on root window.
 * Qt's QXcbScreen::grabWindow() uses xcb_copy_area(root -> pixmap),
 * which returns black on XWayland. We intercept this and fill the
 * destination pixmap with a real screenshot via spectacle/portal.
 */

#define LOGFILE "/tmp/grab_override.log"

static void vlogmsg(const char *fmt, va_list ap) {
    FILE *f = fopen(LOGFILE, "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm tm_buf;
    struct tm *t = localtime_r(&now, &tm_buf);
    if (t) {
        fprintf(f, "[%02d:%02d:%02d pid=%d] ", t->tm_hour, t->tm_min, t->tm_sec, getpid());
    } else {
        fprintf(f, "[??:??:?? pid=%d] ", getpid());
    }
    vfprintf(f, fmt, ap);
    fprintf(f, "\n");
    fflush(f);
    fclose(f);
}

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlogmsg(fmt, ap);
    va_end(ap);
}

static __thread int diag_log_depth = 0;

static void diag_logmsg(const char *fmt, ...) {
    if (diag_log_depth) return;
    diag_log_depth++;
    va_list ap;
    va_start(ap, fmt);
    vlogmsg(fmt, ap);
    va_end(ap);
    diag_log_depth--;
}

typedef xcb_void_cookie_t (*real_xcb_copy_area_t)(
    xcb_connection_t *, xcb_drawable_t, xcb_drawable_t, xcb_gcontext_t,
    int16_t, int16_t, int16_t, int16_t, uint16_t, uint16_t);

static real_xcb_copy_area_t real_xcb_copy_area_fn = NULL;

typedef int (*real_XIQueryVersion_t)(Display *, int *, int *);
typedef int (*real_XISelectEvents_t)(Display *, Window, XIEventMask *, int);
typedef int (*real_XSelectInput_t)(Display *, Window, long);
typedef int (*real_XNextEvent_t)(Display *, XEvent *);
typedef Bool (*real_XGetEventData_t)(Display *, XGenericEventCookie *);
typedef void (*real_XFreeEventData_t)(Display *, XGenericEventCookie *);
typedef Bool (*real_XQueryPointer_t)(Display *, Window, Window *, Window *, int *, int *, int *, int *, unsigned int *);
typedef Bool (*real_XQueryExtension_t)(Display *, const char *, int *, int *, int *);
typedef int (*real_XEventsQueued_t)(Display *, int);
typedef int (*real_XPending_t)(Display *);
typedef int (*real_poll_t)(struct pollfd *, nfds_t, int);
typedef int (*real_ppoll_t)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
typedef int (*real_epoll_ctl_t)(int, int, int, struct epoll_event *);
typedef int (*real_epoll_wait_t)(int, struct epoll_event *, int, int);
typedef int (*real_epoll_pwait_t)(int, struct epoll_event *, int, int, const sigset_t *);

typedef xcb_intern_atom_cookie_t (*real_xcb_intern_atom_t)(xcb_connection_t *, uint8_t, uint16_t, const char *);
typedef xcb_intern_atom_reply_t *(*real_xcb_intern_atom_reply_t)(xcb_connection_t *, xcb_intern_atom_cookie_t, xcb_generic_error_t **);
typedef xcb_get_property_cookie_t (*real_xcb_get_property_t)(xcb_connection_t *, uint8_t, xcb_window_t, xcb_atom_t, xcb_atom_t, uint32_t, uint32_t);
typedef xcb_get_property_reply_t *(*real_xcb_get_property_reply_t)(xcb_connection_t *, xcb_get_property_cookie_t, xcb_generic_error_t **);

static real_XIQueryVersion_t real_XIQueryVersion_fn = NULL;
static real_XISelectEvents_t real_XISelectEvents_fn = NULL;
static real_XSelectInput_t real_XSelectInput_fn = NULL;
static real_XNextEvent_t real_XNextEvent_fn = NULL;
static real_XGetEventData_t real_XGetEventData_fn = NULL;
static real_XFreeEventData_t real_XFreeEventData_fn = NULL;
static real_XQueryPointer_t real_XQueryPointer_fn = NULL;
static real_XQueryExtension_t real_XQueryExtension_fn = NULL;
static real_XEventsQueued_t real_XEventsQueued_fn = NULL;
static real_XPending_t real_XPending_fn = NULL;
static real_poll_t real_poll_fn = NULL;
static real_ppoll_t real_ppoll_fn = NULL;
static real_epoll_ctl_t real_epoll_ctl_fn = NULL;
static real_epoll_wait_t real_epoll_wait_fn = NULL;
static real_epoll_pwait_t real_epoll_pwait_fn = NULL;
static real_xcb_intern_atom_t real_xcb_intern_atom_fn = NULL;
static real_xcb_intern_atom_t real_xcb_intern_atom_unchecked_fn = NULL;
static real_xcb_intern_atom_reply_t real_xcb_intern_atom_reply_fn = NULL;
static real_xcb_get_property_t real_xcb_get_property_fn = NULL;
static real_xcb_get_property_t real_xcb_get_property_unchecked_fn = NULL;
static real_xcb_get_property_reply_t real_xcb_get_property_reply_fn = NULL;

static int warned_XIQueryVersion = 0;
static int warned_XISelectEvents = 0;
static int warned_XSelectInput = 0;
static int warned_XNextEvent = 0;
static int warned_XGetEventData = 0;
static int warned_XFreeEventData = 0;
static int warned_XQueryPointer = 0;
static int warned_XQueryExtension = 0;
static int warned_XEventsQueued = 0;
static int warned_XPending = 0;
static int warned_poll = 0;
static int warned_ppoll = 0;
static int warned_epoll_ctl = 0;
static int warned_epoll_wait = 0;
static int warned_epoll_pwait = 0;
static int warned_xcb_copy_area = 0;
static int warned_xcb_intern_atom = 0;
static int warned_xcb_intern_atom_unchecked = 0;
static int warned_xcb_intern_atom_reply = 0;
static int warned_xcb_get_property = 0;
static int warned_xcb_get_property_unchecked = 0;
static int warned_xcb_get_property_reply = 0;

static void *resolve_next_symbol(const char *name, int *warned) {
    void *sym = dlsym(RTLD_NEXT, name);
    if (!sym && warned && !*warned) {
        *warned = 1;
        logmsg("missing real symbol %s", name);
    }
    return sym;
}

static pthread_once_t diag_config_once = PTHREAD_ONCE_INIT;
static int diag_all_enabled = 0;
static int diag_xinput_category_enabled = 0;
static int diag_xcb_category_enabled = 0;

static pthread_once_t screenshot_config_once = PTHREAD_ONCE_INIT;
static int screenshot_hook_enabled = 0;

static pthread_once_t active_window_config_once = PTHREAD_ONCE_INIT;
static int active_window_bridge_enabled = 0;
static int active_window_diag_requested = 0;
static uint32_t active_window_ttl_ms = 5000;

static pthread_once_t activity_config_once = PTHREAD_ONCE_INIT;
static int activity_bridge_enabled = 0;
static int activity_profile_hook_enabled = 0; /* human-like event stream (keys/buttons/motion bursts) */
static uint32_t activity_ttl_ms = 2000;   /* max age of a "fresh" activity stamp */
static uint32_t activity_rate_ms = 1000;  /* min gap between synthesized events; also the poll-wait cap */

static int env_equals(const char *name, const char *expected) {
    const char *value = getenv(name);
    return value && strcmp(value, expected) == 0;
}

static void load_diag_config(void) {
    diag_all_enabled = env_equals("GRAB_OVERRIDE_DIAG", "1");
    diag_xinput_category_enabled = !env_equals("GRAB_OVERRIDE_DIAG_XINPUT", "0");
    diag_xcb_category_enabled = !env_equals("GRAB_OVERRIDE_DIAG_XCB", "0");
}

static int diag_xinput_enabled(void) {
    pthread_once(&diag_config_once, load_diag_config);
    return diag_all_enabled && diag_xinput_category_enabled;
}

static int diag_xcb_enabled(void) {
    pthread_once(&diag_config_once, load_diag_config);
    return diag_all_enabled && diag_xcb_category_enabled;
}

static void load_screenshot_config(void) {
    screenshot_hook_enabled = env_equals("GRAB_OVERRIDE_SCREENSHOT", "1");
}

static int screenshot_enabled(void) {
    pthread_once(&screenshot_config_once, load_screenshot_config);
    return screenshot_hook_enabled;
}

static uint32_t parse_env_u32(const char *name, uint32_t fallback) {
    const char *value = getenv(name);
    if (!value || value[0] == '\0') return fallback;

    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) return fallback;
    return (uint32_t)parsed;
}

static void load_active_window_config(void) {
    active_window_bridge_enabled = env_equals("GRAB_OVERRIDE_ACTIVE_WINDOW", "1");
    active_window_diag_requested = env_equals("GRAB_OVERRIDE_ACTIVE_WINDOW_DIAG", "1");
    active_window_ttl_ms = parse_env_u32("GRAB_OVERRIDE_ACTIVE_WINDOW_TTL_MS", 5000);
}

static int active_window_enabled(void) {
    pthread_once(&active_window_config_once, load_active_window_config);
    return active_window_bridge_enabled;
}

static int active_window_diag_enabled(void) {
    pthread_once(&active_window_config_once, load_active_window_config);
    pthread_once(&diag_config_once, load_diag_config);
    return active_window_diag_requested || diag_all_enabled;
}

static int xcb_tracking_enabled(void) {
    return diag_xcb_enabled() || active_window_enabled();
}

static void load_activity_config(void) {
    activity_bridge_enabled = env_equals("GRAB_OVERRIDE_ACTIVITY", "1");
    activity_profile_hook_enabled = env_equals("GRAB_OVERRIDE_ACTIVITY_PROFILE", "1");
    activity_ttl_ms = parse_env_u32("GRAB_OVERRIDE_ACTIVITY_TTL_MS", 2000);
    activity_rate_ms = parse_env_u32("GRAB_OVERRIDE_ACTIVITY_RATE_MS", 1000);
    if (activity_rate_ms == 0) activity_rate_ms = 1000;
}

static int activity_enabled(void) {
    pthread_once(&activity_config_once, load_activity_config);
    return activity_bridge_enabled;
}

/* Profile mode shapes the synthetic stream to resemble human input (keystroke
 * press/release pairs, clicks, and motion bursts with real valuator deltas)
 * instead of a single bare RawMotion once per rate interval. Off by default so
 * the simple, confirmed-working path stays the fallback. */
static int activity_profile_enabled(void) {
    pthread_once(&activity_config_once, load_activity_config);
    return activity_bridge_enabled && activity_profile_hook_enabled;
}

static void append_name(char *buf, size_t buf_size, const char *name) {
    if (!buf || buf_size == 0 || !name) return;
    size_t len = strlen(buf);
    if (len >= buf_size - 1) return;
    snprintf(buf + len, buf_size - len, "%s%s", len ? "," : "", name);
}

static const char *xi_event_name(int evtype) {
    switch (evtype) {
        case XI_KeyPress: return "KeyPress";
        case XI_KeyRelease: return "KeyRelease";
        case XI_ButtonPress: return "ButtonPress";
        case XI_ButtonRelease: return "ButtonRelease";
        case XI_Motion: return "Motion";
        case XI_RawKeyPress: return "RawKeyPress";
        case XI_RawKeyRelease: return "RawKeyRelease";
        case XI_RawButtonPress: return "RawButtonPress";
        case XI_RawButtonRelease: return "RawButtonRelease";
        case XI_RawMotion: return "RawMotion";
        default: return "unknown";
    }
}

static int xi_mask_has(const XIEventMask *mask, int evtype) {
    if (!mask || !mask->mask || evtype < 0 || mask->mask_len <= 0) return 0;
    if ((evtype >> 3) >= mask->mask_len) return 0;
    return XIMaskIsSet(mask->mask, evtype) != 0;
}

static void decode_xi_mask(const XIEventMask *mask, char *buf, size_t buf_size) {
    static const struct {
        int evtype;
        const char *name;
    } events[] = {
        { XI_RawKeyPress, "RawKeyPress" },
        { XI_RawKeyRelease, "RawKeyRelease" },
        { XI_RawButtonPress, "RawButtonPress" },
        { XI_RawButtonRelease, "RawButtonRelease" },
        { XI_RawMotion, "RawMotion" },
        { XI_KeyPress, "KeyPress" },
        { XI_KeyRelease, "KeyRelease" },
        { XI_ButtonPress, "ButtonPress" },
        { XI_ButtonRelease, "ButtonRelease" },
        { XI_Motion, "Motion" },
    };

    if (!buf || buf_size == 0) return;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(events) / sizeof(events[0]); i++) {
        if (xi_mask_has(mask, events[i].evtype)) append_name(buf, buf_size, events[i].name);
    }
    if (buf[0] == '\0') snprintf(buf, buf_size, "none");
}

static const char *core_event_name(int type) {
    switch (type) {
        case KeyPress: return "KeyPress";
        case KeyRelease: return "KeyRelease";
        case ButtonPress: return "ButtonPress";
        case ButtonRelease: return "ButtonRelease";
        case MotionNotify: return "MotionNotify";
        case EnterNotify: return "EnterNotify";
        case LeaveNotify: return "LeaveNotify";
        case FocusIn: return "FocusIn";
        case FocusOut: return "FocusOut";
        case KeymapNotify: return "KeymapNotify";
        case Expose: return "Expose";
        case GraphicsExpose: return "GraphicsExpose";
        case NoExpose: return "NoExpose";
        case VisibilityNotify: return "VisibilityNotify";
        case CreateNotify: return "CreateNotify";
        case DestroyNotify: return "DestroyNotify";
        case UnmapNotify: return "UnmapNotify";
        case MapNotify: return "MapNotify";
        case MapRequest: return "MapRequest";
        case ReparentNotify: return "ReparentNotify";
        case ConfigureNotify: return "ConfigureNotify";
        case ConfigureRequest: return "ConfigureRequest";
        case GravityNotify: return "GravityNotify";
        case ResizeRequest: return "ResizeRequest";
        case CirculateNotify: return "CirculateNotify";
        case CirculateRequest: return "CirculateRequest";
        case PropertyNotify: return "PropertyNotify";
        case SelectionClear: return "SelectionClear";
        case SelectionRequest: return "SelectionRequest";
        case SelectionNotify: return "SelectionNotify";
        case ColormapNotify: return "ColormapNotify";
        case ClientMessage: return "ClientMessage";
        case MappingNotify: return "MappingNotify";
        case GenericEvent: return "GenericEvent";
        default: return "unknown";
    }
}

static void decode_core_event_mask(long event_mask, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    buf[0] = '\0';
    if (event_mask & KeyPressMask) append_name(buf, buf_size, "KeyPress");
    if (event_mask & KeyReleaseMask) append_name(buf, buf_size, "KeyRelease");
    if (event_mask & ButtonPressMask) append_name(buf, buf_size, "ButtonPress");
    if (event_mask & ButtonReleaseMask) append_name(buf, buf_size, "ButtonRelease");
    if (event_mask & PointerMotionMask) append_name(buf, buf_size, "PointerMotion");
    if (event_mask & PointerMotionHintMask) append_name(buf, buf_size, "PointerMotionHint");
    if (event_mask & ButtonMotionMask) append_name(buf, buf_size, "ButtonMotion");
    if (event_mask & EnterWindowMask) append_name(buf, buf_size, "EnterWindow");
    if (event_mask & LeaveWindowMask) append_name(buf, buf_size, "LeaveWindow");
    if (event_mask & FocusChangeMask) append_name(buf, buf_size, "FocusChange");
    if (event_mask & PropertyChangeMask) append_name(buf, buf_size, "PropertyChange");
    if (event_mask & StructureNotifyMask) append_name(buf, buf_size, "StructureNotify");
    if (event_mask & SubstructureNotifyMask) append_name(buf, buf_size, "SubstructureNotify");
    if (event_mask & SubstructureRedirectMask) append_name(buf, buf_size, "SubstructureRedirect");
    if (buf[0] == '\0') snprintf(buf, buf_size, "none");
}

typedef struct {
    unsigned int count;
    unsigned int suppressed;
    time_t last_log;
} diag_rate_state_t;

static pthread_mutex_t diag_rate_mutex = PTHREAD_MUTEX_INITIALIZER;
static diag_rate_state_t diag_xget_rates[64];
static diag_rate_state_t diag_xget_unknown_rate;
static diag_rate_state_t diag_xnext_rates[128];
static diag_rate_state_t diag_xnext_unknown_rate;
static diag_rate_state_t diag_xquery_pointer_rate;

static int diag_rate_should_log(diag_rate_state_t *state, unsigned int first_limit,
                                time_t interval_secs, unsigned int *suppressed_out) {
    int should_log = 0;
    time_t now = time(NULL);
    if (suppressed_out) *suppressed_out = 0;

    pthread_mutex_lock(&diag_rate_mutex);
    if (state->count < first_limit) {
        should_log = 1;
    } else if (state->last_log == 0 || now - state->last_log >= interval_secs) {
        should_log = 1;
    }

    if (should_log) {
        if (suppressed_out) *suppressed_out = state->suppressed;
        state->suppressed = 0;
        state->last_log = now;
    } else {
        state->suppressed++;
    }
    state->count++;
    pthread_mutex_unlock(&diag_rate_mutex);
    return should_log;
}

static diag_rate_state_t *xget_rate_state(int evtype) {
    if (evtype >= 0 && evtype < (int)(sizeof(diag_xget_rates) / sizeof(diag_xget_rates[0]))) {
        return &diag_xget_rates[evtype];
    }
    return &diag_xget_unknown_rate;
}

static diag_rate_state_t *xnext_rate_state(int type) {
    if (type >= 0 && type < (int)(sizeof(diag_xnext_rates) / sizeof(diag_xnext_rates[0]))) {
        return &diag_xnext_rates[type];
    }
    return &diag_xnext_unknown_rate;
}

typedef struct {
    int valid;
    Display *display;
    int present;
    int opcode;
    int event;
    int error;
} xinput_opcode_cache_t;

static pthread_mutex_t xinput_opcode_mutex = PTHREAD_MUTEX_INITIALIZER;
static xinput_opcode_cache_t xinput_opcode_cache[8];
static size_t xinput_opcode_next = 0;

static int get_xinput_opcode(Display *display, int *opcode_out) {
    if (!display) return 0;

    pthread_mutex_lock(&xinput_opcode_mutex);
    for (size_t i = 0; i < sizeof(xinput_opcode_cache) / sizeof(xinput_opcode_cache[0]); i++) {
        if (xinput_opcode_cache[i].valid && xinput_opcode_cache[i].display == display) {
            int present = xinput_opcode_cache[i].present;
            int opcode = xinput_opcode_cache[i].opcode;
            pthread_mutex_unlock(&xinput_opcode_mutex);
            if (present && opcode_out) *opcode_out = opcode;
            return present;
        }
    }
    pthread_mutex_unlock(&xinput_opcode_mutex);

    if (!real_XQueryExtension_fn)
        real_XQueryExtension_fn = (real_XQueryExtension_t)resolve_next_symbol("XQueryExtension", &warned_XQueryExtension);
    if (!real_XQueryExtension_fn) return 0;

    int opcode = 0, event = 0, error = 0;
    Bool present = real_XQueryExtension_fn(display, "XInputExtension", &opcode, &event, &error);

    pthread_mutex_lock(&xinput_opcode_mutex);
    xinput_opcode_cache[xinput_opcode_next].valid = 1;
    xinput_opcode_cache[xinput_opcode_next].display = display;
    xinput_opcode_cache[xinput_opcode_next].present = present ? 1 : 0;
    xinput_opcode_cache[xinput_opcode_next].opcode = opcode;
    xinput_opcode_cache[xinput_opcode_next].event = event;
    xinput_opcode_cache[xinput_opcode_next].error = error;
    xinput_opcode_next = (xinput_opcode_next + 1) % (sizeof(xinput_opcode_cache) / sizeof(xinput_opcode_cache[0]));
    pthread_mutex_unlock(&xinput_opcode_mutex);

    if (present && opcode_out) *opcode_out = opcode;
    return present ? 1 : 0;
}

#define DIAG_NAME_MAX 128
#define DIAG_VALUE_MAX 256
#define DIAG_ESCAPE_MAX (DIAG_VALUE_MAX * 4 + 8)
#define DIAG_INTERN_REQUESTS 256
#define DIAG_ATOM_NAMES 256
#define DIAG_PROPERTY_REQUESTS 512

typedef struct {
    int valid;
    xcb_connection_t *connection;
    uint32_t sequence;
    int unchecked;
    char name[DIAG_NAME_MAX];
} intern_request_t;

typedef struct {
    int valid;
    xcb_connection_t *connection;
    xcb_atom_t atom;
    char name[DIAG_NAME_MAX];
} atom_name_t;

typedef struct {
    int valid;
    xcb_connection_t *connection;
    uint32_t sequence;
    int unchecked;
    uint8_t delete;
    xcb_window_t window;
    xcb_atom_t property;
    xcb_atom_t type;
    uint32_t long_offset;
    uint32_t long_length;
    char property_name[DIAG_NAME_MAX];
    char type_name[DIAG_NAME_MAX];
} property_request_t;

static pthread_mutex_t diag_xcb_mutex = PTHREAD_MUTEX_INITIALIZER;
static intern_request_t intern_requests[DIAG_INTERN_REQUESTS];
static atom_name_t atom_names[DIAG_ATOM_NAMES];
static property_request_t property_requests[DIAG_PROPERTY_REQUESTS];
static size_t intern_requests_next = 0;
static size_t atom_names_next = 0;
static size_t property_requests_next = 0;

static void copy_len_string(char *dst, size_t dst_size, const char *src, size_t len) {
    if (!dst || dst_size == 0) return;
    if (!src) len = 0;
    if (len >= dst_size) len = dst_size - 1;
    if (len > 0) memcpy(dst, src, len);
    dst[len] = '\0';
}

static void escape_bytes(const void *data, size_t len, char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!data) {
        snprintf(out, out_size, "<null>");
        return;
    }

    const unsigned char *bytes = (const unsigned char *)data;
    size_t limit = len > DIAG_VALUE_MAX ? DIAG_VALUE_MAX : len;
    size_t used = 0;
    for (size_t i = 0; i < limit && used + 1 < out_size; i++) {
        unsigned char ch = bytes[i];
        if (ch == '\0') {
            if (used + 2 >= out_size) break;
            out[used++] = '\\';
            out[used++] = '0';
        } else if (ch == '\\' || ch == '"') {
            if (used + 2 >= out_size) break;
            out[used++] = '\\';
            out[used++] = (char)ch;
        } else if (isprint(ch)) {
            out[used++] = (char)ch;
        } else {
            if (used + 4 >= out_size) break;
            int written = snprintf(out + used, out_size - used, "\\x%02x", ch);
            if (written < 0 || (size_t)written >= out_size - used) {
                used = out_size - 1;
                break;
            }
            used += (size_t)written;
        }
    }
    if (len > DIAG_VALUE_MAX && used + 3 < out_size) {
        out[used++] = '.';
        out[used++] = '.';
        out[used++] = '.';
    }
    out[used] = '\0';
}

static const char *predefined_atom_name(xcb_atom_t atom) {
    switch (atom) {
        case XCB_ATOM_NONE: return "NONE/ANY";
        case XCB_ATOM_ATOM: return "ATOM";
        case XCB_ATOM_CARDINAL: return "CARDINAL";
        case XCB_ATOM_INTEGER: return "INTEGER";
        case XCB_ATOM_STRING: return "STRING";
        case XCB_ATOM_WINDOW: return "WINDOW";
        case XCB_ATOM_WM_NAME: return "WM_NAME";
        case XCB_ATOM_WM_CLASS: return "WM_CLASS";
        default: return NULL;
    }
}

static int lookup_atom_name_locked(xcb_connection_t *connection, xcb_atom_t atom, char *out, size_t out_size) {
    const char *predefined = predefined_atom_name(atom);
    if (predefined) {
        snprintf(out, out_size, "%s", predefined);
        return 1;
    }

    for (size_t i = 0; i < DIAG_ATOM_NAMES; i++) {
        size_t idx = (atom_names_next + DIAG_ATOM_NAMES - 1 - i) % DIAG_ATOM_NAMES;
        if (atom_names[idx].valid && atom_names[idx].atom == atom && atom_names[idx].connection == connection) {
            snprintf(out, out_size, "%s", atom_names[idx].name);
            return 1;
        }
    }
    return 0;
}

static int lookup_atom_name(xcb_connection_t *connection, xcb_atom_t atom, char *out, size_t out_size) {
    int found;
    pthread_mutex_lock(&diag_xcb_mutex);
    found = lookup_atom_name_locked(connection, atom, out, out_size);
    pthread_mutex_unlock(&diag_xcb_mutex);
    if (!found && out && out_size > 0) snprintf(out, out_size, "unknown");
    return found;
}

static int predefined_atom_by_name(const char *name, xcb_atom_t *atom_out) {
    if (!name || !atom_out) return 0;
    if (strcmp(name, "NONE") == 0 || strcmp(name, "ANY") == 0 || strcmp(name, "NONE/ANY") == 0) {
        *atom_out = XCB_ATOM_NONE;
        return 1;
    }
    if (strcmp(name, "ATOM") == 0) {
        *atom_out = XCB_ATOM_ATOM;
        return 1;
    }
    if (strcmp(name, "CARDINAL") == 0) {
        *atom_out = XCB_ATOM_CARDINAL;
        return 1;
    }
    if (strcmp(name, "INTEGER") == 0) {
        *atom_out = XCB_ATOM_INTEGER;
        return 1;
    }
    if (strcmp(name, "STRING") == 0) {
        *atom_out = XCB_ATOM_STRING;
        return 1;
    }
    if (strcmp(name, "WINDOW") == 0) {
        *atom_out = XCB_ATOM_WINDOW;
        return 1;
    }
    if (strcmp(name, "WM_NAME") == 0) {
        *atom_out = XCB_ATOM_WM_NAME;
        return 1;
    }
    if (strcmp(name, "WM_CLASS") == 0) {
        *atom_out = XCB_ATOM_WM_CLASS;
        return 1;
    }
    return 0;
}

static int lookup_atom_by_name(xcb_connection_t *connection, const char *name, xcb_atom_t *atom_out) {
    if (predefined_atom_by_name(name, atom_out)) return 1;
    if (!name || !atom_out) return 0;

    int found = 0;
    pthread_mutex_lock(&diag_xcb_mutex);
    for (size_t i = 0; i < DIAG_ATOM_NAMES; i++) {
        size_t idx = (atom_names_next + DIAG_ATOM_NAMES - 1 - i) % DIAG_ATOM_NAMES;
        if (atom_names[idx].valid && atom_names[idx].connection == connection &&
            strcmp(atom_names[idx].name, name) == 0) {
            *atom_out = atom_names[idx].atom;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&diag_xcb_mutex);
    return found;
}

static void store_atom_name_locked(xcb_connection_t *connection, xcb_atom_t atom, const char *name) {
    if (atom == XCB_ATOM_NONE || !name || name[0] == '\0') return;

    for (size_t i = 0; i < DIAG_ATOM_NAMES; i++) {
        if (atom_names[i].valid && atom_names[i].connection == connection && atom_names[i].atom == atom) {
            snprintf(atom_names[i].name, sizeof(atom_names[i].name), "%s", name);
            return;
        }
    }

    atom_names[atom_names_next].valid = 1;
    atom_names[atom_names_next].connection = connection;
    atom_names[atom_names_next].atom = atom;
    snprintf(atom_names[atom_names_next].name, sizeof(atom_names[atom_names_next].name), "%s", name);
    atom_names_next = (atom_names_next + 1) % DIAG_ATOM_NAMES;
}

static void store_intern_request(xcb_connection_t *connection, uint32_t sequence, int unchecked, const char *name) {
    pthread_mutex_lock(&diag_xcb_mutex);
    intern_requests[intern_requests_next].valid = 1;
    intern_requests[intern_requests_next].connection = connection;
    intern_requests[intern_requests_next].sequence = sequence;
    intern_requests[intern_requests_next].unchecked = unchecked;
    snprintf(intern_requests[intern_requests_next].name, sizeof(intern_requests[intern_requests_next].name), "%s", name ? name : "");
    intern_requests_next = (intern_requests_next + 1) % DIAG_INTERN_REQUESTS;
    pthread_mutex_unlock(&diag_xcb_mutex);
}

static int lookup_intern_request(xcb_connection_t *connection, uint32_t sequence, char *name, size_t name_size, int *unchecked) {
    int found = 0;
    pthread_mutex_lock(&diag_xcb_mutex);
    for (size_t i = 0; i < DIAG_INTERN_REQUESTS; i++) {
        size_t idx = (intern_requests_next + DIAG_INTERN_REQUESTS - 1 - i) % DIAG_INTERN_REQUESTS;
        if (intern_requests[idx].valid && intern_requests[idx].connection == connection && intern_requests[idx].sequence == sequence) {
            snprintf(name, name_size, "%s", intern_requests[idx].name);
            if (unchecked) *unchecked = intern_requests[idx].unchecked;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&diag_xcb_mutex);
    if (!found) {
        if (name && name_size > 0) snprintf(name, name_size, "unknown");
        if (unchecked) *unchecked = 0;
    }
    return found;
}

static int is_tracked_property_name(const char *name) {
    static const char *tracked[] = {
        "_NET_ACTIVE_WINDOW",
        "_NET_WM_PID",
        "_NET_WM_NAME",
        "WM_NAME",
        "WM_CLASS",
        "_NET_CLIENT_LIST",
        "_NET_CLIENT_LIST_STACKING",
    };

    if (!name) return 0;
    for (size_t i = 0; i < sizeof(tracked) / sizeof(tracked[0]); i++) {
        if (strcmp(name, tracked[i]) == 0) return 1;
    }
    return 0;
}

static void store_property_request(xcb_connection_t *connection, uint32_t sequence, int unchecked, uint8_t delete,
                                   xcb_window_t window, xcb_atom_t property, xcb_atom_t type,
                                   uint32_t long_offset, uint32_t long_length,
                                   char *property_name_out, size_t property_name_size,
                                   char *type_name_out, size_t type_name_size) {
    char property_name[DIAG_NAME_MAX];
    char type_name[DIAG_NAME_MAX];

    pthread_mutex_lock(&diag_xcb_mutex);
    if (!lookup_atom_name_locked(connection, property, property_name, sizeof(property_name)))
        snprintf(property_name, sizeof(property_name), "unknown");
    if (!lookup_atom_name_locked(connection, type, type_name, sizeof(type_name)))
        snprintf(type_name, sizeof(type_name), "unknown");

    property_requests[property_requests_next].valid = 1;
    property_requests[property_requests_next].connection = connection;
    property_requests[property_requests_next].sequence = sequence;
    property_requests[property_requests_next].unchecked = unchecked;
    property_requests[property_requests_next].delete = delete;
    property_requests[property_requests_next].window = window;
    property_requests[property_requests_next].property = property;
    property_requests[property_requests_next].type = type;
    property_requests[property_requests_next].long_offset = long_offset;
    property_requests[property_requests_next].long_length = long_length;
    snprintf(property_requests[property_requests_next].property_name, sizeof(property_requests[property_requests_next].property_name), "%s", property_name);
    snprintf(property_requests[property_requests_next].type_name, sizeof(property_requests[property_requests_next].type_name), "%s", type_name);
    property_requests_next = (property_requests_next + 1) % DIAG_PROPERTY_REQUESTS;
    pthread_mutex_unlock(&diag_xcb_mutex);

    if (property_name_out && property_name_size > 0) snprintf(property_name_out, property_name_size, "%s", property_name);
    if (type_name_out && type_name_size > 0) snprintf(type_name_out, type_name_size, "%s", type_name);
}

static int lookup_property_request(xcb_connection_t *connection, uint32_t sequence, property_request_t *out) {
    int found = 0;
    pthread_mutex_lock(&diag_xcb_mutex);
    for (size_t i = 0; i < DIAG_PROPERTY_REQUESTS; i++) {
        size_t idx = (property_requests_next + DIAG_PROPERTY_REQUESTS - 1 - i) % DIAG_PROPERTY_REQUESTS;
        if (property_requests[idx].valid && property_requests[idx].connection == connection && property_requests[idx].sequence == sequence) {
            if (out) *out = property_requests[idx];
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&diag_xcb_mutex);
    return found;
}

static void format_window_list(const void *value, int value_len, char *out, size_t out_size, size_t *count_out) {
    const unsigned char *bytes = (const unsigned char *)value;
    size_t count = value_len > 0 ? (size_t)value_len / sizeof(uint32_t) : 0;
    size_t limit = count > 8 ? 8 : count;
    size_t used = 0;

    if (out_size > 0) out[0] = '\0';
    for (size_t i = 0; i < limit && out_size > used; i++) {
        uint32_t window = 0;
        memcpy(&window, bytes + i * sizeof(uint32_t), sizeof(window));
        int written = snprintf(out + used, out_size - used, "%s0x%08x", i ? "," : "", window);
        if (written < 0 || (size_t)written >= out_size - used) {
            used = out_size - 1;
            break;
        }
        used += (size_t)written;
    }
    if (count > limit && out_size > used + 4) snprintf(out + used, out_size - used, ",...");
    if (count_out) *count_out = count;
}

static void format_property_detail(const property_request_t *request, xcb_get_property_reply_t *reply,
                                   char *detail, size_t detail_size) {
    if (!detail || detail_size == 0) return;
    detail[0] = '\0';
    if (!request || !reply || !is_tracked_property_name(request->property_name)) return;

    int value_len = xcb_get_property_value_length(reply);
    const void *value = xcb_get_property_value(reply);
    if (value_len < 0) value_len = 0;

    if ((strcmp(request->property_name, "_NET_ACTIVE_WINDOW") == 0 ||
         strcmp(request->property_name, "_NET_WM_PID") == 0) &&
        reply->format == 32 && value && value_len >= (int)sizeof(uint32_t)) {
        uint32_t first = 0;
        memcpy(&first, value, sizeof(first));
        if (strcmp(request->property_name, "_NET_ACTIVE_WINDOW") == 0)
            snprintf(detail, detail_size, " value_window=0x%08x", first);
        else
            snprintf(detail, detail_size, " pid=%u", first);
    } else if ((strcmp(request->property_name, "_NET_CLIENT_LIST") == 0 ||
                strcmp(request->property_name, "_NET_CLIENT_LIST_STACKING") == 0) &&
               reply->format == 32 && value) {
        char windows[256];
        size_t count = 0;
        format_window_list(value, value_len, windows, sizeof(windows), &count);
        snprintf(detail, detail_size, " count=%zu windows=%s", count, windows[0] ? windows : "none");
    } else if (strcmp(request->property_name, "_NET_WM_NAME") == 0 ||
               strcmp(request->property_name, "WM_NAME") == 0 ||
               strcmp(request->property_name, "WM_CLASS") == 0) {
        char text[DIAG_ESCAPE_MAX];
        escape_bytes(value, (size_t)value_len, text, sizeof(text));
        snprintf(detail, detail_size, " text=\"%s\"", text);
    }
}

static int is_root_window(xcb_connection_t *c, xcb_drawable_t drawable);

typedef enum {
    ACTIVE_CACHE_STATUS_LOADED = 0,
    ACTIVE_CACHE_STATUS_NO_PATH,
    ACTIVE_CACHE_STATUS_MISSING,
    ACTIVE_CACHE_STATUS_READ_FAILED,
    ACTIVE_CACHE_STATUS_INVALID,
    ACTIVE_CACHE_STATUS_STALE,
    ACTIVE_CACHE_STATUS_EMPTY,
    ACTIVE_CACHE_STATUS_NON_NORMAL,
} active_cache_status_t;

static pthread_mutex_t active_window_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static sd_active_window_state_t active_window_cached_state;
static int active_window_cached_usable = 0;
static uint64_t active_window_cache_checked_ms = 0;
static int active_window_last_logged_status = -1;
static uint64_t active_window_last_status_log_ms = 0;
static uint64_t active_window_last_loaded_log_update_ms = 0;

/* In-memory active-window state, published by the in-process DBus service
 * (fed by the KWin script). Replaces the on-disk state file. */
static pthread_mutex_t active_window_service_mutex = PTHREAD_MUTEX_INITIALIZER;
static sd_active_window_state_t active_window_service_state;
static int active_window_service_has_state = 0;

static uint64_t active_window_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static const char *active_cache_status_name(active_cache_status_t status) {
    switch (status) {
        case ACTIVE_CACHE_STATUS_LOADED: return "loaded";
        case ACTIVE_CACHE_STATUS_NO_PATH: return "no-path";
        case ACTIVE_CACHE_STATUS_MISSING: return "missing";
        case ACTIVE_CACHE_STATUS_READ_FAILED: return "read-failed";
        case ACTIVE_CACHE_STATUS_INVALID: return "invalid";
        case ACTIVE_CACHE_STATUS_STALE: return "stale";
        case ACTIVE_CACHE_STATUS_EMPTY: return "empty";
        case ACTIVE_CACHE_STATUS_NON_NORMAL: return "non-normal";
        default: return "unknown";
    }
}

static int active_window_has_nul(const char *value, size_t size) {
    return memchr(value, '\0', size) != NULL;
}

static int active_window_state_strings_valid(const sd_active_window_state_t *state) {
    return active_window_has_nul(state->title, sizeof(state->title)) &&
           active_window_has_nul(state->resource_class, sizeof(state->resource_class)) &&
           active_window_has_nul(state->resource_name, sizeof(state->resource_name)) &&
           active_window_has_nul(state->desktop_file, sizeof(state->desktop_file)) &&
           active_window_has_nul(state->internal_id, sizeof(state->internal_id));
}

static void active_window_log_cache_status(active_cache_status_t status,
                                           const sd_active_window_state_t *state,
                                           uint64_t now_ms, uint64_t age_ms) {
    if (!active_window_diag_enabled()) return;

    if (status == ACTIVE_CACHE_STATUS_LOADED) {
        if (!state || active_window_last_loaded_log_update_ms == state->updated_monotonic_ms) return;
        active_window_last_loaded_log_update_ms = state->updated_monotonic_ms;
        diag_logmsg("active-window cache loaded title=\"%s\" pid=%u class=\"%s\" age_ms=%llu source=dbus",
                    state->title, state->pid, state->resource_class,
                    (unsigned long long)age_ms);
        active_window_last_logged_status = status;
        active_window_last_status_log_ms = now_ms;
        return;
    }

    if (active_window_last_logged_status == status &&
        active_window_last_status_log_ms != 0 &&
        now_ms >= active_window_last_status_log_ms &&
        now_ms - active_window_last_status_log_ms < 5000) {
        return;
    }

    active_window_last_logged_status = status;
    active_window_last_status_log_ms = now_ms;
    if (status == ACTIVE_CACHE_STATUS_STALE) {
        diag_logmsg("active-window cache stale age_ms=%llu ttl_ms=%u source=dbus",
                    (unsigned long long)age_ms, active_window_ttl_ms);
    } else {
        diag_logmsg("active-window cache %s source=dbus",
                    active_cache_status_name(status));
    }
}

static int active_window_read_state(sd_active_window_state_t *state_out,
                                    active_cache_status_t *status_out,
                                    uint64_t now_ms, uint64_t *age_ms_out) {
    sd_active_window_state_t state;
    memset(&state, 0, sizeof(state));
    if (age_ms_out) *age_ms_out = 0;

    /* Snapshot the state published by the in-process DBus service. */
    pthread_mutex_lock(&active_window_service_mutex);
    int have_state = active_window_service_has_state;
    if (have_state) state = active_window_service_state;
    pthread_mutex_unlock(&active_window_service_mutex);

    if (!have_state) {
        if (status_out) *status_out = ACTIVE_CACHE_STATUS_MISSING;
        return 0;
    }

    if (state.magic != SD_ACTIVE_WINDOW_MAGIC || state.version != SD_ACTIVE_WINDOW_VERSION ||
        !active_window_state_strings_valid(&state)) {
        if (status_out) *status_out = ACTIVE_CACHE_STATUS_INVALID;
        return 0;
    }

    if ((state.flags & SD_ACTIVE_WINDOW_FLAG_VALID) == 0) {
        if (status_out) *status_out = ACTIVE_CACHE_STATUS_EMPTY;
        return 0;
    }
    if ((state.flags & SD_ACTIVE_WINDOW_FLAG_NORMAL) == 0) {
        if (status_out) *status_out = ACTIVE_CACHE_STATUS_NON_NORMAL;
        return 0;
    }

    uint64_t age_ms = 0;
    if (state.updated_monotonic_ms > now_ms + 1000u) {
        if (status_out) *status_out = ACTIVE_CACHE_STATUS_INVALID;
        return 0;
    }
    if (now_ms >= state.updated_monotonic_ms) age_ms = now_ms - state.updated_monotonic_ms;
    if (age_ms_out) *age_ms_out = age_ms;
    if (age_ms > active_window_ttl_ms) {
        if (status_out) *status_out = ACTIVE_CACHE_STATUS_STALE;
        return 0;
    }

    if (state_out) *state_out = state;
    if (status_out) *status_out = ACTIVE_CACHE_STATUS_LOADED;
    return 1;
}

/* ---- Active-window bridge: in-process DBus service -------------------- */

/*
 * A dedicated thread owns the org.screen_doctor.ActiveWindow bus name and
 * receives Update() calls from the KWin script, publishing the active window
 * into memory. This replaces the external screen-doctor-active-window-helper
 * process and its state file. Do not run that standalone helper alongside
 * Time Doctor - both would contend for the same bus name.
 */
#define SD_AW_DBUS_NAME "org.screen_doctor.ActiveWindow"
#define SD_AW_DBUS_OBJECT_PATH "/org/screen_doctor/ActiveWindow"

static const gchar aw_introspection_xml[] =
    "<node>"
    "  <interface name='org.screen_doctor.ActiveWindow'>"
    "    <method name='Update'>"
    "      <arg type='s' name='title' direction='in'/>"
    "      <arg type='s' name='resource_class' direction='in'/>"
    "      <arg type='s' name='resource_name' direction='in'/>"
    "      <arg type='s' name='desktop_file' direction='in'/>"
    "      <arg type='i' name='pid' direction='in'/>"
    "      <arg type='b' name='normal_window' direction='in'/>"
    "      <arg type='s' name='internal_id' direction='in'/>"
    "    </method>"
    "    <method name='Ping'><arg type='s' name='status' direction='out'/></method>"
    "  </interface>"
    "</node>";

static pthread_once_t active_window_service_once = PTHREAD_ONCE_INIT;

static void aw_copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_size, "%s", src);
}

static int aw_string_nonempty(const char *v) { return v && v[0] != '\0'; }

static void aw_desktop_basename(char *dst, size_t dst_size, const char *desktop_file) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!aw_string_nonempty(desktop_file)) return;
    const char *base = strrchr(desktop_file, '/');
    base = base ? base + 1 : desktop_file;
    aw_copy_string(dst, dst_size, base);
    size_t len = strlen(dst);
    const char suffix[] = ".desktop";
    size_t suffix_len = sizeof(suffix) - 1;
    if (len > suffix_len && strcmp(dst + len - suffix_len, suffix) == 0)
        dst[len - suffix_len] = '\0';
}

static void aw_lowercase_simple(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;
    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_size; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            dst[out++] = (char)tolower(ch);
    }
    dst[out] = '\0';
}

static void aw_fill_state(sd_active_window_state_t *state, const char *title,
                          const char *resource_class, const char *resource_name,
                          const char *desktop_file, gint32 pid, gboolean normal_window,
                          const char *internal_id) {
    memset(state, 0, sizeof(*state));
    state->magic = SD_ACTIVE_WINDOW_MAGIC;
    state->version = SD_ACTIVE_WINDOW_VERSION;
    state->updated_monotonic_ms = active_window_monotonic_ms();

    int valid = aw_string_nonempty(title) || aw_string_nonempty(resource_class) ||
                aw_string_nonempty(resource_name) || aw_string_nonempty(desktop_file) ||
                aw_string_nonempty(internal_id) || pid > 0 || normal_window;

    if (pid > 0) {
        state->pid = (uint32_t)pid;
        state->flags |= SD_ACTIVE_WINDOW_FLAG_HAS_PID;
    }
    if (valid) state->flags |= SD_ACTIVE_WINDOW_FLAG_VALID;
    if (normal_window) state->flags |= SD_ACTIVE_WINDOW_FLAG_NORMAL;

    aw_copy_string(state->title, sizeof(state->title), title);
    aw_copy_string(state->desktop_file, sizeof(state->desktop_file), desktop_file);
    aw_copy_string(state->internal_id, sizeof(state->internal_id), internal_id);

    if (aw_string_nonempty(resource_class)) {
        aw_copy_string(state->resource_class, sizeof(state->resource_class), resource_class);
    } else {
        aw_desktop_basename(state->resource_class, sizeof(state->resource_class), desktop_file);
        if (!aw_string_nonempty(state->resource_class) && aw_string_nonempty(resource_name))
            aw_copy_string(state->resource_class, sizeof(state->resource_class), resource_name);
        if (!aw_string_nonempty(state->resource_class) && valid)
            aw_copy_string(state->resource_class, sizeof(state->resource_class), "unknown");
    }

    if (aw_string_nonempty(resource_name)) {
        aw_copy_string(state->resource_name, sizeof(state->resource_name), resource_name);
    } else if (aw_string_nonempty(state->resource_class)) {
        aw_lowercase_simple(state->resource_name, sizeof(state->resource_name), state->resource_class);
        if (!aw_string_nonempty(state->resource_name))
            aw_copy_string(state->resource_name, sizeof(state->resource_name), state->resource_class);
    }
}

static void aw_handle_method_call(GDBusConnection *connection, const gchar *sender,
                                  const gchar *object_path, const gchar *interface_name,
                                  const gchar *method_name, GVariant *parameters,
                                  GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)connection; (void)sender; (void)object_path; (void)interface_name; (void)user_data;

    if (g_strcmp0(method_name, "Ping") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "ok"));
        return;
    }
    if (g_strcmp0(method_name, "Update") == 0) {
        const char *title = "", *resource_class = "", *resource_name = "";
        const char *desktop_file = "", *internal_id = "";
        gint32 pid = 0;
        gboolean normal_window = FALSE;
        g_variant_get(parameters, "(&s&s&s&sib&s)", &title, &resource_class,
                      &resource_name, &desktop_file, &pid, &normal_window, &internal_id);

        sd_active_window_state_t st;
        aw_fill_state(&st, title, resource_class, resource_name, desktop_file, pid,
                      normal_window, internal_id);
        pthread_mutex_lock(&active_window_service_mutex);
        active_window_service_state = st;
        active_window_service_has_state = 1;
        pthread_mutex_unlock(&active_window_service_mutex);

        if (active_window_diag_enabled())
            diag_logmsg("active-window service update title=\"%s\" class=\"%s\" pid=%u flags=0x%x",
                        st.title, st.resource_class, st.pid, st.flags);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }
    g_dbus_method_invocation_return_error(invocation, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                          "unknown method %s", method_name);
}

static const GDBusInterfaceVTable aw_vtable = { aw_handle_method_call, NULL, NULL, {0} };
static GDBusNodeInfo *aw_introspection_data;

static void aw_on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)name; (void)user_data;
    GError *error = NULL;
    g_dbus_connection_register_object(connection, SD_AW_DBUS_OBJECT_PATH,
                                      aw_introspection_data->interfaces[0], &aw_vtable,
                                      NULL, NULL, &error);
    if (error) {
        if (active_window_diag_enabled())
            diag_logmsg("active-window service register failed: %s", error->message);
        g_clear_error(&error);
    }
}

/* Keep the published state's timestamp fresh while it is valid, so the
 * preload-side TTL means "the KWin script stopped updating", not merely
 * "the focused window has not changed". Mirrors the old helper's 2s refresh. */
static gboolean aw_refresh_timestamp(gpointer user_data) {
    (void)user_data;
    pthread_mutex_lock(&active_window_service_mutex);
    if (active_window_service_has_state &&
        (active_window_service_state.flags & SD_ACTIVE_WINDOW_FLAG_VALID)) {
        active_window_service_state.updated_monotonic_ms = active_window_monotonic_ms();
    }
    pthread_mutex_unlock(&active_window_service_mutex);
    return G_SOURCE_CONTINUE;
}

static void *active_window_service_thread(void *arg) {
    (void)arg;
    GMainContext *ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);

    GError *error = NULL;
    aw_introspection_data = g_dbus_node_info_new_for_xml(aw_introspection_xml, &error);
    if (!aw_introspection_data) {
        if (active_window_diag_enabled())
            diag_logmsg("active-window service introspection failed: %s",
                        error ? error->message : "unknown");
        g_clear_error(&error);
        g_main_context_pop_thread_default(ctx);
        g_main_context_unref(ctx);
        return NULL;
    }

    guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, SD_AW_DBUS_NAME,
                                    G_BUS_NAME_OWNER_FLAGS_NONE,
                                    aw_on_bus_acquired, NULL, NULL, NULL, NULL);
    if (active_window_diag_enabled())
        diag_logmsg("active-window service owning %s (in-process)", SD_AW_DBUS_NAME);

    /* Attach the refresh timer to THIS thread's context. g_timeout_add_*
     * would attach to the global default context, which this thread never
     * iterates, so the source is created and attached to ctx explicitly. */
    GSource *refresh_src = g_timeout_source_new_seconds(2);
    g_source_set_callback(refresh_src, aw_refresh_timestamp, NULL, NULL);
    g_source_attach(refresh_src, ctx);
    g_source_unref(refresh_src);

    GMainLoop *loop = g_main_loop_new(ctx, FALSE);
    g_main_loop_run(loop);

    g_bus_unown_name(owner_id);
    g_main_loop_unref(loop);
    g_main_context_pop_thread_default(ctx);
    g_main_context_unref(ctx);
    return NULL;
}

static void active_window_start_service_once(void) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, active_window_service_thread, NULL) == 0)
        pthread_detach(tid);
}

static int active_window_load_state(sd_active_window_state_t *state_out) {
    if (!active_window_enabled()) return 0;
    pthread_once(&active_window_service_once, active_window_start_service_once);

    uint64_t now_ms = active_window_monotonic_ms();
    pthread_mutex_lock(&active_window_cache_mutex);
    if (active_window_cache_checked_ms != 0 && now_ms >= active_window_cache_checked_ms &&
        now_ms - active_window_cache_checked_ms < 100) {
        if (active_window_cached_usable && state_out) *state_out = active_window_cached_state;
        int usable = active_window_cached_usable;
        pthread_mutex_unlock(&active_window_cache_mutex);
        return usable;
    }

    active_cache_status_t status = ACTIVE_CACHE_STATUS_INVALID;
    uint64_t age_ms = 0;
    sd_active_window_state_t state;
    int usable = active_window_read_state(&state, &status, now_ms, &age_ms);
    if (usable) active_window_cached_state = state;
    active_window_cached_usable = usable;
    active_window_cache_checked_ms = now_ms;
    active_window_log_cache_status(status, usable ? &state : NULL, now_ms, age_ms);
    if (usable && state_out) *state_out = state;
    pthread_mutex_unlock(&active_window_cache_mutex);
    return usable;
}

/* ---- Input-activity bridge: in-process Wayland idle watcher ----------- */

/*
 * A dedicated thread inside this library opens its own Wayland connection
 * and subscribes to ext_idle_notifier_v1 (input-idle variant on v2+). While
 * the compositor reports the user is active it keeps an in-memory timestamp
 * fresh; the synthesizer reads that timestamp directly. No helper process
 * and no state file are involved.
 *
 * libwayland-client is loaded with dlopen at runtime, so the library keeps
 * no build-time dependency on it and simply no-ops if Wayland is absent.
 * Only the two ext_idle_* protocol interfaces are defined by hand; the core
 * wl_registry / wl_seat interfaces are resolved from libwayland itself.
 */

/* Minimal libwayland-client ABI (no dev headers required). */
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

typedef struct wl_display *(*wl_display_connect_fn)(const char *);
typedef void (*wl_display_disconnect_fn)(struct wl_display *);
typedef int (*wl_display_get_fd_fn)(struct wl_display *);
typedef int (*wl_display_roundtrip_fn)(struct wl_display *);
typedef int (*wl_display_dispatch_fn)(struct wl_display *);
typedef int (*wl_display_dispatch_pending_fn)(struct wl_display *);
typedef int (*wl_display_flush_fn)(struct wl_display *);
typedef struct wl_proxy *(*wl_proxy_marshal_flags_fn)(struct wl_proxy *, uint32_t,
                                                      const struct wl_interface *,
                                                      uint32_t, uint32_t, ...);
typedef int (*wl_proxy_add_listener_fn)(struct wl_proxy *, void (**)(void), void *);
typedef void (*wl_proxy_destroy_fn)(struct wl_proxy *);
typedef uint32_t (*wl_proxy_get_version_fn)(struct wl_proxy *);

static wl_display_connect_fn wl_display_connect_p;
static wl_display_disconnect_fn wl_display_disconnect_p;
static wl_display_get_fd_fn wl_display_get_fd_p;
static wl_display_roundtrip_fn wl_display_roundtrip_p;
static wl_display_dispatch_fn wl_display_dispatch_p;
static wl_display_dispatch_pending_fn wl_display_dispatch_pending_p;
static wl_display_flush_fn wl_display_flush_p;
static wl_proxy_marshal_flags_fn wl_proxy_marshal_flags_p;
static wl_proxy_add_listener_fn wl_proxy_add_listener_p;
static wl_proxy_destroy_fn wl_proxy_destroy_p;
static wl_proxy_get_version_fn wl_proxy_get_version_p;
static const struct wl_interface *wl_registry_interface_p;
static const struct wl_interface *wl_seat_interface_p;

#define WL_DISPLAY_GET_REGISTRY 1
#define WL_REGISTRY_BIND 0
#define EXT_IDLE_NOTIFIER_V1_GET_IDLE_NOTIFICATION 1
#define EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION 2
#define SD_IDLE_TIMEOUT_MS 1000u

static const struct wl_interface ext_idle_notification_v1_interface;
static const struct wl_interface ext_idle_notifier_v1_interface;

/* [0]=notification new_id, [1]=timeout(uint,no type), [2]=wl_seat (filled at runtime). */
static const struct wl_interface *idle_arg_types[3];

static const struct wl_message ext_idle_notifier_v1_requests[] = {
    { "destroy", "", idle_arg_types },
    { "get_idle_notification", "nuo", idle_arg_types },
    { "get_input_idle_notification", "2nuo", idle_arg_types },
};
static const struct wl_interface ext_idle_notifier_v1_interface = {
    "ext_idle_notifier_v1", 2, 3, ext_idle_notifier_v1_requests, 0, NULL,
};
static const struct wl_message ext_idle_notification_v1_requests[] = {
    { "destroy", "", idle_arg_types },
};
static const struct wl_message ext_idle_notification_v1_events[] = {
    { "idled", "", idle_arg_types },
    { "resumed", "", idle_arg_types },
};
static const struct wl_interface ext_idle_notification_v1_interface = {
    "ext_idle_notification_v1", 2, 1, ext_idle_notification_v1_requests,
    2, ext_idle_notification_v1_events,
};

static pthread_mutex_t activity_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t activity_last_synth_ms = 0;
static int activity_last_logged_fresh = -1;

/* In-memory activity state, updated by the Wayland thread. */
static uint64_t activity_mem_last_activity_ms = 0;
static int activity_mem_active = 0;
static int activity_mem_valid = 0;

static pthread_once_t activity_thread_once = PTHREAD_ONCE_INIT;
static struct wl_proxy *activity_wl_notifier;
static struct wl_proxy *activity_wl_seat;
static uint32_t activity_wl_notifier_name, activity_wl_seat_name;
static uint32_t activity_wl_notifier_version;

static void activity_mem_update(int is_active) {
    uint64_t now = active_window_monotonic_ms();
    pthread_mutex_lock(&activity_cache_mutex);
    activity_mem_active = is_active;
    activity_mem_valid = 1;
    if (is_active) activity_mem_last_activity_ms = now;
    pthread_mutex_unlock(&activity_cache_mutex);
}

static void activity_notification_idled(void *data, struct wl_proxy *n) {
    (void)data; (void)n;
    activity_mem_update(0);
}
static void activity_notification_resumed(void *data, struct wl_proxy *n) {
    (void)data; (void)n;
    activity_mem_update(1);
}
static void (*activity_notification_listener[2])(void) = {
    (void (*)(void))activity_notification_idled,
    (void (*)(void))activity_notification_resumed,
};

static void activity_registry_global(void *data, struct wl_proxy *registry, uint32_t name,
                                     const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, "wl_seat") == 0 && !activity_wl_seat) {
        uint32_t v = version < 4 ? version : 4;
        activity_wl_seat = wl_proxy_marshal_flags_p(registry, WL_REGISTRY_BIND,
                                                    wl_seat_interface_p, v, 0, name,
                                                    wl_seat_interface_p->name, v, NULL);
        activity_wl_seat_name = name;
    } else if (strcmp(interface, "ext_idle_notifier_v1") == 0 && !activity_wl_notifier) {
        activity_wl_notifier_version = version < 2 ? version : 2;
        activity_wl_notifier = wl_proxy_marshal_flags_p(registry, WL_REGISTRY_BIND,
                                                        &ext_idle_notifier_v1_interface,
                                                        activity_wl_notifier_version, 0, name,
                                                        ext_idle_notifier_v1_interface.name,
                                                        activity_wl_notifier_version, NULL);
        activity_wl_notifier_name = name;
    }
}
static void activity_registry_global_remove(void *data, struct wl_proxy *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}
static void (*activity_registry_listener[2])(void) = {
    (void (*)(void))activity_registry_global,
    (void (*)(void))activity_registry_global_remove,
};

static int activity_resolve_wayland(void) {
    void *h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
    if (!h) return 0;
    wl_display_connect_p = (wl_display_connect_fn)dlsym(h, "wl_display_connect");
    wl_display_disconnect_p = (wl_display_disconnect_fn)dlsym(h, "wl_display_disconnect");
    wl_display_get_fd_p = (wl_display_get_fd_fn)dlsym(h, "wl_display_get_fd");
    wl_display_roundtrip_p = (wl_display_roundtrip_fn)dlsym(h, "wl_display_roundtrip");
    wl_display_dispatch_p = (wl_display_dispatch_fn)dlsym(h, "wl_display_dispatch");
    wl_display_dispatch_pending_p = (wl_display_dispatch_pending_fn)dlsym(h, "wl_display_dispatch_pending");
    wl_display_flush_p = (wl_display_flush_fn)dlsym(h, "wl_display_flush");
    wl_proxy_marshal_flags_p = (wl_proxy_marshal_flags_fn)dlsym(h, "wl_proxy_marshal_flags");
    wl_proxy_add_listener_p = (wl_proxy_add_listener_fn)dlsym(h, "wl_proxy_add_listener");
    wl_proxy_destroy_p = (wl_proxy_destroy_fn)dlsym(h, "wl_proxy_destroy");
    wl_proxy_get_version_p = (wl_proxy_get_version_fn)dlsym(h, "wl_proxy_get_version");
    wl_registry_interface_p = (const struct wl_interface *)dlsym(h, "wl_registry_interface");
    wl_seat_interface_p = (const struct wl_interface *)dlsym(h, "wl_seat_interface");

    if (!wl_display_connect_p || !wl_display_disconnect_p || !wl_display_get_fd_p ||
        !wl_display_roundtrip_p || !wl_display_dispatch_p || !wl_display_dispatch_pending_p ||
        !wl_display_flush_p || !wl_proxy_marshal_flags_p || !wl_proxy_add_listener_p ||
        !wl_proxy_destroy_p || !wl_proxy_get_version_p || !wl_registry_interface_p ||
        !wl_seat_interface_p) {
        return 0;
    }
    idle_arg_types[0] = &ext_idle_notification_v1_interface;
    idle_arg_types[1] = NULL;
    idle_arg_types[2] = wl_seat_interface_p;
    return 1;
}

static void *activity_wayland_thread(void *arg) {
    (void)arg;
    if (!activity_resolve_wayland()) {
        if (diag_xinput_enabled())
            diag_logmsg("diag:activity wayland unavailable (dlopen/resolve failed)");
        return NULL;
    }

    struct wl_display *display = wl_display_connect_p(NULL);
    if (!display) {
        if (diag_xinput_enabled())
            diag_logmsg("diag:activity wayland connect failed");
        return NULL;
    }

    struct wl_proxy *registry = wl_proxy_marshal_flags_p(
        (struct wl_proxy *)display, WL_DISPLAY_GET_REGISTRY, wl_registry_interface_p,
        wl_proxy_get_version_p((struct wl_proxy *)display), 0, NULL);
    wl_proxy_add_listener_p(registry, activity_registry_listener, NULL);
    wl_display_roundtrip_p(display);

    if (!activity_wl_notifier || !activity_wl_seat) {
        if (diag_xinput_enabled())
            diag_logmsg("diag:activity compositor lacks %s",
                        !activity_wl_notifier ? "ext_idle_notifier_v1" : "wl_seat");
        wl_display_disconnect_p(display);
        return NULL;
    }

    uint32_t opcode = (activity_wl_notifier_version >= 2)
                          ? EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION
                          : EXT_IDLE_NOTIFIER_V1_GET_IDLE_NOTIFICATION;
    struct wl_proxy *notification = wl_proxy_marshal_flags_p(
        activity_wl_notifier, opcode, &ext_idle_notification_v1_interface,
        wl_proxy_get_version_p(activity_wl_notifier), 0, NULL, SD_IDLE_TIMEOUT_MS,
        activity_wl_seat);
    if (!notification) {
        wl_display_disconnect_p(display);
        return NULL;
    }
    wl_proxy_add_listener_p(notification, activity_notification_listener, NULL);
    wl_display_flush_p(display);

    if (diag_xinput_enabled())
        diag_logmsg("diag:activity wayland watching via %s",
                    opcode == EXT_IDLE_NOTIFIER_V1_GET_INPUT_IDLE_NOTIFICATION
                        ? "get_input_idle_notification" : "get_idle_notification");

    /* Assume active at startup so genuine work is covered immediately; the
     * first idled event (after the timeout of quiet) corrects it. */
    activity_mem_update(1);

    int fd = wl_display_get_fd_p(display);
    for (;;) {
        wl_display_dispatch_pending_p(display);
        wl_display_flush_p(display);

        int is_active;
        pthread_mutex_lock(&activity_cache_mutex);
        is_active = activity_mem_active;
        pthread_mutex_unlock(&activity_cache_mutex);

        struct pollfd pfd = { fd, POLLIN, 0 };
        int r = poll(&pfd, 1, is_active ? 500 : -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch_p(display) < 0) break;
        }
        /* While active, keep the in-memory stamp fresh (no per-input events
         * arrive between resumed and idled). */
        if (is_active) activity_mem_update(1);
    }

    wl_display_disconnect_p(display);
    return NULL;
}

static void activity_start_wayland_thread_once(void) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, activity_wayland_thread, NULL) == 0) {
        pthread_detach(tid);
    }
}

/* Returns 1 if the user is currently active (Wayland thread reports ACTIVE
 * and the last-activity stamp is within TTL). Lazily starts the watcher. */
static int activity_is_fresh(void) {
    if (!activity_enabled()) return 0;
    pthread_once(&activity_thread_once, activity_start_wayland_thread_once);

    uint64_t now_ms = active_window_monotonic_ms();
    pthread_mutex_lock(&activity_cache_mutex);
    int fresh = 0;
    if (activity_mem_valid && activity_mem_active) {
        uint64_t last = activity_mem_last_activity_ms;
        uint64_t age = (now_ms >= last) ? now_ms - last : 0;
        if (age <= activity_ttl_ms) fresh = 1;
    }
    if (diag_xinput_enabled() && fresh != activity_last_logged_fresh) {
        activity_last_logged_fresh = fresh;
        diag_logmsg("diag:activity state fresh=%d (in-process wayland watcher)", fresh);
    }
    pthread_mutex_unlock(&activity_cache_mutex);
    return fresh;
}

/* ---- Input-activity bridge: synthesize XI_RawMotion ------------------- */

/*
 * Time Doctor's idle monitor runs a dedicated thread with its own Xlib
 * Display: it selects XI raw events on the root window and blocks in
 * XNextEvent. Native Wayland input never reaches that XWayland connection,
 * so we fabricate periodic RawMotion events while the helper reports the
 * user is active. The fabricated cookie carries a sentinel in its cookie
 * id so XGetEventData/XFreeEventData can recognize and service it without
 * touching real Xlib.
 */
#define SD_SYNTH_COOKIE_SENTINEL 0x5D000000u
#define SD_SYNTH_COOKIE_MASK 0xFF000000u

static pthread_mutex_t activity_synth_mutex = PTHREAD_MUTEX_INITIALIZER;
static Display *activity_monitor_display = NULL; /* connection that selected raw events */
static int activity_monitor_fd = -1;             /* ConnectionNumber of that display */
/* Time Doctor (Electron/Chromium) registers the monitor fd with an epoll
 * instance rather than polling it directly, so we also learn which epoll fd
 * watches it (and the epoll_event.data it registered, to replay on inject). */
static int activity_monitor_epfd = -1;
static uint64_t activity_monitor_epoll_data = 0;
static int activity_monitor_epoll_data_valid = 0;
/*
 * The monitor fd is only recognized once XISelectEvents runs, but Time Doctor's
 * Chromium/Qt event loop registers that fd with its epoll instance earlier, at
 * Display-connection setup. So the epoll_ctl for the monitor fd can arrive
 * before we know which fd is the monitor, and the forward "fd == monitor" guard
 * misses it. We therefore remember recent EPOLL_CTL_ADD/MOD registrations in a
 * small ring and reconcile against it when the raw-motion subscription is noted.
 */
#define SD_EPOLL_TABLE_SIZE 256
typedef struct {
    int fd;
    int epfd;
    uint64_t data;
} sd_epoll_reg_t;
static sd_epoll_reg_t activity_epoll_table[SD_EPOLL_TABLE_SIZE];
static int activity_epoll_table_head = 0; /* next write slot */
static unsigned int activity_synth_counter = 0;
static int activity_pointer_deviceid = 2; /* plausible master pointer defaults */
static int activity_pointer_sourceid = 2;
/*
 * Time Doctor integrates the raw-input Xlib connection into its main Qt
 * event loop (a poll/ppoll over many fds), draining via XPending/XNextEvent
 * from a callback - it never blocks inside XNextEvent. So we deliver at the
 * poll layer: when the monitor fd is in a poll set and the user is active,
 * we mark that fd readable at most once per rate interval and raise
 * activity_synth_pending; the XPending/XNextEvent hooks then hand back one
 * synthetic RawMotion. All other poll calls pass through untouched.
 */
static int activity_synth_pending = 0;

/* ---- Profile mode: human-like synthetic event stream ------------------ */
/*
 * The simple path emits one bare RawMotion per rate interval. That reads as a
 * machine signature: perfectly periodic, no keyboard, empty valuators. Profile
 * mode instead pushes short bursts of descriptors into a queue - motion runs
 * with real dx/dy deltas, keystroke press/release pairs, clicks/scrolls - and
 * releases them one per drain, with jittered gaps between bursts. It never
 * fabricates activity from nothing: generation is still gated on
 * activity_is_fresh() (the compositor actually saw input within the TTL).
 *
 * Privacy is unchanged: we don't know real keycodes or coordinates, so the
 * stream is a plausible profile, not a replay. Emitted event classes are
 * restricted to those the monitor actually selected (activity_selected_raw).
 */
#define SD_SEL_MOTION        (1u << 0)
#define SD_SEL_KEYPRESS      (1u << 1)
#define SD_SEL_KEYRELEASE    (1u << 2)
#define SD_SEL_BUTTONPRESS   (1u << 3)
#define SD_SEL_BUTTONRELEASE (1u << 4)
static unsigned int activity_selected_raw = 0; /* SD_SEL_* the monitor subscribed to */

typedef struct {
    int evtype;         /* XI_RawMotion / XI_RawKeyPress / ... */
    int detail;         /* keycode or button number */
    int flags;          /* e.g. XIKeyRepeat */
    int have_valuators; /* motion carries dx/dy */
    double dx, dy;
} sd_synth_desc_t;

#define SD_SYNTH_Q 128
static sd_synth_desc_t activity_synth_queue[SD_SYNTH_Q];
static int activity_synth_q_head = 0; /* pop index */
static int activity_synth_q_tail = 0; /* push index */
/* Descriptors handed to XNextEvent, keyed by cookie low byte so the matching
 * XGetEventData rebuilds the right raw event. */
static sd_synth_desc_t activity_synth_deliver[256];
static uint32_t activity_rng_state = 0;
static uint64_t activity_next_gap_ms = 0;        /* jittered gap until next burst */
/* Real raw-event server time, sampled so synthetic events share that clock. */
static uint64_t activity_last_real_time = 0;
static uint64_t activity_last_real_time_mono = 0;

/* xorshift32; call under activity_synth_mutex. Self-seeds from the clock. */
static uint32_t activity_rng_next(void) {
    uint32_t x = activity_rng_state;
    if (x == 0) x = 0x9e3779b9u ^ (uint32_t)active_window_monotonic_ms();
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    activity_rng_state = x ? x : 0x9e3779b9u;
    return activity_rng_state;
}

/* Queue helpers; all call under activity_synth_mutex. */
static int activity_q_count_locked(void) {
    int n = activity_synth_q_tail - activity_synth_q_head;
    return n < 0 ? n + SD_SYNTH_Q : n;
}
static void activity_q_push_locked(const sd_synth_desc_t *d) {
    int next = (activity_synth_q_tail + 1) % SD_SYNTH_Q;
    if (next == activity_synth_q_head) return; /* full: drop rather than overrun */
    activity_synth_queue[activity_synth_q_tail] = *d;
    activity_synth_q_tail = next;
}
static int activity_q_pop_locked(sd_synth_desc_t *out) {
    if (activity_synth_q_head == activity_synth_q_tail) return 0;
    *out = activity_synth_queue[activity_synth_q_head];
    activity_synth_q_head = (activity_synth_q_head + 1) % SD_SYNTH_Q;
    return 1;
}

static void activity_push_motion_locked(void) {
    sd_synth_desc_t d;
    memset(&d, 0, sizeof(d));
    d.evtype = XI_RawMotion;
    d.have_valuators = 1;
    d.dx = (double)((int)(activity_rng_next() % 9) - 4); /* -4..+4 px */
    d.dy = (double)((int)(activity_rng_next() % 9) - 4);
    activity_q_push_locked(&d);
}

/*
 * Push one human-like burst of descriptors. Weighted mix, restricted to the
 * event classes the monitor selected. Call under activity_synth_mutex.
 */
static void activity_generate_burst_locked(void) {
    unsigned int sel = activity_selected_raw ? activity_selected_raw : SD_SEL_MOTION;
    uint32_t roll = activity_rng_next() % 100;

    if ((sel & SD_SEL_MOTION) && roll < 70) {
        int n = 3 + (int)(activity_rng_next() % 10); /* 3..12 motion events */
        for (int i = 0; i < n; i++) activity_push_motion_locked();
    } else if ((sel & SD_SEL_KEYPRESS) && roll < 92) {
        int keys = 1 + (int)(activity_rng_next() % 5); /* 1..5 keystrokes */
        for (int i = 0; i < keys; i++) {
            int kc = 24 + (int)(activity_rng_next() % 42); /* plausible alnum keycodes */
            sd_synth_desc_t p;
            memset(&p, 0, sizeof(p));
            p.evtype = XI_RawKeyPress;
            p.detail = kc;
            activity_q_push_locked(&p);
            if (sel & SD_SEL_KEYRELEASE) {
                sd_synth_desc_t r;
                memset(&r, 0, sizeof(r));
                r.evtype = XI_RawKeyRelease;
                r.detail = kc;
                activity_q_push_locked(&r);
            }
        }
    } else if (sel & SD_SEL_BUTTONPRESS) {
        int btn = (roll < 98) ? 1 : (4 + (int)(activity_rng_next() % 2)); /* click, sometimes wheel */
        sd_synth_desc_t p;
        memset(&p, 0, sizeof(p));
        p.evtype = XI_RawButtonPress;
        p.detail = btn;
        activity_q_push_locked(&p);
        if (sel & SD_SEL_BUTTONRELEASE) {
            sd_synth_desc_t r;
            memset(&r, 0, sizeof(r));
            r.evtype = XI_RawButtonRelease;
            r.detail = btn;
            activity_q_push_locked(&r);
        }
    } else if (sel & SD_SEL_MOTION) {
        activity_push_motion_locked(); /* selected set lacked the rolled class */
    }
}

/* Remember a real raw event's server Time so synthetic events extrapolate on
 * the same clock rather than raw monotonic ms. */
static void activity_note_server_time(Time t) {
    pthread_mutex_lock(&activity_synth_mutex);
    activity_last_real_time = (uint64_t)t;
    activity_last_real_time_mono = active_window_monotonic_ms();
    pthread_mutex_unlock(&activity_synth_mutex);
}
static uint64_t activity_synth_server_time(void) {
    pthread_mutex_lock(&activity_synth_mutex);
    uint64_t base = activity_last_real_time;
    uint64_t base_mono = activity_last_real_time_mono;
    pthread_mutex_unlock(&activity_synth_mutex);
    uint64_t now = active_window_monotonic_ms();
    if (base == 0) return now; /* no real sample yet */
    return base + (now - base_mono);
}


/* Remember an EPOLL_CTL_ADD/MOD of any fd so a later raw-motion subscription can
 * reconcile against it. Newest-wins: reuse an existing slot for the same fd,
 * else overwrite the oldest ring entry. */
static void activity_epoll_table_record(int epfd, int fd, uint64_t data) {
    pthread_mutex_lock(&activity_synth_mutex);
    int slot = -1;
    for (int i = 0; i < SD_EPOLL_TABLE_SIZE; i++) {
        if (activity_epoll_table[i].fd == fd) { slot = i; break; }
    }
    if (slot < 0) {
        slot = activity_epoll_table_head;
        activity_epoll_table_head = (activity_epoll_table_head + 1) % SD_EPOLL_TABLE_SIZE;
    }
    activity_epoll_table[slot].fd = fd;
    activity_epoll_table[slot].epfd = epfd;
    activity_epoll_table[slot].data = data;
    pthread_mutex_unlock(&activity_synth_mutex);
}

/* Drop a table entry when its fd is removed from an epoll set. */
static void activity_epoll_table_forget(int epfd, int fd) {
    pthread_mutex_lock(&activity_synth_mutex);
    for (int i = 0; i < SD_EPOLL_TABLE_SIZE; i++) {
        if (activity_epoll_table[i].fd == fd && activity_epoll_table[i].epfd == epfd) {
            activity_epoll_table[i].fd = -1;
            activity_epoll_table[i].epfd = -1;
            activity_epoll_table[i].data = 0;
        }
    }
    pthread_mutex_unlock(&activity_synth_mutex);
}

/* Called with activity_synth_mutex held: if the monitor fd was registered with
 * an epoll instance before we knew it was the monitor, adopt that registration
 * now so epoll_wait/epoll_pwait start injecting synthetic RawMotion. */
static void activity_epoll_reconcile_locked(int fd) {
    if (activity_monitor_epoll_data_valid) return;
    for (int i = 0; i < SD_EPOLL_TABLE_SIZE; i++) {
        if (activity_epoll_table[i].fd == fd) {
            activity_monitor_epfd = activity_epoll_table[i].epfd;
            activity_monitor_epoll_data = activity_epoll_table[i].data;
            activity_monitor_epoll_data_valid = 1;
            if (diag_xinput_enabled())
                diag_logmsg("diag:activity epoll reconciled from table epfd=%d fd=%d data=0x%llx",
                            activity_monitor_epfd, fd,
                            (unsigned long long)activity_monitor_epoll_data);
            return;
        }
    }
}

/* Record which Display/fd is the raw-input monitor so the poll hook only
 * ever touches that one connection. */
static void activity_note_raw_subscription(Display *display, XIEventMask *masks, int num_masks) {
    if (!activity_enabled() || !display || !masks) return;

    /* Record every raw class the monitor subscribed to so profile mode only
     * ever synthesizes event types Time Doctor's handler already expects. */
    unsigned int sel = 0;
    int has_motion = 0;
    for (int i = 0; i < num_masks; i++) {
        if (xi_mask_has(&masks[i], XI_RawMotion))        { sel |= SD_SEL_MOTION; has_motion = 1; }
        if (xi_mask_has(&masks[i], XI_RawKeyPress))      sel |= SD_SEL_KEYPRESS;
        if (xi_mask_has(&masks[i], XI_RawKeyRelease))    sel |= SD_SEL_KEYRELEASE;
        if (xi_mask_has(&masks[i], XI_RawButtonPress))   sel |= SD_SEL_BUTTONPRESS;
        if (xi_mask_has(&masks[i], XI_RawButtonRelease)) sel |= SD_SEL_BUTTONRELEASE;
    }
    if (sel) {
        pthread_mutex_lock(&activity_synth_mutex);
        activity_selected_raw |= sel;
        pthread_mutex_unlock(&activity_synth_mutex);
    }

    /* The monitor connection is the one that selected raw motion; that fd is
     * what the poll/drain hooks target. */
    if (!has_motion) return;
    int fd = ConnectionNumber(display);
    pthread_mutex_lock(&activity_synth_mutex);
    activity_monitor_display = display;
    activity_monitor_fd = fd;
    activity_epoll_reconcile_locked(fd);
    pthread_mutex_unlock(&activity_synth_mutex);
    if (diag_xinput_enabled()) {
        diag_logmsg("diag:activity monitor display=%p fd=%d noted (raw-motion subscription) selected=0x%x",
                    (void *)display, fd, sel);
    }
}

static int activity_monitor_fd_get(void) {
    /* Set once at subscription; a plain aligned read is safe here and keeps
     * the poll/ppoll fast path lock-free. */
    return activity_monitor_fd;
}

static int activity_display_is_monitor(Display *display) {
    pthread_mutex_lock(&activity_synth_mutex);
    int match = (activity_monitor_display == display);
    pthread_mutex_unlock(&activity_synth_mutex);
    return match;
}

/* Remember the device ids of real RawMotion events so synthetic ones blend
 * in. Called from XGetEventData for genuine raw motion. */
static void activity_note_pointer_device(int deviceid, int sourceid) {
    pthread_mutex_lock(&activity_synth_mutex);
    activity_pointer_deviceid = deviceid;
    activity_pointer_sourceid = sourceid;
    pthread_mutex_unlock(&activity_synth_mutex);
}

static int cookie_is_synthetic(const XGenericEventCookie *cookie) {
    return cookie && (cookie->cookie & SD_SYNTH_COOKIE_MASK) == SD_SYNTH_COOKIE_SENTINEL;
}

/* Build a synthetic RawMotion GenericEvent into event_return. Data is left
 * NULL; XGetEventData fills it on request (the standard cookie flow). */
static void activity_fill_synth_event(Display *display, int xi_opcode, XEvent *event_return) {
    memset(event_return, 0, sizeof(*event_return));
    XGenericEventCookie *ck = &event_return->xcookie;
    ck->type = GenericEvent;
    ck->send_event = False;
    ck->display = display;
    ck->extension = xi_opcode;
    ck->evtype = XI_RawMotion;
    ck->data = NULL;

    pthread_mutex_lock(&activity_synth_mutex);
    ck->cookie = SD_SYNTH_COOKIE_SENTINEL | (activity_synth_counter++ & 0x00FFFFFFu);
    pthread_mutex_unlock(&activity_synth_mutex);
}

/* Profile-mode fill: carry the descriptor's evtype in the cookie and stash the
 * descriptor keyed by the cookie low byte so XGetEventData rebuilds it. */
static void activity_fill_synth_event_desc(Display *display, int xi_opcode,
                                           const sd_synth_desc_t *d, XEvent *event_return) {
    memset(event_return, 0, sizeof(*event_return));
    XGenericEventCookie *ck = &event_return->xcookie;
    ck->type = GenericEvent;
    ck->send_event = False;
    ck->display = display;
    ck->extension = xi_opcode;
    ck->evtype = d->evtype;
    ck->data = NULL;

    pthread_mutex_lock(&activity_synth_mutex);
    unsigned int c = activity_synth_counter++;
    ck->cookie = SD_SYNTH_COOKIE_SENTINEL | (c & 0x00FFFFFFu);
    activity_synth_deliver[c & 0xFF] = *d;
    pthread_mutex_unlock(&activity_synth_mutex);
}

/*
 * Profile-mode wakeup gate. Returns 1 while a burst is draining or when a new
 * jittered gap has elapsed (generating the next burst). Bursts drain one event
 * per call so the queue empties across successive drains/polls.
 */
static int activity_profile_should_emit(void) {
    pthread_mutex_lock(&activity_synth_mutex);
    int have = activity_q_count_locked() > 0;
    pthread_mutex_unlock(&activity_synth_mutex);
    if (have) return 1; /* keep signaling readable until the burst is drained */

    if (!activity_is_fresh()) return 0;

    uint64_t now = active_window_monotonic_ms();
    int fire = 0;
    pthread_mutex_lock(&activity_cache_mutex);
    if (activity_last_synth_ms == 0 || now < activity_last_synth_ms ||
        now - activity_last_synth_ms >= activity_next_gap_ms) {
        activity_last_synth_ms = now;
        fire = 1;
    }
    pthread_mutex_unlock(&activity_cache_mutex);
    if (!fire) return 0;

    pthread_mutex_lock(&activity_synth_mutex);
    activity_generate_burst_locked();
    /* Next gap: 0.5x..2x the rate, with an occasional longer idle pause so the
     * cadence never reads as a fixed pulse. */
    uint32_t base = activity_rate_ms ? activity_rate_ms : 1000;
    uint32_t gap = base / 2 + (activity_rng_next() % (base * 3 / 2 + 1));
    if ((activity_rng_next() % 5) == 0) gap += base * (2 + (activity_rng_next() % 3));
    int have_now = activity_q_count_locked() > 0;
    pthread_mutex_unlock(&activity_synth_mutex);

    pthread_mutex_lock(&activity_cache_mutex);
    activity_next_gap_ms = gap;
    pthread_mutex_unlock(&activity_cache_mutex);

    return have_now;
}

/* Decide whether to inject a synthetic wakeup now: user active and the rate
 * gate has elapsed. On success raises activity_synth_pending and returns 1. */
static int activity_should_fake_wakeup(void) {
    if (activity_profile_enabled()) return activity_profile_should_emit();

    if (!activity_is_fresh()) return 0;
    uint64_t now = active_window_monotonic_ms();
    int emit = 0;
    pthread_mutex_lock(&activity_cache_mutex);
    if (activity_last_synth_ms == 0 || now < activity_last_synth_ms ||
        now - activity_last_synth_ms >= activity_rate_ms) {
        activity_last_synth_ms = now;
        emit = 1;
    }
    pthread_mutex_unlock(&activity_cache_mutex);
    if (!emit) return 0;

    pthread_mutex_lock(&activity_synth_mutex);
    activity_synth_pending = 1;
    pthread_mutex_unlock(&activity_synth_mutex);
    return 1;
}

static int activity_synth_pending_get(void) {
    if (activity_profile_enabled()) {
        pthread_mutex_lock(&activity_synth_mutex);
        int p = activity_q_count_locked() > 0;
        pthread_mutex_unlock(&activity_synth_mutex);
        return p;
    }
    pthread_mutex_lock(&activity_synth_mutex);
    int p = activity_synth_pending;
    pthread_mutex_unlock(&activity_synth_mutex);
    return p;
}

/*
 * If a synthetic RawMotion is pending for the monitor display and no real
 * event is already queued, fill event_return with it and clear the flag.
 * Returns 1 if a synthetic event was produced.
 */
static int activity_take_synth_event(Display *display, XEvent *event_return) {
    if (!activity_enabled() || !event_return || !activity_display_is_monitor(display)) return 0;
    if (!activity_synth_pending_get()) return 0;

    if (!real_XEventsQueued_fn)
        real_XEventsQueued_fn = (real_XEventsQueued_t)resolve_next_symbol("XEventsQueued", &warned_XEventsQueued);
    if (real_XEventsQueued_fn && real_XEventsQueued_fn(display, QueuedAlready) > 0)
        return 0; /* real event already buffered - serve that instead */

    int xi_opcode = 0;
    if (!get_xinput_opcode(display, &xi_opcode)) return 0;

    if (activity_profile_enabled()) {
        sd_synth_desc_t d;
        pthread_mutex_lock(&activity_synth_mutex);
        int got = activity_q_pop_locked(&d);
        pthread_mutex_unlock(&activity_synth_mutex);
        if (!got) return 0;
        activity_fill_synth_event_desc(display, xi_opcode, &d, event_return);
        if (diag_xinput_enabled()) {
            diag_logmsg("diag:activity synth evtype=%s(%d) detail=%d device=%d cookie=0x%x",
                        xi_event_name(d.evtype), d.evtype, d.detail,
                        activity_pointer_deviceid, event_return->xcookie.cookie);
        }
        return 1;
    }

    pthread_mutex_lock(&activity_synth_mutex);
    activity_synth_pending = 0;
    pthread_mutex_unlock(&activity_synth_mutex);

    activity_fill_synth_event(display, xi_opcode, event_return);
    if (diag_xinput_enabled()) {
        diag_logmsg("diag:activity synth evtype=RawMotion device=%d cookie=0x%x",
                    activity_pointer_deviceid, event_return->xcookie.cookie);
    }
    return 1;
}

/*
 * Shared poll/ppoll interception. Scoped strictly to the monitor connection
 * fd: if it is not in the set, the caller must have already passed through to
 * the real syscall. Here we cap the wait and, on a quiet monitor fd with the
 * user active, mark it readable so the event loop drains a synthetic event.
 * Returns the (possibly adjusted) syscall return value.
 */
static uint64_t activity_poll_diag_ms = 0;

static int activity_poll_adjust(struct pollfd *fds, nfds_t nfds, int mfd, int real_ret) {
    if (real_ret < 0) return real_ret;

    /* Find the monitor fd slot. */
    int idx = -1;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd == mfd) { idx = (int)i; break; }
    }
    if (idx < 0) return real_ret;

    int real_ready = (fds[idx].revents & POLLIN) ? 1 : 0;
    int fresh = real_ready ? 0 : activity_is_fresh();
    int faked = 0;

    /* Real data already waiting on the monitor fd: never interfere. */
    if (!real_ready && fresh && activity_should_fake_wakeup()) {
        fds[idx].revents |= POLLIN;
        faked = 1;
    }

    if (diag_xinput_enabled()) {
        uint64_t now = active_window_monotonic_ms();
        if (now - activity_poll_diag_ms >= 1000) {
            activity_poll_diag_ms = now;
            diag_logmsg("diag:activity poll fd=%d real_ready=%d fresh=%d faked=%d",
                        mfd, real_ready, fresh, faked);
        }
    }

    if (faked) return real_ret <= 0 ? 1 : real_ret + 1;
    return real_ret;
}

/* True when the monitor fd is present in this poll set (POLLIN requested). */
static int activity_pollset_has_monitor(const struct pollfd *fds, nfds_t nfds, int mfd) {
    if (mfd < 0 || !fds) return 0;
    for (nfds_t i = 0; i < nfds; i++) {
        if (fds[i].fd == mfd && (fds[i].events & POLLIN)) return 1;
    }
    return 0;
}

static uint64_t activity_epoll_diag_ms = 0;

/*
 * epoll counterpart of activity_poll_adjust. After a real epoll_wait/pwait on
 * the monitor's epoll instance, optionally append a synthetic EPOLLIN event
 * for the monitor fd (identified by its registered data) so Time Doctor's IO
 * thread drains a fabricated RawMotion. Returns the adjusted event count.
 */
static int activity_epoll_adjust(struct epoll_event *events, int maxevents, int real_ret) {
    if (real_ret < 0 || !events) return real_ret;
    if (!activity_monitor_epoll_data_valid) return real_ret;

    uint64_t data = activity_monitor_epoll_data;

    /* Monitor fd already reported ready this round: never interfere. */
    int already = 0;
    for (int i = 0; i < real_ret; i++) {
        if (events[i].data.u64 == data) { already = 1; break; }
    }

    int fresh = already ? 0 : activity_is_fresh();
    int faked = 0;
    if (!already && fresh && real_ret < maxevents && activity_should_fake_wakeup()) {
        events[real_ret].events = EPOLLIN;
        events[real_ret].data.u64 = data;
        faked = 1;
    }

    if (diag_xinput_enabled()) {
        uint64_t now = active_window_monotonic_ms();
        if (now - activity_epoll_diag_ms >= 1000) {
            activity_epoll_diag_ms = now;
            diag_logmsg("diag:activity epoll already_ready=%d fresh=%d faked=%d",
                        already, fresh, faked);
        }
    }

    return faked ? real_ret + 1 : real_ret;
}

static int request_allows_type(const property_request_t *request, xcb_atom_t expected_type) {
    if (!request) return 0;
    return request->type == XCB_ATOM_NONE || request->type == expected_type;
}

static uint16_t synthetic_reply_sequence(const property_request_t *request,
                                         xcb_get_property_reply_t *original) {
    if (original) return original->sequence;
    return request ? (uint16_t)(request->sequence & 0xffffu) : 0;
}

static xcb_get_property_reply_t *build_synthetic_property_reply(uint8_t format,
                                                                xcb_atom_t type,
                                                                uint32_t value_len,
                                                                const void *value,
                                                                size_t value_bytes,
                                                                uint16_t sequence) {
    size_t padded_bytes = (value_bytes + 3u) & ~(size_t)3u;
    if (padded_bytes / 4u > UINT32_MAX) return NULL;

    xcb_get_property_reply_t *reply = calloc(1, sizeof(*reply) + padded_bytes);
    if (!reply) return NULL;

    reply->response_type = 1;
    reply->format = format;
    reply->sequence = sequence;
    reply->length = (uint32_t)(padded_bytes / 4u);
    reply->type = type;
    reply->bytes_after = 0;
    reply->value_len = value_len;
    if (value && value_bytes > 0) memcpy(xcb_get_property_value(reply), value, value_bytes);
    return reply;
}

static xcb_get_property_reply_t *build_synthetic_u32_reply(const property_request_t *request,
                                                           xcb_get_property_reply_t *original,
                                                           xcb_atom_t type,
                                                           uint32_t value) {
    return build_synthetic_property_reply(32, type, 1, &value, sizeof(value),
                                          synthetic_reply_sequence(request, original));
}

static xcb_get_property_reply_t *build_synthetic_bytes_reply(const property_request_t *request,
                                                             xcb_get_property_reply_t *original,
                                                             xcb_atom_t type,
                                                             const void *value,
                                                             size_t value_bytes) {
    if (value_bytes > UINT32_MAX) return NULL;
    return build_synthetic_property_reply(8, type, (uint32_t)value_bytes, value,
                                          value_bytes, synthetic_reply_sequence(request, original));
}

static xcb_get_property_reply_t *replace_property_reply(xcb_get_property_reply_t *original,
                                                        xcb_generic_error_t **error,
                                                        xcb_get_property_reply_t *synthetic) {
    if (!synthetic) return original;
    if (original) free(original);
    if (error && *error) {
        free(*error);
        *error = NULL;
    }
    return synthetic;
}

static void active_window_desktop_basename(char *dst, size_t dst_size, const char *desktop_file) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!desktop_file || desktop_file[0] == '\0') return;

    const char *base = strrchr(desktop_file, '/');
    base = base ? base + 1 : desktop_file;
    copy_len_string(dst, dst_size, base, strlen(base));

    size_t len = strlen(dst);
    const char suffix[] = ".desktop";
    size_t suffix_len = sizeof(suffix) - 1;
    if (len > suffix_len && strcmp(dst + len - suffix_len, suffix) == 0) {
        dst[len - suffix_len] = '\0';
    }
}

static void active_window_lowercase_simple(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!src) return;

    size_t out = 0;
    for (size_t i = 0; src[i] && out + 1 < dst_size; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            dst[out++] = (char)tolower(ch);
        }
    }
    dst[out] = '\0';
}

static void active_window_resource_class(const sd_active_window_state_t *state,
                                         char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (state->resource_class[0] != '\0') {
        snprintf(out, out_size, "%s", state->resource_class);
    } else {
        active_window_desktop_basename(out, out_size, state->desktop_file);
        if (out[0] == '\0' && state->resource_name[0] != '\0') {
            snprintf(out, out_size, "%s", state->resource_name);
        }
    }
    if (out[0] == '\0') snprintf(out, out_size, "unknown");
}

static void active_window_resource_name(const sd_active_window_state_t *state,
                                        const char *resource_class,
                                        char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (state->resource_name[0] != '\0') {
        snprintf(out, out_size, "%s", state->resource_name);
        return;
    }

    active_window_lowercase_simple(out, out_size, resource_class);
    if (out[0] == '\0') snprintf(out, out_size, "%s", resource_class && resource_class[0] ? resource_class : "unknown");
}

static xcb_atom_t active_window_utf8_type(xcb_connection_t *connection,
                                          const property_request_t *request,
                                          xcb_get_property_reply_t *original,
                                          int *known_utf8) {
    xcb_atom_t utf8 = XCB_ATOM_NONE;
    if (known_utf8) *known_utf8 = lookup_atom_by_name(connection, "UTF8_STRING", &utf8);
    else lookup_atom_by_name(connection, "UTF8_STRING", &utf8);

    if (utf8 != XCB_ATOM_NONE) return utf8;
    if (request && request->type != XCB_ATOM_NONE) return request->type;
    if (original && original->type != XCB_ATOM_NONE) return original->type;
    return XCB_ATOM_STRING;
}

static xcb_get_property_reply_t *build_client_list_reply(const property_request_t *request,
                                                         xcb_get_property_reply_t *original,
                                                         int *already_present,
                                                         size_t *count_out) {
    if (already_present) *already_present = 0;
    if (count_out) *count_out = 1;

    size_t original_count = 0;
    const void *original_value = NULL;
    int original_valid = 0;
    if (original && original->format == 32) {
        int value_len = xcb_get_property_value_length(original);
        if (value_len >= 0 && value_len % (int)sizeof(uint32_t) == 0) {
            original_value = xcb_get_property_value(original);
            original_count = (size_t)value_len / sizeof(uint32_t);
            original_valid = original_value != NULL || original_count == 0;
        }
    }

    if (original_valid && original_value) {
        const unsigned char *bytes = (const unsigned char *)original_value;
        for (size_t i = 0; i < original_count; i++) {
            uint32_t window = 0;
            memcpy(&window, bytes + i * sizeof(window), sizeof(window));
            if (window == SD_SYNTHETIC_ACTIVE_WINDOW) {
                if (already_present) *already_present = 1;
                if (count_out) *count_out = original_count;
                return NULL;
            }
        }
    } else {
        original_count = 0;
    }

    if (original_count > (SIZE_MAX / sizeof(uint32_t)) - 1) return NULL;
    size_t count = original_count + 1;
    size_t bytes_len = count * sizeof(uint32_t);
    if (bytes_len / 4u > UINT32_MAX) return NULL;

    xcb_get_property_reply_t *reply = build_synthetic_property_reply(
        32, XCB_ATOM_WINDOW, (uint32_t)count, NULL, bytes_len,
        synthetic_reply_sequence(request, original));
    if (!reply) return NULL;

    unsigned char *value = (unsigned char *)xcb_get_property_value(reply);
    if (original_count > 0 && original_value) {
        memcpy(value, original_value, original_count * sizeof(uint32_t));
    }
    uint32_t synthetic = SD_SYNTHETIC_ACTIVE_WINDOW;
    memcpy(value + original_count * sizeof(uint32_t), &synthetic, sizeof(synthetic));
    if (count_out) *count_out = count;
    return reply;
}

static xcb_get_property_reply_t *active_window_synthesize_property_reply(
    xcb_connection_t *connection, const property_request_t *request, int request_known,
    xcb_get_property_reply_t *reply, xcb_generic_error_t **error) {
    if (!active_window_enabled() || !request_known || !request) return reply;
    if (request->delete || request->long_offset != 0) return reply;
    if (!is_tracked_property_name(request->property_name)) return reply;

    sd_active_window_state_t state;
    if (!active_window_load_state(&state)) return reply;

    xcb_get_property_reply_t *synthetic = NULL;

    if (strcmp(request->property_name, "_NET_ACTIVE_WINDOW") == 0) {
        if (!is_root_window(connection, request->window) || !request_allows_type(request, XCB_ATOM_WINDOW)) return reply;
        synthetic = build_synthetic_u32_reply(request, reply, XCB_ATOM_WINDOW, SD_SYNTHETIC_ACTIVE_WINDOW);
        if (synthetic && active_window_diag_enabled()) {
            diag_logmsg("active-window synth property=_NET_ACTIVE_WINDOW value_window=0x%08x",
                        SD_SYNTHETIC_ACTIVE_WINDOW);
        }
        return replace_property_reply(reply, error, synthetic);
    }

    if ((strcmp(request->property_name, "_NET_CLIENT_LIST") == 0 ||
         strcmp(request->property_name, "_NET_CLIENT_LIST_STACKING") == 0)) {
        if (!is_root_window(connection, request->window) || !request_allows_type(request, XCB_ATOM_WINDOW)) return reply;
        int already_present = 0;
        size_t count = 0;
        synthetic = build_client_list_reply(request, reply, &already_present, &count);
        if (already_present) return reply;
        if (synthetic && active_window_diag_enabled()) {
            diag_logmsg("active-window synth property=%s count=%zu value_window=0x%08x",
                        request->property_name, count, SD_SYNTHETIC_ACTIVE_WINDOW);
        }
        return replace_property_reply(reply, error, synthetic);
    }

    if (request->window != SD_SYNTHETIC_ACTIVE_WINDOW) return reply;

    if (strcmp(request->property_name, "_NET_WM_PID") == 0) {
        if (!request_allows_type(request, XCB_ATOM_CARDINAL)) return reply;
        synthetic = build_synthetic_u32_reply(request, reply, XCB_ATOM_CARDINAL, state.pid);
        if (synthetic && active_window_diag_enabled()) {
            diag_logmsg("active-window synth property=_NET_WM_PID pid=%u", state.pid);
        }
        return replace_property_reply(reply, error, synthetic);
    }

    if (strcmp(request->property_name, "_NET_WM_NAME") == 0) {
        int known_utf8 = 0;
        xcb_atom_t type = active_window_utf8_type(connection, request, reply, &known_utf8);
        if (known_utf8 && !request_allows_type(request, type)) return reply;
        synthetic = build_synthetic_bytes_reply(request, reply, type, state.title, strlen(state.title));
        if (synthetic && active_window_diag_enabled()) {
            char escaped[DIAG_ESCAPE_MAX];
            escape_bytes(state.title, strlen(state.title), escaped, sizeof(escaped));
            diag_logmsg("active-window synth property=_NET_WM_NAME text=\"%s\"", escaped);
        }
        return replace_property_reply(reply, error, synthetic);
    }

    if (strcmp(request->property_name, "WM_NAME") == 0) {
        if (!request_allows_type(request, XCB_ATOM_STRING)) return reply;
        synthetic = build_synthetic_bytes_reply(request, reply, XCB_ATOM_STRING, state.title, strlen(state.title));
        if (synthetic && active_window_diag_enabled()) {
            char escaped[DIAG_ESCAPE_MAX];
            escape_bytes(state.title, strlen(state.title), escaped, sizeof(escaped));
            diag_logmsg("active-window synth property=WM_NAME text=\"%s\"", escaped);
        }
        return replace_property_reply(reply, error, synthetic);
    }

    if (strcmp(request->property_name, "WM_CLASS") == 0) {
        if (!request_allows_type(request, XCB_ATOM_STRING)) return reply;

        char class_name[SD_ACTIVE_WINDOW_CLASS_MAX];
        char resource_name[SD_ACTIVE_WINDOW_CLASS_MAX];
        active_window_resource_class(&state, class_name, sizeof(class_name));
        active_window_resource_name(&state, class_name, resource_name, sizeof(resource_name));

        unsigned char value[SD_ACTIVE_WINDOW_CLASS_MAX * 2 + 2];
        size_t resource_len = strlen(resource_name);
        size_t class_len = strlen(class_name);
        size_t value_len = resource_len + 1u + class_len + 1u;
        if (value_len > sizeof(value)) return reply;
        memcpy(value, resource_name, resource_len);
        value[resource_len] = '\0';
        memcpy(value + resource_len + 1u, class_name, class_len);
        value[value_len - 1u] = '\0';

        synthetic = build_synthetic_bytes_reply(request, reply, XCB_ATOM_STRING, value, value_len);
        if (synthetic && active_window_diag_enabled()) {
            char escaped[DIAG_ESCAPE_MAX];
            escape_bytes(value, value_len, escaped, sizeof(escaped));
            diag_logmsg("active-window synth property=WM_CLASS text=\"%s\"", escaped);
        }
        return replace_property_reply(reply, error, synthetic);
    }

    return reply;
}

int XIQueryVersion(Display *display, int *major_version_inout, int *minor_version_inout) {
    if (!real_XIQueryVersion_fn)
        real_XIQueryVersion_fn = (real_XIQueryVersion_t)resolve_next_symbol("XIQueryVersion", &warned_XIQueryVersion);
    if (!real_XIQueryVersion_fn) return -1;

    int requested_major = major_version_inout ? *major_version_inout : -1;
    int requested_minor = minor_version_inout ? *minor_version_inout : -1;
    int status = real_XIQueryVersion_fn(display, major_version_inout, minor_version_inout);

    if (diag_xinput_enabled()) {
        int returned_major = major_version_inout ? *major_version_inout : -1;
        int returned_minor = minor_version_inout ? *minor_version_inout : -1;
        diag_logmsg("diag:xinput XIQueryVersion requested=%d.%d returned=%d.%d status=%d",
                    requested_major, requested_minor, returned_major, returned_minor, status);
    }
    return status;
}

int XISelectEvents(Display *display, Window win, XIEventMask *masks, int num_masks) {
    if (!real_XISelectEvents_fn)
        real_XISelectEvents_fn = (real_XISelectEvents_t)resolve_next_symbol("XISelectEvents", &warned_XISelectEvents);
    if (!real_XISelectEvents_fn) return -1;

    int status = real_XISelectEvents_fn(display, win, masks, num_masks);

    activity_note_raw_subscription(display, masks, num_masks);

    if (diag_xinput_enabled()) {
        if (!masks || num_masks <= 0) {
            diag_logmsg("diag:xinput XISelectEvents window=0x%lx num_masks=%d status=%d masks=none",
                        (unsigned long)win, num_masks, status);
        } else {
            for (int i = 0; i < num_masks; i++) {
                char mask_desc[256];
                decode_xi_mask(&masks[i], mask_desc, sizeof(mask_desc));
                diag_logmsg("diag:xinput XISelectEvents window=0x%lx mask_index=%d num_masks=%d device=%d mask_len=%d masks=%s status=%d",
                            (unsigned long)win, i, num_masks, masks[i].deviceid, masks[i].mask_len,
                            mask_desc, status);
            }
        }
    }
    return status;
}

int XSelectInput(Display *display, Window w, long event_mask) {
    if (!real_XSelectInput_fn)
        real_XSelectInput_fn = (real_XSelectInput_t)resolve_next_symbol("XSelectInput", &warned_XSelectInput);
    if (!real_XSelectInput_fn) return 0;

    int status = real_XSelectInput_fn(display, w, event_mask);

    if (diag_xinput_enabled()) {
        char mask_desc[256];
        decode_core_event_mask(event_mask, mask_desc, sizeof(mask_desc));
        diag_logmsg("diag:xinput XSelectInput window=0x%lx mask=0x%lx events=%s status=%d",
                    (unsigned long)w, (unsigned long)event_mask, mask_desc, status);
    }
    return status;
}

int XNextEvent(Display *display, XEvent *event_return) {
    if (!real_XNextEvent_fn)
        real_XNextEvent_fn = (real_XNextEvent_t)resolve_next_symbol("XNextEvent", &warned_XNextEvent);
    if (!real_XNextEvent_fn) return -1;

    if (event_return && activity_take_synth_event(display, event_return)) {
        /* Synthetic RawMotion delivered; mirror the diag XNextEvent line so
         * the trace shows the same shape as a real event. */
        if (diag_xinput_enabled()) {
            unsigned int suppressed = 0;
            if (diag_rate_should_log(xnext_rate_state(GenericEvent), 20, 1, &suppressed)) {
                char suppressed_buf[64] = "";
                if (suppressed > 0) snprintf(suppressed_buf, sizeof(suppressed_buf), " suppressed=%u", suppressed);
                diag_logmsg("diag:xinput XNextEvent type=GenericEvent(35) extension=%d evtype=%s(%d) synthetic=1%s",
                            event_return->xcookie.extension,
                            xi_event_name(event_return->xcookie.evtype),
                            event_return->xcookie.evtype, suppressed_buf);
            }
        }
        return 0;
    }

    int status = real_XNextEvent_fn(display, event_return);

    if (status == 0 && event_return && diag_xinput_enabled()) {
        unsigned int suppressed = 0;
        int type = event_return->type;
        if (diag_rate_should_log(xnext_rate_state(type), 20, 1, &suppressed)) {
            char suppressed_buf[64] = "";
            if (suppressed > 0) snprintf(suppressed_buf, sizeof(suppressed_buf), " suppressed=%u", suppressed);
            if (type == GenericEvent) {
                diag_logmsg("diag:xinput XNextEvent type=%s(%d) extension=%d evtype=%s(%d)%s",
                            core_event_name(type), type, event_return->xcookie.extension,
                            xi_event_name(event_return->xcookie.evtype), event_return->xcookie.evtype,
                            suppressed_buf);
            } else {
                diag_logmsg("diag:xinput XNextEvent type=%s(%d)%s",
                            core_event_name(type), type, suppressed_buf);
            }
        }
    }
    return status;
}

Bool XGetEventData(Display *display, XGenericEventCookie *cookie) {
    if (!real_XGetEventData_fn)
        real_XGetEventData_fn = (real_XGetEventData_t)resolve_next_symbol("XGetEventData", &warned_XGetEventData);
    if (!real_XGetEventData_fn) return False;

    /* Service our fabricated cookie without touching real Xlib. */
    if (activity_enabled() && cookie_is_synthetic(cookie)) {
        XIRawEvent *raw = (XIRawEvent *)calloc(1, sizeof(XIRawEvent));
        if (!raw) return False;

        /* Profile mode: rebuild the exact descriptor this cookie carried. */
        sd_synth_desc_t d;
        memset(&d, 0, sizeof(d));
        int have_desc = 0;
        if (activity_profile_enabled()) {
            pthread_mutex_lock(&activity_synth_mutex);
            d = activity_synth_deliver[cookie->cookie & 0xFF];
            pthread_mutex_unlock(&activity_synth_mutex);
            have_desc = 1;
        }

        pthread_mutex_lock(&activity_synth_mutex);
        int deviceid = activity_pointer_deviceid;
        int sourceid = activity_pointer_sourceid;
        pthread_mutex_unlock(&activity_synth_mutex);
        raw->type = GenericEvent;
        raw->extension = cookie->extension;
        raw->evtype = have_desc ? d.evtype : XI_RawMotion;
        raw->send_event = False;
        raw->display = display;
        raw->time = (Time)activity_synth_server_time();
        raw->deviceid = deviceid;
        raw->sourceid = sourceid;
        raw->detail = have_desc ? d.detail : 0;
        raw->flags = have_desc ? d.flags : 0;
        raw->valuators.mask_len = 0;
        raw->valuators.mask = NULL;
        raw->valuators.values = NULL;
        raw->raw_values = NULL;

        /* Real motion carries axis 0/1 deltas in both accelerated (values) and
         * unaccelerated (raw_values) form; give synthetic motion the same shape
         * instead of an empty valuator set. Freed in XFreeEventData. */
        if (have_desc && d.have_valuators && d.evtype == XI_RawMotion) {
            const int nax = 2;
            int mlen = (nax + 7) / 8;
            unsigned char *mask = (unsigned char *)calloc(1, (size_t)mlen);
            double *vals = (double *)calloc((size_t)nax, sizeof(double));
            double *rawv = (double *)calloc((size_t)nax, sizeof(double));
            if (mask && vals && rawv) {
                XISetMask(mask, 0);
                XISetMask(mask, 1);
                vals[0] = rawv[0] = d.dx;
                vals[1] = rawv[1] = d.dy;
                raw->valuators.mask_len = mlen;
                raw->valuators.mask = mask;
                raw->valuators.values = vals;
                raw->raw_values = rawv;
            } else {
                free(mask);
                free(vals);
                free(rawv);
            }
        }
        cookie->data = raw;
        return True;
    }

    Bool result = real_XGetEventData_fn(display, cookie);

    /* Learn the real pointer device ids and server-time base so synthetic
     * events match genuine ones. */
    if (result && cookie && cookie->data && activity_enabled()) {
        int opcode = 0;
        if (get_xinput_opcode(display, &opcode) && cookie->extension == opcode &&
            cookie->evtype == XI_RawMotion) {
            XIRawEvent *raw = (XIRawEvent *)cookie->data;
            activity_note_pointer_device(raw->deviceid, raw->sourceid);
            activity_note_server_time(raw->time);
        }
    }

    if (result && cookie && diag_xinput_enabled()) {
        int opcode = 0;
        if (get_xinput_opcode(display, &opcode) && cookie->extension == opcode) {
            unsigned int suppressed = 0;
            int evtype = cookie->evtype;
            if (diag_rate_should_log(xget_rate_state(evtype), 20, 1, &suppressed)) {
                char suppressed_buf[64] = "";
                if (suppressed > 0) snprintf(suppressed_buf, sizeof(suppressed_buf), " suppressed=%u", suppressed);

                if (cookie->data && (evtype == XI_RawKeyPress || evtype == XI_RawKeyRelease ||
                                     evtype == XI_RawButtonPress || evtype == XI_RawButtonRelease ||
                                     evtype == XI_RawMotion)) {
                    XIRawEvent *raw = (XIRawEvent *)cookie->data;
                    diag_logmsg("diag:xinput XGetEventData evtype=%s(%d) device=%d source=%d detail=%d time=%lu valuator_mask_len=%d%s",
                                xi_event_name(evtype), evtype, raw->deviceid, raw->sourceid,
                                raw->detail, (unsigned long)raw->time, raw->valuators.mask_len,
                                suppressed_buf);
                } else {
                    diag_logmsg("diag:xinput XGetEventData evtype=%s(%d) extension=%d data=%p%s",
                                xi_event_name(evtype), evtype, cookie->extension, cookie->data,
                                suppressed_buf);
                }
            }
        }
    }
    return result;
}

void XFreeEventData(Display *display, XGenericEventCookie *cookie) {
    /* Free our fabricated cookie's data; real Xlib never saw it. */
    if (activity_enabled() && cookie_is_synthetic(cookie)) {
        if (cookie->data) {
            XIRawEvent *raw = (XIRawEvent *)cookie->data;
            free(raw->valuators.mask);
            free(raw->valuators.values);
            free(raw->raw_values);
            free(raw);
        }
        cookie->data = NULL;
        return;
    }

    if (!real_XFreeEventData_fn)
        real_XFreeEventData_fn = (real_XFreeEventData_t)resolve_next_symbol("XFreeEventData", &warned_XFreeEventData);
    if (!real_XFreeEventData_fn) return;

    real_XFreeEventData_fn(display, cookie);
}

/*
 * Report a pending synthetic event to the monitor connection's drain loop.
 *
 * Time Doctor does not block on the monitor fd via poll/ppoll/epoll - it drains
 * the raw-input connection on a timer, calling XPending/XEventsQueued and then
 * XNextEvent. So the poll/epoll layer never fires for this fd and never arms a
 * synthetic event. We therefore arm here too: when the real queue is empty on
 * the monitor display and the user is active, run the rate-gated wakeup so the
 * caller's very next XNextEvent hands back a synthetic RawMotion.
 */
static int activity_report_synth_for_drain(Display *display) {
    if (!activity_enabled() || !activity_display_is_monitor(display)) return 0;
    if (activity_synth_pending_get()) return 1;
    return activity_should_fake_wakeup(); /* rate-gated; sets pending on success */
}

int XPending(Display *display) {
    if (!real_XPending_fn)
        real_XPending_fn = (real_XPending_t)resolve_next_symbol("XPending", &warned_XPending);
    if (!real_XPending_fn) return 0;

    int n = real_XPending_fn(display);
    if (n > 0) return n;
    if (activity_report_synth_for_drain(display)) return 1;
    return n;
}

int XEventsQueued(Display *display, int mode) {
    if (!real_XEventsQueued_fn)
        real_XEventsQueued_fn = (real_XEventsQueued_t)resolve_next_symbol("XEventsQueued", &warned_XEventsQueued);
    if (!real_XEventsQueued_fn) return 0;

    int n = real_XEventsQueued_fn(display, mode);
    if (n > 0) return n;
    if (activity_report_synth_for_drain(display)) return 1;
    return n;
}

/*
 * poll/ppoll are intercepted only to deliver synthetic input-activity events
 * to Time Doctor's main event loop, and only for the single file descriptor
 * of the raw-input monitor connection. Every other call - the overwhelming
 * majority in this process - is passed straight through unchanged.
 */
int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    if (!real_poll_fn)
        real_poll_fn = (real_poll_t)resolve_next_symbol("poll", &warned_poll);
    if (!real_poll_fn) { errno = ENOSYS; return -1; }

    if (!activity_enabled()) return real_poll_fn(fds, nfds, timeout);
    int mfd = activity_monitor_fd_get();
    if (!activity_pollset_has_monitor(fds, nfds, mfd))
        return real_poll_fn(fds, nfds, timeout);

    /* Cap the wait so we can inject a wakeup at the configured rate. */
    int capped = timeout;
    if (capped < 0 || capped > (int)activity_rate_ms) capped = (int)activity_rate_ms;
    int r = real_poll_fn(fds, nfds, capped);
    return activity_poll_adjust(fds, nfds, mfd, r);
}

int ppoll(struct pollfd *fds, nfds_t nfds, const struct timespec *tmo_p, const sigset_t *sigmask) {
    if (!real_ppoll_fn)
        real_ppoll_fn = (real_ppoll_t)resolve_next_symbol("ppoll", &warned_ppoll);
    if (!real_ppoll_fn) { errno = ENOSYS; return -1; }

    if (!activity_enabled()) return real_ppoll_fn(fds, nfds, tmo_p, sigmask);
    int mfd = activity_monitor_fd_get();
    if (!activity_pollset_has_monitor(fds, nfds, mfd))
        return real_ppoll_fn(fds, nfds, tmo_p, sigmask);

    /* Cap the wait to the rate interval. */
    struct timespec cap;
    cap.tv_sec = activity_rate_ms / 1000u;
    cap.tv_nsec = (long)(activity_rate_ms % 1000u) * 1000000L;
    const struct timespec *use = tmo_p;
    if (!tmo_p || tmo_p->tv_sec > cap.tv_sec ||
        (tmo_p->tv_sec == cap.tv_sec && tmo_p->tv_nsec > cap.tv_nsec)) {
        use = &cap;
    }
    int r = real_ppoll_fn(fds, nfds, use, sigmask);
    return activity_poll_adjust(fds, nfds, mfd, r);
}

/* Learn which epoll instance watches the monitor fd, and the data it uses. */
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) {
    if (!real_epoll_ctl_fn)
        real_epoll_ctl_fn = (real_epoll_ctl_t)resolve_next_symbol("epoll_ctl", &warned_epoll_ctl);
    if (!real_epoll_ctl_fn) { errno = ENOSYS; return -1; }

    int r = real_epoll_ctl_fn(epfd, op, fd, event);

    if (r == 0 && activity_enabled()) {
        if ((op == EPOLL_CTL_ADD || op == EPOLL_CTL_MOD) && event) {
            /* Record every registration so a later raw-motion subscription can
             * reconcile even if this fd was registered before we knew it was
             * the monitor (the common case in Time Doctor's Chromium loop). */
            activity_epoll_table_record(epfd, fd, event->data.u64);

            if (fd == activity_monitor_fd_get()) {
                pthread_mutex_lock(&activity_synth_mutex);
                activity_monitor_epfd = epfd;
                activity_monitor_epoll_data = event->data.u64;
                activity_monitor_epoll_data_valid = 1;
                pthread_mutex_unlock(&activity_synth_mutex);
                if (diag_xinput_enabled())
                    diag_logmsg("diag:activity epoll registered epfd=%d fd=%d data=0x%llx",
                                epfd, fd, (unsigned long long)event->data.u64);
            }
        } else if (op == EPOLL_CTL_DEL) {
            activity_epoll_table_forget(epfd, fd);
            if (fd == activity_monitor_fd_get()) {
                pthread_mutex_lock(&activity_synth_mutex);
                if (activity_monitor_epfd == epfd) {
                    activity_monitor_epfd = -1;
                    activity_monitor_epoll_data_valid = 0;
                }
                pthread_mutex_unlock(&activity_synth_mutex);
            }
        }
    }
    return r;
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    if (!real_epoll_wait_fn)
        real_epoll_wait_fn = (real_epoll_wait_t)resolve_next_symbol("epoll_wait", &warned_epoll_wait);
    if (!real_epoll_wait_fn) { errno = ENOSYS; return -1; }

    if (!activity_enabled() || epfd != activity_monitor_epfd || maxevents < 1)
        return real_epoll_wait_fn(epfd, events, maxevents, timeout);

    /* Reserve one slot for a possible synthetic event, and cap the wait. */
    int cap = timeout;
    if (cap < 0 || cap > (int)activity_rate_ms) cap = (int)activity_rate_ms;
    int r = real_epoll_wait_fn(epfd, events, maxevents, cap);
    return activity_epoll_adjust(events, maxevents, r);
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout,
                const sigset_t *sigmask) {
    if (!real_epoll_pwait_fn)
        real_epoll_pwait_fn = (real_epoll_pwait_t)resolve_next_symbol("epoll_pwait", &warned_epoll_pwait);
    if (!real_epoll_pwait_fn) { errno = ENOSYS; return -1; }

    if (!activity_enabled() || epfd != activity_monitor_epfd || maxevents < 1)
        return real_epoll_pwait_fn(epfd, events, maxevents, timeout, sigmask);

    int cap = timeout;
    if (cap < 0 || cap > (int)activity_rate_ms) cap = (int)activity_rate_ms;
    int r = real_epoll_pwait_fn(epfd, events, maxevents, cap, sigmask);
    return activity_epoll_adjust(events, maxevents, r);
}

Bool XQueryPointer(Display *display, Window w, Window *root_return, Window *child_return,
                   int *root_x_return, int *root_y_return, int *win_x_return,
                   int *win_y_return, unsigned int *mask_return) {
    if (!real_XQueryPointer_fn)
        real_XQueryPointer_fn = (real_XQueryPointer_t)resolve_next_symbol("XQueryPointer", &warned_XQueryPointer);
    if (!real_XQueryPointer_fn) return False;

    Bool result = real_XQueryPointer_fn(display, w, root_return, child_return,
                                        root_x_return, root_y_return, win_x_return,
                                        win_y_return, mask_return);

    if (diag_xinput_enabled()) {
        unsigned int suppressed = 0;
        if (diag_rate_should_log(&diag_xquery_pointer_rate, 10, 1, &suppressed)) {
            char suppressed_buf[64] = "";
            if (suppressed > 0) snprintf(suppressed_buf, sizeof(suppressed_buf), " suppressed=%u", suppressed);
            diag_logmsg("diag:xinput XQueryPointer window=0x%lx result=%d root=0x%lx child=0x%lx root_xy=%d,%d win_xy=%d,%d mask=0x%x%s",
                        (unsigned long)w, result ? 1 : 0,
                        (unsigned long)(root_return ? *root_return : 0),
                        (unsigned long)(child_return ? *child_return : 0),
                        root_x_return ? *root_x_return : 0,
                        root_y_return ? *root_y_return : 0,
                        win_x_return ? *win_x_return : 0,
                        win_y_return ? *win_y_return : 0,
                        mask_return ? *mask_return : 0,
                        suppressed_buf);
        }
    }
    return result;
}

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t *c, uint8_t only_if_exists,
                                         uint16_t name_len, const char *name) {
    if (!real_xcb_intern_atom_fn)
        real_xcb_intern_atom_fn = (real_xcb_intern_atom_t)resolve_next_symbol("xcb_intern_atom", &warned_xcb_intern_atom);
    if (!real_xcb_intern_atom_fn) {
        xcb_intern_atom_cookie_t empty = {0};
        return empty;
    }

    xcb_intern_atom_cookie_t cookie = real_xcb_intern_atom_fn(c, only_if_exists, name_len, name);

    int tracking = xcb_tracking_enabled();
    int diag = diag_xcb_enabled();
    if (tracking) {
        char name_buf[DIAG_NAME_MAX];
        copy_len_string(name_buf, sizeof(name_buf), name, name_len);
        store_intern_request(c, cookie.sequence, 0, name_buf);
    }
    if (diag) {
        char escaped[DIAG_ESCAPE_MAX];
        escape_bytes(name, name_len, escaped, sizeof(escaped));
        diag_logmsg("diag:xcb intern_atom sequence=%u only_if_exists=%u name=\"%s\"",
                    cookie.sequence, only_if_exists, escaped);
    }
    return cookie;
}

xcb_intern_atom_cookie_t xcb_intern_atom_unchecked(xcb_connection_t *c, uint8_t only_if_exists,
                                                   uint16_t name_len, const char *name) {
    if (!real_xcb_intern_atom_unchecked_fn)
        real_xcb_intern_atom_unchecked_fn = (real_xcb_intern_atom_t)resolve_next_symbol("xcb_intern_atom_unchecked", &warned_xcb_intern_atom_unchecked);
    if (!real_xcb_intern_atom_unchecked_fn) {
        xcb_intern_atom_cookie_t empty = {0};
        return empty;
    }

    xcb_intern_atom_cookie_t cookie = real_xcb_intern_atom_unchecked_fn(c, only_if_exists, name_len, name);

    int tracking = xcb_tracking_enabled();
    int diag = diag_xcb_enabled();
    if (tracking) {
        char name_buf[DIAG_NAME_MAX];
        copy_len_string(name_buf, sizeof(name_buf), name, name_len);
        store_intern_request(c, cookie.sequence, 1, name_buf);
    }
    if (diag) {
        char escaped[DIAG_ESCAPE_MAX];
        escape_bytes(name, name_len, escaped, sizeof(escaped));
        diag_logmsg("diag:xcb intern_atom_unchecked sequence=%u only_if_exists=%u name=\"%s\"",
                    cookie.sequence, only_if_exists, escaped);
    }
    return cookie;
}

xcb_intern_atom_reply_t *xcb_intern_atom_reply(xcb_connection_t *c, xcb_intern_atom_cookie_t cookie,
                                               xcb_generic_error_t **e) {
    if (!real_xcb_intern_atom_reply_fn)
        real_xcb_intern_atom_reply_fn = (real_xcb_intern_atom_reply_t)resolve_next_symbol("xcb_intern_atom_reply", &warned_xcb_intern_atom_reply);
    if (!real_xcb_intern_atom_reply_fn) return NULL;

    xcb_intern_atom_reply_t *reply = real_xcb_intern_atom_reply_fn(c, cookie, e);

    char name[DIAG_NAME_MAX];
    int unchecked = 0;
    int known = 0;
    snprintf(name, sizeof(name), "unknown");

    int tracking = xcb_tracking_enabled();
    int diag = diag_xcb_enabled();
    if (tracking) {
        known = lookup_intern_request(c, cookie.sequence, name, sizeof(name), &unchecked);
        if (reply && reply->atom != XCB_ATOM_NONE && known) {
            pthread_mutex_lock(&diag_xcb_mutex);
            store_atom_name_locked(c, reply->atom, name);
            pthread_mutex_unlock(&diag_xcb_mutex);
        }
    }

    if (diag) {
        if (reply) {
            diag_logmsg("diag:xcb atom_reply%s sequence=%u atom=0x%08x name=%s%s",
                        unchecked ? "_unchecked" : "", cookie.sequence, reply->atom, name,
                        known ? "" : " request=unknown");
        } else {
            unsigned int error_code = (e && *e) ? (*e)->error_code : 0;
            diag_logmsg("diag:xcb atom_reply%s sequence=%u reply=NULL error=%u name=%s%s",
                        unchecked ? "_unchecked" : "", cookie.sequence, error_code, name,
                        known ? "" : " request=unknown");
        }
    }
    return reply;
}

xcb_get_property_cookie_t xcb_get_property(xcb_connection_t *c, uint8_t delete,
                                           xcb_window_t window, xcb_atom_t property,
                                           xcb_atom_t type, uint32_t long_offset,
                                           uint32_t long_length) {
    if (!real_xcb_get_property_fn)
        real_xcb_get_property_fn = (real_xcb_get_property_t)resolve_next_symbol("xcb_get_property", &warned_xcb_get_property);
    if (!real_xcb_get_property_fn) {
        xcb_get_property_cookie_t empty = {0};
        return empty;
    }

    xcb_get_property_cookie_t cookie = real_xcb_get_property_fn(c, delete, window, property, type, long_offset, long_length);

    int tracking = xcb_tracking_enabled();
    int diag = diag_xcb_enabled();
    if (tracking) {
        char property_name[DIAG_NAME_MAX];
        char type_name[DIAG_NAME_MAX];
        store_property_request(c, cookie.sequence, 0, delete, window, property, type, long_offset, long_length,
                               property_name, sizeof(property_name), type_name, sizeof(type_name));
        if (diag) {
        diag_logmsg("diag:xcb get_property sequence=%u delete=%u window=0x%08x property=%s(0x%08x) type=%s(0x%08x) offset=%u length=%u tracked=%d",
                    cookie.sequence, delete, window, property_name, property, type_name, type,
                    long_offset, long_length, is_tracked_property_name(property_name));
        }
    }
    return cookie;
}

xcb_get_property_cookie_t xcb_get_property_unchecked(xcb_connection_t *c, uint8_t delete,
                                                     xcb_window_t window, xcb_atom_t property,
                                                     xcb_atom_t type, uint32_t long_offset,
                                                     uint32_t long_length) {
    if (!real_xcb_get_property_unchecked_fn)
        real_xcb_get_property_unchecked_fn = (real_xcb_get_property_t)resolve_next_symbol("xcb_get_property_unchecked", &warned_xcb_get_property_unchecked);
    if (!real_xcb_get_property_unchecked_fn) {
        xcb_get_property_cookie_t empty = {0};
        return empty;
    }

    xcb_get_property_cookie_t cookie = real_xcb_get_property_unchecked_fn(c, delete, window, property, type, long_offset, long_length);

    int tracking = xcb_tracking_enabled();
    int diag = diag_xcb_enabled();
    if (tracking) {
        char property_name[DIAG_NAME_MAX];
        char type_name[DIAG_NAME_MAX];
        store_property_request(c, cookie.sequence, 1, delete, window, property, type, long_offset, long_length,
                               property_name, sizeof(property_name), type_name, sizeof(type_name));
        if (diag) {
        diag_logmsg("diag:xcb get_property_unchecked sequence=%u delete=%u window=0x%08x property=%s(0x%08x) type=%s(0x%08x) offset=%u length=%u tracked=%d",
                    cookie.sequence, delete, window, property_name, property, type_name, type,
                    long_offset, long_length, is_tracked_property_name(property_name));
        }
    }
    return cookie;
}

xcb_get_property_reply_t *xcb_get_property_reply(xcb_connection_t *c, xcb_get_property_cookie_t cookie,
                                                 xcb_generic_error_t **e) {
    if (!real_xcb_get_property_reply_fn)
        real_xcb_get_property_reply_fn = (real_xcb_get_property_reply_t)resolve_next_symbol("xcb_get_property_reply", &warned_xcb_get_property_reply);
    if (!real_xcb_get_property_reply_fn) return NULL;

    xcb_get_property_reply_t *reply = real_xcb_get_property_reply_fn(c, cookie, e);

    property_request_t request;
    memset(&request, 0, sizeof(request));
    request.connection = c;
    request.sequence = cookie.sequence;
    snprintf(request.property_name, sizeof(request.property_name), "unknown");
    snprintf(request.type_name, sizeof(request.type_name), "unknown");
    int known = xcb_tracking_enabled() ? lookup_property_request(c, cookie.sequence, &request) : 0;

    reply = active_window_synthesize_property_reply(c, &request, known, reply, e);

    if (diag_xcb_enabled()) {
        if (reply) {
            char reply_type_name[DIAG_NAME_MAX];
            char detail[DIAG_ESCAPE_MAX + 128];
            int value_len = xcb_get_property_value_length(reply);
            if (value_len < 0) value_len = 0;
            lookup_atom_name(c, reply->type, reply_type_name, sizeof(reply_type_name));
            format_property_detail(&request, reply, detail, sizeof(detail));
            diag_logmsg("diag:xcb property_reply%s sequence=%u window=0x%08x property=%s(0x%08x) format=%u type=%s(0x%08x) value_len=%d bytes_after=%u%s%s",
                        request.unchecked ? "_unchecked" : "", cookie.sequence, request.window,
                        request.property_name, request.property, reply->format,
                        reply_type_name, reply->type, value_len, reply->bytes_after,
                        known ? "" : " request=unknown", detail);
        } else {
            unsigned int error_code = (e && *e) ? (*e)->error_code : 0;
            diag_logmsg("diag:xcb property_reply%s sequence=%u window=0x%08x property=%s(0x%08x) reply=NULL error=%u%s",
                        request.unchecked ? "_unchecked" : "", cookie.sequence, request.window,
                        request.property_name, request.property, error_code,
                        known ? "" : " request=unknown");
        }
    }
    return reply;
}

static int is_root_window(xcb_connection_t *c, xcb_drawable_t drawable) {
    const xcb_setup_t *setup = xcb_get_setup(c);
    if (!setup) return 0;
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    while (iter.rem) {
        if (iter.data->root == drawable) return 1;
        xcb_screen_next(&iter);
    }
    return 0;
}

static int take_screenshot(const char *output_path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "env -u QT_QPA_PLATFORM -u LD_PRELOAD "
             "-u GRAB_OVERRIDE_SCREENSHOT -u GRAB_OVERRIDE_ACTIVE_WINDOW "
             "-u GRAB_OVERRIDE_ACTIVE_WINDOW_TTL_MS -u GRAB_OVERRIDE_ACTIVE_WINDOW_DIAG "
             "-u GRAB_OVERRIDE_ACTIVITY -u GRAB_OVERRIDE_ACTIVITY_TTL_MS "
             "-u GRAB_OVERRIDE_ACTIVITY_RATE_MS "
             "-u GRAB_OVERRIDE_DIAG "
             "-u GRAB_OVERRIDE_DIAG_XCB -u GRAB_OVERRIDE_DIAG_XINPUT "
             "spectacle --background --nonotify --fullscreen --output '%s' 2>/dev/null",
             output_path);
    int ret = system(cmd);
    if (ret != 0) return -1;
    struct stat st;
    if (stat(output_path, &st) != 0 || st.st_size == 0) return -1;
    return 0;
}

/* Load PNG and return BGRA pixels for the requested region */
static uint8_t *load_png_pixels(const char *path,
                                 int req_x, int req_y,
                                 int req_w, int req_h,
                                 int *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return NULL; }
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return NULL; }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int png_w = png_get_image_width(png, info);
    int png_h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth = png_get_bit_depth(png, info);

    if (png_w <= 0 || png_h <= 0 || req_w <= 0 || req_h <= 0 ||
        req_w > INT_MAX / 4 / req_h) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    /* BGRA for X11 pixel format */
    png_set_bgr(png);
    png_read_update_info(png, info);

    png_bytep *rows = malloc(sizeof(png_bytep) * png_h);
    if (!rows) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return NULL; }
    size_t rowbytes = png_get_rowbytes(png, info);
    for (int i = 0; i < png_h; i++) {
        rows[i] = malloc(rowbytes);
        if (!rows[i]) {
            for (int j = 0; j < i; j++) free(rows[j]);
            free(rows);
            png_destroy_read_struct(&png, &info, NULL);
            fclose(fp);
            return NULL;
        }
    }
    png_read_image(png, rows);

    int data_size = req_w * req_h * 4;
    uint8_t *data = calloc(1, data_size);
    if (!data) {
        for (int i = 0; i < png_h; i++) free(rows[i]);
        free(rows);
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return NULL;
    }

    for (int y = 0; y < req_h; y++) {
        int src_y = req_y + y;
        if (src_y < 0 || src_y >= png_h) continue;
        for (int x = 0; x < req_w; x++) {
            int src_x = req_x + x;
            if (src_x < 0 || src_x >= png_w) continue;
            memcpy(&data[(y * req_w + x) * 4], &rows[src_y][src_x * 4], 4);
        }
    }

    for (int i = 0; i < png_h; i++) free(rows[i]);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    *out_size = data_size;
    return data;
}

xcb_void_cookie_t
xcb_copy_area(xcb_connection_t *c,
              xcb_drawable_t src_drawable,
              xcb_drawable_t dst_drawable,
              xcb_gcontext_t gc,
              int16_t src_x, int16_t src_y,
              int16_t dst_x, int16_t dst_y,
              uint16_t width, uint16_t height) {

    if (!real_xcb_copy_area_fn)
        real_xcb_copy_area_fn =
            (real_xcb_copy_area_t)resolve_next_symbol("xcb_copy_area", &warned_xcb_copy_area);

    if (!real_xcb_copy_area_fn) {
        xcb_void_cookie_t empty = {0};
        return empty;
    }

    if (!c || !screenshot_enabled() || width == 0 || height == 0 ||
        !is_root_window(c, src_drawable)) {
        /* Normal copy - pass through */
        return real_xcb_copy_area_fn(c, src_drawable, dst_drawable, gc,
                                      src_x, src_y, dst_x, dst_y,
                                      width, height);
    }

    logmsg("xcb_copy_area from ROOT: src(%d,%d) dst(%d,%d) %dx%d",
            src_x, src_y, dst_x, dst_y, width, height);

    /* First do the real copy (will copy black) */
    xcb_void_cookie_t cookie =
        real_xcb_copy_area_fn(c, src_drawable, dst_drawable, gc,
                               src_x, src_y, dst_x, dst_y,
                               width, height);

    /* Now take a real screenshot and overwrite the pixmap */
    char tmppath[256];
    snprintf(tmppath, sizeof(tmppath), "/tmp/td_grab_%d.png", getpid());

    if (take_screenshot(tmppath) != 0) {
        logmsg("Screenshot failed");
        return cookie;
    }

    int data_size = 0;
    uint8_t *pixels = load_png_pixels(tmppath, src_x, src_y,
                                       width, height, &data_size);
    unlink(tmppath);

    if (!pixels) {
        logmsg("Failed to load PNG pixels");
        return cookie;
    }

    /* Get screen depth */
    const xcb_setup_t *setup = xcb_get_setup(c);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    uint8_t depth = iter.data ? iter.data->root_depth : 24;

    /* Write screenshot data to the destination pixmap using xcb_put_image.
     * We send in chunks because xcb has a max request size. */
    uint32_t max_req = xcb_get_maximum_request_length(c);
    /* max data per request in bytes (leave room for header) */
    uint32_t max_data = (max_req * 4) - 64;
    int row_bytes = width * 4;
    int rows_per_chunk = max_data / row_bytes;
    if (rows_per_chunk < 1) rows_per_chunk = 1;
    if (rows_per_chunk > height) rows_per_chunk = height;

    for (int y_off = 0; y_off < height; y_off += rows_per_chunk) {
        int chunk_rows = rows_per_chunk;
        if (y_off + chunk_rows > height)
            chunk_rows = height - y_off;

        xcb_put_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                       dst_drawable, gc,
                       width, chunk_rows,       /* width, height of data */
                       dst_x, dst_y + y_off,    /* dst position */
                       0, depth,
                       chunk_rows * row_bytes,
                       &pixels[y_off * row_bytes]);
    }

    xcb_flush(c);
    free(pixels);

    logmsg("Wrote %dx%d screenshot to pixmap via xcb_put_image", width, height);
    return cookie;
}

__attribute__((constructor))
static void init(void) {
    logmsg("=== grab_override.so loaded (screenshot=%s active_window=%s activity=%s) ===",
           screenshot_enabled() ? "on" : "off",
           active_window_enabled() ? "on" : "off",
           activity_enabled() ? "on" : "off");

    /* Empty epoll-registration slots must not match a real fd 0 (stdin). */
    for (int i = 0; i < SD_EPOLL_TABLE_SIZE; i++) {
        activity_epoll_table[i].fd = -1;
        activity_epoll_table[i].epfd = -1;
    }

    /* Start the in-process bridge threads eagerly so the active-window DBus
     * service is ready to receive KWin pushes and the input-activity watcher
     * tracks from the outset. Both guards are idempotent (pthread_once). */
    if (active_window_enabled())
        pthread_once(&active_window_service_once, active_window_start_service_once);
    if (activity_enabled())
        pthread_once(&activity_thread_once, activity_start_wayland_thread_once);
}
