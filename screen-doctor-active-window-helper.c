#define _GNU_SOURCE
#include "active_window_state.h"

#include <gio/gio.h>
#include <glib.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SD_DBUS_NAME "org.screen_doctor.ActiveWindow"
#define SD_DBUS_OBJECT_PATH "/org/screen_doctor/ActiveWindow"
#define SD_DBUS_INTERFACE "org.screen_doctor.ActiveWindow"

static const gchar introspection_xml[] =
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
    "    <method name='Ping'>"
    "      <arg type='s' name='status' direction='out'/>"
    "    </method>"
    "    <method name='GetState'>"
    "      <arg type='u' name='magic' direction='out'/>"
    "      <arg type='u' name='version' direction='out'/>"
    "      <arg type='t' name='updated_monotonic_ms' direction='out'/>"
    "      <arg type='u' name='pid' direction='out'/>"
    "      <arg type='u' name='flags' direction='out'/>"
    "      <arg type='s' name='title' direction='out'/>"
    "      <arg type='s' name='resource_class' direction='out'/>"
    "      <arg type='s' name='resource_name' direction='out'/>"
    "      <arg type='s' name='desktop_file' direction='out'/>"
    "      <arg type='s' name='internal_id' direction='out'/>"
    "    </method>"
    "  </interface>"
    "</node>";

static GMainLoop *main_loop;
static GDBusNodeInfo *introspection_data;
static sd_active_window_state_t current_state;
static char state_path[PATH_MAX];
static int debug_enabled;

static uint64_t monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int env_equals(const char *name, const char *expected) {
    const char *value = getenv(name);
    return value && strcmp(value, expected) == 0;
}

static void debug_log(const char *fmt, ...) {
    if (!debug_enabled) return;

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) return;
    if (!src) src = "";
    snprintf(dst, dst_size, "%s", src);
}

static int string_nonempty(const char *value) {
    return value && value[0] != '\0';
}

static void desktop_basename(char *dst, size_t dst_size, const char *desktop_file) {
    if (!dst || dst_size == 0) return;
    dst[0] = '\0';
    if (!string_nonempty(desktop_file)) return;

    const char *base = strrchr(desktop_file, '/');
    base = base ? base + 1 : desktop_file;
    copy_string(dst, dst_size, base);

    size_t len = strlen(dst);
    const char suffix[] = ".desktop";
    size_t suffix_len = sizeof(suffix) - 1;
    if (len > suffix_len && strcmp(dst + len - suffix_len, suffix) == 0) {
        dst[len - suffix_len] = '\0';
    }
}

static void lowercase_simple(char *dst, size_t dst_size, const char *src) {
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

static void resolve_state_path(void) {
    const char *override = getenv("SCREEN_DOCTOR_ACTIVE_WINDOW_STATE");
    if (!string_nonempty(override)) override = getenv("GRAB_OVERRIDE_ACTIVE_WINDOW_STATE");
    if (string_nonempty(override)) {
        copy_string(state_path, sizeof(state_path), override);
        return;
    }

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!string_nonempty(runtime_dir)) {
        state_path[0] = '\0';
        return;
    }

    snprintf(state_path, sizeof(state_path), "%s/screen-doctor/active-window.bin", runtime_dir);
}

static int write_all(int fd, const void *data, size_t len) {
    const unsigned char *bytes = (const unsigned char *)data;
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

static int write_state_file(const sd_active_window_state_t *state) {
    if (!string_nonempty(state_path)) {
        fprintf(stderr, "screen-doctor-active-window-helper: XDG_RUNTIME_DIR is not set and no state override was provided\n");
        return -1;
    }

    char *dir = g_path_get_dirname(state_path);
    if (!dir) return -1;
    if (g_mkdir_with_parents(dir, 0700) != 0) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to create %s: %s\n", dir, g_strerror(errno));
        g_free(dir);
        return -1;
    }
    chmod(dir, 0700);

    char tmp_path[PATH_MAX];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.active-window.bin.%ld.tmp", dir, (long)getpid());
    g_free(dir);

    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to open %s: %s\n", tmp_path, g_strerror(errno));
        return -1;
    }

    int ok = 0;
    if (write_all(fd, state, sizeof(*state)) != 0) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to write %s: %s\n", tmp_path, g_strerror(errno));
        ok = -1;
    }
    if (ok == 0 && fsync(fd) != 0) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to fsync %s: %s\n", tmp_path, g_strerror(errno));
        ok = -1;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to close %s: %s\n", tmp_path, g_strerror(errno));
        ok = -1;
    }
    if (ok == 0 && rename(tmp_path, state_path) != 0) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to rename %s to %s: %s\n", tmp_path, state_path, g_strerror(errno));
        ok = -1;
    }
    if (ok != 0) unlink(tmp_path);
    return ok;
}

static void fill_state(sd_active_window_state_t *state, const char *title,
                       const char *resource_class, const char *resource_name,
                       const char *desktop_file, gint32 pid, gboolean normal_window,
                       const char *internal_id) {
    memset(state, 0, sizeof(*state));
    state->magic = SD_ACTIVE_WINDOW_MAGIC;
    state->version = SD_ACTIVE_WINDOW_VERSION;
    state->updated_monotonic_ms = monotonic_ms();

    int valid = string_nonempty(title) || string_nonempty(resource_class) ||
                string_nonempty(resource_name) || string_nonempty(desktop_file) ||
                string_nonempty(internal_id) || pid > 0 || normal_window;

    if (pid > 0) {
        state->pid = (uint32_t)pid;
        state->flags |= SD_ACTIVE_WINDOW_FLAG_HAS_PID;
    }
    if (valid) state->flags |= SD_ACTIVE_WINDOW_FLAG_VALID;
    if (normal_window) state->flags |= SD_ACTIVE_WINDOW_FLAG_NORMAL;

    copy_string(state->title, sizeof(state->title), title);
    copy_string(state->desktop_file, sizeof(state->desktop_file), desktop_file);
    copy_string(state->internal_id, sizeof(state->internal_id), internal_id);

    if (string_nonempty(resource_class)) {
        copy_string(state->resource_class, sizeof(state->resource_class), resource_class);
    } else {
        desktop_basename(state->resource_class, sizeof(state->resource_class), desktop_file);
        if (!string_nonempty(state->resource_class) && string_nonempty(resource_name)) {
            copy_string(state->resource_class, sizeof(state->resource_class), resource_name);
        }
        if (!string_nonempty(state->resource_class) && valid) {
            copy_string(state->resource_class, sizeof(state->resource_class), "unknown");
        }
    }

    if (string_nonempty(resource_name)) {
        copy_string(state->resource_name, sizeof(state->resource_name), resource_name);
    } else if (string_nonempty(state->resource_class)) {
        lowercase_simple(state->resource_name, sizeof(state->resource_name), state->resource_class);
        if (!string_nonempty(state->resource_name)) {
            copy_string(state->resource_name, sizeof(state->resource_name), state->resource_class);
        }
    }
}

static gboolean refresh_state_timestamp(gpointer user_data) {
    (void)user_data;

    if ((current_state.flags & SD_ACTIVE_WINDOW_FLAG_VALID) == 0) {
        return G_SOURCE_CONTINUE;
    }

    current_state.updated_monotonic_ms = monotonic_ms();
    if (write_state_file(&current_state) != 0) {
        debug_log("screen-doctor-active-window-helper: failed to refresh active-window cache timestamp");
    }
    return G_SOURCE_CONTINUE;
}

static void handle_method_call(GDBusConnection *connection, const gchar *sender,
                               const gchar *object_path, const gchar *interface_name,
                               const gchar *method_name, GVariant *parameters,
                               GDBusMethodInvocation *invocation, gpointer user_data) {
    (void)connection;
    (void)sender;
    (void)object_path;
    (void)interface_name;
    (void)user_data;

    if (g_strcmp0(method_name, "Ping") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(s)", "ok"));
        return;
    }

    if (g_strcmp0(method_name, "GetState") == 0) {
        g_dbus_method_invocation_return_value(
            invocation,
            g_variant_new("(uutuusssss)", current_state.magic, current_state.version,
                          current_state.updated_monotonic_ms, current_state.pid,
                          current_state.flags, current_state.title,
                          current_state.resource_class, current_state.resource_name,
                          current_state.desktop_file, current_state.internal_id));
        return;
    }

    if (g_strcmp0(method_name, "Update") == 0) {
        const char *title = "";
        const char *resource_class = "";
        const char *resource_name = "";
        const char *desktop_file = "";
        const char *internal_id = "";
        gint32 pid = 0;
        gboolean normal_window = FALSE;

        g_variant_get(parameters, "(&s&s&s&sib&s)", &title, &resource_class,
                      &resource_name, &desktop_file, &pid, &normal_window,
                      &internal_id);

        fill_state(&current_state, title, resource_class, resource_name,
                   desktop_file, pid, normal_window, internal_id);

        if (write_state_file(&current_state) != 0) {
            g_dbus_method_invocation_return_error(invocation, G_IO_ERROR,
                                                  G_IO_ERROR_FAILED,
                                                  "failed to write active-window cache");
            return;
        }

        debug_log("screen-doctor-active-window-helper: update title=\"%s\" class=\"%s\" name=\"%s\" pid=%u flags=0x%x",
                  current_state.title, current_state.resource_class,
                  current_state.resource_name, current_state.pid,
                  current_state.flags);
        g_dbus_method_invocation_return_value(invocation, NULL);
        return;
    }

    g_dbus_method_invocation_return_error(invocation, G_IO_ERROR,
                                          G_IO_ERROR_NOT_SUPPORTED,
                                          "unknown method %s", method_name);
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call,
    NULL,
    NULL,
    {0}
};

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)name;
    (void)user_data;

    GError *error = NULL;
    guint registration_id = g_dbus_connection_register_object(
        connection,
        SD_DBUS_OBJECT_PATH,
        introspection_data->interfaces[0],
        &interface_vtable,
        NULL,
        NULL,
        &error);
    if (registration_id == 0) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to register object: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        if (main_loop) g_main_loop_quit(main_loop);
    }
}

static void on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    (void)user_data;
    fprintf(stderr, "screen-doctor-active-window-helper: owning %s, cache=%s\n",
            name, string_nonempty(state_path) ? state_path : "<unset>");
}

static void on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)connection;
    (void)user_data;
    fprintf(stderr, "screen-doctor-active-window-helper: lost bus name %s\n", name);
    if (main_loop) g_main_loop_quit(main_loop);
}

int main(void) {
    debug_enabled = env_equals("SCREEN_DOCTOR_HELPER_DEBUG", "1");
    resolve_state_path();

    memset(&current_state, 0, sizeof(current_state));
    current_state.magic = SD_ACTIVE_WINDOW_MAGIC;
    current_state.version = SD_ACTIVE_WINDOW_VERSION;

    GError *error = NULL;
    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, &error);
    if (!introspection_data) {
        fprintf(stderr, "screen-doctor-active-window-helper: failed to parse introspection XML: %s\n",
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return 1;
    }

    guint owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
                                    SD_DBUS_NAME,
                                    G_BUS_NAME_OWNER_FLAGS_NONE,
                                    on_bus_acquired,
                                    on_name_acquired,
                                    on_name_lost,
                                    NULL,
                                    NULL);

    main_loop = g_main_loop_new(NULL, FALSE);
    g_timeout_add_seconds(2, refresh_state_timestamp, NULL);
    g_main_loop_run(main_loop);

    g_bus_unown_name(owner_id);
    g_main_loop_unref(main_loop);
    g_dbus_node_info_unref(introspection_data);
    return 0;
}
