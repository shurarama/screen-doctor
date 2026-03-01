CC = gcc
CFLAGS = -shared -fPIC -Wall -O2
LIBS = $(shell pkg-config --cflags --libs xcb xcb-randr libpng) -ldl

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
