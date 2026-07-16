CC = gcc
CFLAGS = -Wall -O2
SO_CFLAGS = -shared -fPIC $(CFLAGS)
XI_LIBS := $(shell pkg-config --exists xi 2>/dev/null && pkg-config --cflags --libs xi)

# The preload now hosts both bridges in-process: the active-window DBus
# service (GLib/GDBus) and the input-activity Wayland watcher. GLib/GDBus is
# linked; libwayland-client is loaded with dlopen at runtime (see -ldl), so no
# Wayland -devel package is required.
GLIB_CFLAGS := $(shell pkg-config --cflags gio-2.0 glib-2.0)
GLIB_LIBS := $(shell pkg-config --libs gio-2.0 glib-2.0)
LIBS = $(shell pkg-config --cflags --libs xcb xcb-randr x11 libpng) $(XI_LIBS) $(GLIB_LIBS) -ldl -pthread

# The standalone helpers below are optional diagnostic tools; the preload no
# longer needs them at runtime. Build them with `make helpers`. libwayland is
# linked by soname so no -devel package (unversioned .so symlink) is needed.
ACTIVITY_LIBS := $(shell pkg-config --libs wayland-client 2>/dev/null || echo -l:libwayland-client.so.0)

all: grab_override.so

grab_override.so: grab_override.c active_window_state.h
	$(CC) $(SO_CFLAGS) $(GLIB_CFLAGS) -o $@ grab_override.c $(LIBS)

helpers: screen-doctor-active-window-helper screen-doctor-activity-helper

screen-doctor-active-window-helper: screen-doctor-active-window-helper.c active_window_state.h
	$(CC) $(CFLAGS) $(GLIB_CFLAGS) -o $@ screen-doctor-active-window-helper.c $(GLIB_LIBS)

screen-doctor-activity-helper: screen-doctor-activity-helper.c activity_state.h
	$(CC) $(CFLAGS) -o $@ screen-doctor-activity-helper.c $(ACTIVITY_LIBS)

test: test_grab test_xcb_grab

test_grab: test_grab.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs x11 libpng)

test_xcb_grab: test_xcb_grab.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs xcb)

clean:
	rm -f grab_override.so screen-doctor-active-window-helper screen-doctor-activity-helper test_grab test_xcb_grab

.PHONY: all helpers test clean
