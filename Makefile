CC = gcc
CFLAGS = -shared -fPIC -Wall -O2
XI_LIBS := $(shell pkg-config --exists xi 2>/dev/null && pkg-config --cflags --libs xi)
LIBS = $(shell pkg-config --cflags --libs xcb xcb-randr x11 libpng) $(XI_LIBS) -ldl -pthread

all: grab_override.so

grab_override.so: grab_override.c
	$(CC) $(CFLAGS) -o $@ $< $(LIBS)

test: test_grab test_xcb_grab

test_grab: test_grab.c
	$(CC) -Wall -O2 -o $@ $< $(shell pkg-config --cflags --libs x11 libpng)

test_xcb_grab: test_xcb_grab.c
	$(CC) -Wall -O2 -o $@ $< $(shell pkg-config --cflags --libs xcb)

clean:
	rm -f grab_override.so test_grab test_xcb_grab

.PHONY: all test clean
