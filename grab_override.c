#define _GNU_SOURCE
#include <xcb/xcb.h>
#include <X11/Xlib.h>
#include <dlfcn.h>
#include <ctype.h>
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
 * LD_PRELOAD: intercept xcb_copy_area on root window.
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

static void store_property_request(xcb_connection_t *connection, uint32_t sequence, int unchecked,
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

    Bool result = real_XGetEventData_fn(display, cookie);

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
    if (!real_XFreeEventData_fn)
        real_XFreeEventData_fn = (real_XFreeEventData_t)resolve_next_symbol("XFreeEventData", &warned_XFreeEventData);
    if (!real_XFreeEventData_fn) return;

    real_XFreeEventData_fn(display, cookie);
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

    if (diag_xcb_enabled()) {
        char name_buf[DIAG_NAME_MAX];
        char escaped[DIAG_ESCAPE_MAX];
        copy_len_string(name_buf, sizeof(name_buf), name, name_len);
        escape_bytes(name, name_len, escaped, sizeof(escaped));
        store_intern_request(c, cookie.sequence, 0, name_buf);
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

    if (diag_xcb_enabled()) {
        char name_buf[DIAG_NAME_MAX];
        char escaped[DIAG_ESCAPE_MAX];
        copy_len_string(name_buf, sizeof(name_buf), name, name_len);
        escape_bytes(name, name_len, escaped, sizeof(escaped));
        store_intern_request(c, cookie.sequence, 1, name_buf);
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

    if (diag_xcb_enabled()) {
        char name[DIAG_NAME_MAX];
        int unchecked = 0;
        int known = lookup_intern_request(c, cookie.sequence, name, sizeof(name), &unchecked);
        if (reply && reply->atom != XCB_ATOM_NONE && known) {
            pthread_mutex_lock(&diag_xcb_mutex);
            store_atom_name_locked(c, reply->atom, name);
            pthread_mutex_unlock(&diag_xcb_mutex);
        }

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

    if (diag_xcb_enabled()) {
        char property_name[DIAG_NAME_MAX];
        char type_name[DIAG_NAME_MAX];
        store_property_request(c, cookie.sequence, 0, window, property, type, long_offset, long_length,
                               property_name, sizeof(property_name), type_name, sizeof(type_name));
        diag_logmsg("diag:xcb get_property sequence=%u delete=%u window=0x%08x property=%s(0x%08x) type=%s(0x%08x) offset=%u length=%u tracked=%d",
                    cookie.sequence, delete, window, property_name, property, type_name, type,
                    long_offset, long_length, is_tracked_property_name(property_name));
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

    if (diag_xcb_enabled()) {
        char property_name[DIAG_NAME_MAX];
        char type_name[DIAG_NAME_MAX];
        store_property_request(c, cookie.sequence, 1, window, property, type, long_offset, long_length,
                               property_name, sizeof(property_name), type_name, sizeof(type_name));
        diag_logmsg("diag:xcb get_property_unchecked sequence=%u delete=%u window=0x%08x property=%s(0x%08x) type=%s(0x%08x) offset=%u length=%u tracked=%d",
                    cookie.sequence, delete, window, property_name, property, type_name, type,
                    long_offset, long_length, is_tracked_property_name(property_name));
    }
    return cookie;
}

xcb_get_property_reply_t *xcb_get_property_reply(xcb_connection_t *c, xcb_get_property_cookie_t cookie,
                                                 xcb_generic_error_t **e) {
    if (!real_xcb_get_property_reply_fn)
        real_xcb_get_property_reply_fn = (real_xcb_get_property_reply_t)resolve_next_symbol("xcb_get_property_reply", &warned_xcb_get_property_reply);
    if (!real_xcb_get_property_reply_fn) return NULL;

    xcb_get_property_reply_t *reply = real_xcb_get_property_reply_fn(c, cookie, e);

    if (diag_xcb_enabled()) {
        property_request_t request;
        memset(&request, 0, sizeof(request));
        request.connection = c;
        request.sequence = cookie.sequence;
        snprintf(request.property_name, sizeof(request.property_name), "unknown");
        snprintf(request.type_name, sizeof(request.type_name), "unknown");
        int known = lookup_property_request(c, cookie.sequence, &request);

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
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "env -u QT_QPA_PLATFORM spectacle --background --nonotify --fullscreen --output '%s' 2>/dev/null",
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
    for (int i = 0; i < png_h; i++)
        rows[i] = malloc(png_get_rowbytes(png, info));
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

    if (!is_root_window(c, src_drawable)) {
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
    logmsg("=== grab_override.so loaded (xcb_copy_area intercept) ===");
}
