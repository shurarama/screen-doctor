# Screen Doctor

An `LD_PRELOAD` fix for *brilliantly engineered* desktop applications that take **black screenshots** on **KDE Plasma Wayland**.

Some truly genius software products — beloved by forward-thinking employers everywhere — still rely on legacy X11 APIs to capture your screen. Because why support Wayland when X11 has been around since 1984 and clearly nothing has changed since then?

These magnificent applications use `xcb_copy_area()` from the X11 root window to take screenshots. On Wayland, the XWayland root window is a beautiful canvas of pure darkness, because Wayland actually isolates applications from each other (a concept apparently too advanced for certain development teams). The result: every screenshot is a black rectangle. Your productivity dashboard looks like you work inside a black hole.

## The Solution

Screen Doctor intercepts `xcb_copy_area()` via `LD_PRELOAD`. When it detects a copy from the root window (i.e., a screenshot attempt by one of these architectural masterpieces), it:

1. Takes a real screenshot through KDE's portal API (via `spectacle`)
2. Writes the actual screen content into the destination pixmap via `xcb_put_image()`

Your screenshots now show your actual desktop instead of the void.

## Requirements

- KDE Plasma 6 on Wayland
- `spectacle` (comes with KDE)
- Build deps: `gcc`, `libxcb-devel`, `libpng-devel`

### Fedora
```bash
sudo dnf install gcc libX11-devel libxcb-devel libpng-devel
```

### Ubuntu/Debian
```bash
sudo apt install gcc libx11-dev libxcb1-dev libxcb-randr0-dev libpng-dev pkg-config
```

## Building

```bash
make
```

## Usage

Edit the `.desktop` file of the application you need to fix:

```ini
Exec=env QT_QPA_PLATFORM=xcb WAYLAND_DISPLAY= LD_PRELOAD=/path/to/grab_override.so /path/to/your/genius/app
```

Key environment variables:
- `QT_QPA_PLATFORM=xcb` — force the app to use XWayland
- `WAYLAND_DISPLAY=` — prevent Qt from auto-detecting Wayland
- `LD_PRELOAD=...` — load Screen Doctor

Restart the application.

## Logs

Check `/tmp/grab_override.log` to verify it's working:

```
[17:37:26 pid=196692] xcb_copy_area from ROOT: src(0,0) dst(0,0) 2560x1440
[17:37:29 pid=196692] Wrote 2560x1440 screenshot to pixmap
```

## How It Works

The typical Qt/XCB screenshot pipeline:
1. `QScreen::grabWindow()` → `QXcbScreen::grabWindow()`
2. `xcb_create_pixmap()` — allocate a pixmap
3. `xcb_copy_area(root_window → pixmap)` — **black on XWayland**
4. Convert pixmap → QImage → save

Screen Doctor intercepts step 3 and replaces the black content with a real screenshot obtained through the XDG Desktop Portal — an API designed by people who are aware that Wayland exists.

## License

MIT
