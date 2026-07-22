# Screen Doctor

`LD_PRELOAD` compatibility hooks for Time Doctor on KDE Plasma Wayland.

The project currently fixes three XWayland/Wayland gaps:

- Screenshot capture: replaces black XWayland root screenshots with real KDE screenshots.
- Active app/window tracking: exposes the real KWin active Wayland window to Time Doctor through synthetic XCB/EWMH replies.
- Idle/activity tracking: synthesizes side-effect-free XInput2 `RawMotion` events while the compositor reports real input, so native Wayland activity is not misread as idle.

Everything runs inside a single preloaded library. The only external component is the KWin script, which the compositor loads to report the active window.

## Components

- `grab_override.so`: the preload library loaded into Time Doctor. It hosts, on dedicated background threads:
  - the active-window D-Bus service (`org.screen_doctor.ActiveWindow`), which receives updates from the KWin script and publishes them in memory;
  - the input-activity watcher, which subscribes to the compositor's `ext_idle_notifier_v1` and drives the idle bridge in memory.
- `kwin/screen-doctor-active-window`: KWin script that publishes active-window metadata to the D-Bus service.

No helper processes and no runtime cache files are involved; all bridge state lives in the preload's memory.

Architecture:

```text
KWin script ─(D-Bus)─┐
                     ├─> grab_override.so (in-process threads + XCB/XInput2 shims) ─> Time Doctor
compositor idle ─────┘   (ext_idle_notifier_v1)
```

## Requirements

- KDE Plasma 6 on Wayland.
- `spectacle`.
- `kpackagetool6` and `kwriteconfig6` for installing/enabling the KWin script.
- Build tools and development headers for XCB, X11, libpng, GLib/GIO.
- Optional XInput2 development headers. The build has fallback definitions when `xi.pc`/`XInput2.h` are unavailable, but installing them is preferred.

### Fedora

```bash
sudo dnf install gcc make pkgconf-pkg-config libX11-devel libXi-devel libxcb-devel libpng-devel glib2-devel
```

### Debian/Ubuntu

```bash
sudo apt install gcc make pkg-config libx11-dev libxi-dev libxcb1-dev libxcb-randr0-dev libpng-dev libglib2.0-dev
```

## Build

```bash
make clean && make
```

Build output:

- `grab_override.so` — the only artifact Time Doctor needs.

The standalone helper binaries (`screen-doctor-active-window-helper`, `screen-doctor-activity-helper`) are no longer required at runtime; their logic now runs inside `grab_override.so`. They remain available as optional diagnostic tools:

```bash
make helpers
```

Do not run `screen-doctor-active-window-helper` alongside Time Doctor: both would contend for the `org.screen_doctor.ActiveWindow` bus name.

Run the local build checks:

```bash
make test
```

## Screenshot Hook

Time Doctor uses `xcb_copy_area()` from the X11 root window for screenshots. On Wayland, the XWayland root window is often blank. `grab_override.so` intercepts root-window `xcb_copy_area()` calls and replaces the destination pixmap with a real screenshot captured through KDE/Spectacle.

The screenshot hook is disabled by default. Enable it only when screenshot replacement is needed:

```bash
GRAB_OVERRIDE_SCREENSHOT=1
```

This keeps the active-window fix isolated from the heavier screenshot path, which launches `spectacle` and writes image data back through Time Doctor's XCB connection.

Screenshot logs are written to:

```text
/tmp/grab_override.log
```

Expected screenshot log lines:

```text
xcb_copy_area from ROOT: src(0,0) dst(0,0) 2560x1440
Wrote 2560x1440 screenshot to pixmap via xcb_put_image
```

## Active Window Bridge

Time Doctor queries active apps through XCB properties such as:

- `_NET_ACTIVE_WINDOW`
- `_NET_WM_PID`
- `_NET_WM_NAME`
- `WM_NAME`
- `WM_CLASS`
- `_NET_CLIENT_LIST`

On KDE Wayland, native Wayland windows are not normal X11 clients, so Time Doctor may see an empty XWayland placeholder instead of the real app.

The bridge fixes that by:

1. Reading the real active window from KWin JavaScript APIs.
2. Sending `title`, `pid`, `resourceClass`, `resourceName`, `desktopFileName`, and `internalId` to the helper over D-Bus.
3. Writing a small runtime cache file.
4. Returning synthetic XCB replies to Time Doctor from `grab_override.so`.

The synthetic active X11 window id is:

```text
0x7f534401
```

## Install KWin Script

Install the script into the user KWin script directory:

```bash
kpackagetool6 --type=KWin/Script -i /home/ashperling/screen-doctor/kwin/screen-doctor-active-window
```

Enable it:

```bash
kwriteconfig6 --file kwinrc --group Plugins --key screen-doctor-active-windowEnabled true
busctl --user call org.kde.KWin /KWin org.kde.KWin reconfigure
busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start
```

Check that KWin loaded it:

```bash
busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting isScriptLoaded s screen-doctor-active-window
```

Expected result:

```text
b true
```

If the script was already installed and needs to be reloaded:

```bash
busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s screen-doctor-active-window
busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start
```

## Active-Window Service

There is no helper process to start: when `GRAB_OVERRIDE_ACTIVE_WINDOW=1`, `grab_override.so` starts a background thread inside Time Doctor that owns this session bus name and receives updates from the KWin script:

```text
org.screen_doctor.ActiveWindow
```

Smoke test (with Time Doctor running under the preload):

```bash
gdbus call --session \
  --dest org.screen_doctor.ActiveWindow \
  --object-path /org/screen_doctor/ActiveWindow \
  --method org.screen_doctor.ActiveWindow.Ping
```

Expected result:

```text
('ok',)
```

The service holds the latest active-window state in memory. Each `Update` from the KWin script refreshes it; the preload treats it as stale once its age exceeds `GRAB_OVERRIDE_ACTIVE_WINDOW_TTL_MS`, so stale state means the KWin script stopped updating, not merely that the focused window did not change.

## Run Time Doctor

Manual launch with the active-window fix:

```bash
env \
  QT_QPA_PLATFORM=xcb \
  GRAB_OVERRIDE_ACTIVE_WINDOW=1 \
  GRAB_OVERRIDE_ACTIVITY=1 \
  GRAB_OVERRIDE_ACTIVITY_PERCENT=100 \
  LD_PRELOAD=/home/ashperling/screen-doctor/grab_override.so \
  /home/ashperling/timedoctor/timedoctor --no-sandbox --customuri=
```

Add `GRAB_OVERRIDE_SCREENSHOT=1` only when the screenshot replacement hook is required.

Important environment variables:

- `QT_QPA_PLATFORM=xcb`: force Qt onto XCB/XWayland.
- `LD_PRELOAD=/home/ashperling/screen-doctor/grab_override.so`: load the hook library.
- `GRAB_OVERRIDE_SCREENSHOT=1`: enable root-window screenshot replacement. Disabled by default.
- `GRAB_OVERRIDE_ACTIVE_WINDOW=1`: enable synthetic XCB active-window replies (starts the in-process D-Bus service).
- `GRAB_OVERRIDE_ACTIVE_WINDOW_TTL_MS=5000`: active-window freshness threshold, default `5000`.
- `GRAB_OVERRIDE_ACTIVITY=1`: enable the input-activity bridge (starts the in-process Wayland idle watcher and RawMotion synthesis).
- `GRAB_OVERRIDE_ACTIVITY_PERCENT=100`: expected percentage of calendar minutes in which synthetic activity is allowed, default `100`.

Each hook is disabled by default unless its corresponding `GRAB_OVERRIDE_*` variable is set.

## Diagnostics

General diagnostics:

```bash
GRAB_OVERRIDE_DIAG=1
```

Category controls:

- `GRAB_OVERRIDE_DIAG_XINPUT=0`: disable XInput diagnostics while general diagnostics are on.
- `GRAB_OVERRIDE_DIAG_XCB=0`: disable XCB diagnostics while general diagnostics are on.
- `GRAB_OVERRIDE_ACTIVE_WINDOW_DIAG=1`: log active-window bridge decisions.

Useful diagnostic launch for checking idle/input without flooding XCB property logs:

```bash
env \
  QT_QPA_PLATFORM=xcb \
  GRAB_OVERRIDE_ACTIVE_WINDOW=1 \
  GRAB_OVERRIDE_ACTIVITY=1 \
  GRAB_OVERRIDE_DIAG=1 \
  GRAB_OVERRIDE_DIAG_XCB=0 \
  LD_PRELOAD=/home/ashperling/screen-doctor/grab_override.so \
  /home/ashperling/timedoctor/timedoctor --no-sandbox --customuri=
```

Expected XInput logs when Time Doctor sees activity:

```text
diag:xinput XISelectEvents window=... masks=RawKeyPress,RawKeyRelease,RawButtonPress,RawButtonRelease,RawMotion
diag:xinput XGetEventData evtype=RawMotion(...)
diag:xinput XGetEventData evtype=RawKeyPress(...)
diag:xinput XGetEventData evtype=RawKeyRelease(...)
```

Expected active-window diagnostics:

```text
active-window cache loaded title="..." pid=... class="..."
active-window synth property=_NET_ACTIVE_WINDOW value_window=0x7f534401
active-window synth property=_NET_WM_PID pid=...
active-window synth property=_NET_WM_NAME text="..."
active-window synth property=WM_CLASS text="instance\0class\0"
```

## Validation

Build validation:

```bash
make clean && make && make test
```

Verify exported hooks:

```bash
nm -D grab_override.so | grep -E 'xcb_copy_area|xcb_get_property|xcb_get_property_reply|XISelectEvents|XGetEventData'
```

Verify that Time Doctor loaded this library:

```bash
pid=$(pgrep -n -x timedoctor)
tr '\0' '\n' < /proc/$pid/environ | grep -E '^(QT_QPA_PLATFORM|GRAB_OVERRIDE|LD_PRELOAD)='
grep -E 'screen-doctor|grab_override|libqxcb' /proc/$pid/maps | cut -d' ' -f6- | sort -u
```

Expected:

```text
QT_QPA_PLATFORM=xcb
LD_PRELOAD=/home/ashperling/screen-doctor/grab_override.so
/home/ashperling/screen-doctor/grab_override.so
```

If diagnostics are enabled, `/tmp/grab_override.log` should show active-window synthetic replies and screenshot hook activity.

## Idle Time Notes

The active-window bridge itself does not synthesize input; it fixes what app/window Time Doctor sees. The separate input-activity bridge (below) is what synthesizes `RawMotion` so idle tracking reflects native Wayland work.

Idle time depends on whether Time Doctor receives XInput2 activity events under XWayland. Use XInput diagnostics to verify this while typing or moving the mouse in a native Wayland app.

If these counters/logs move, Time Doctor sees input:

```text
RawMotion
RawButtonPress
RawButtonRelease
RawKeyPress
RawKeyRelease
```

If they do not move during real work, that is what the input-activity bridge below fixes. If they do move but the dashboard still shows high idle, the problem is likely in Time Doctor's own classification/upload/backend logic.

## Input Activity Bridge

On XWayland, Time Doctor's XInput2 idle monitor only sees input that reaches the X server. Native Wayland apps (Firefox, VS Code, Dolphin, ...) deliver input straight to the compositor, so typing and mouse movement there never generate the `RawKeyPress`/`RawMotion` events Time Doctor counts, and it reports the user as idle.

The bridge is entirely in-process (no helper, no state file):

- A background thread inside `grab_override.so` opens its own Wayland connection and subscribes to `ext_idle_notifier_v1` (the input-idle variant on v2+, which reacts to real input and ignores idle-inhibitors). While the user is active it keeps an in-memory timestamp fresh. It records only the boolean fact that input occurred - never keycodes or coordinates. `libwayland-client` is loaded with `dlopen` at runtime, so the library keeps no build-time dependency on it and simply no-ops if Wayland is absent.
- The synthesizer delivers `XI_RawMotion` at the layer where Time Doctor actually waits. Time Doctor does not block in `XNextEvent`; it integrates the raw-input X connection into its main event loop's `poll`/`ppoll` and drains via `XPending`/`XNextEvent` from a callback. So the library hooks `poll`/`ppoll` **scoped strictly to that one connection's file descriptor** (every other poll call in the process passes straight through): when the fd is quiet and the user is active, it marks the fd readable at most once per rate interval, and the `XPending`/`XNextEvent`/`XGetEventData` hooks then hand back one fabricated `RawMotion` cookie. `RawMotion` is used because it is side-effect-free: it cannot type or click anything. Real events are never suppressed - if genuine input arrives on the fd, it is delivered unchanged and no synthetic event is added.

If the compositor stops reporting input, the in-memory stamp goes stale within the TTL and synthesis stops - the bridge makes real activity *visible*, it does not fabricate activity that did not happen.

Enable it in Time Doctor's environment (no separate process to start):

```bash
GRAB_OVERRIDE_ACTIVITY=1
```

Tuning (optional):

- `GRAB_OVERRIDE_ACTIVITY_TTL_MS` (default 2000): max age of a "fresh" activity stamp before synthesis stops.
- `GRAB_OVERRIDE_ACTIVITY_RATE_MS` (default 1000): minimum gap between synthesized events; also the cap on the monitor connection's poll wait.
- `GRAB_OVERRIDE_ACTIVITY_PERCENT` (integer `0..100`, default `100`): independently allows each wall-clock minute with the configured probability. For example, `80` means that each calendar minute is selected with an 80% probability; the observed ratio converges to 80% over time rather than enforcing an exact hourly quota. A selected minute still produces no synthetic events unless `activity_is_fresh()` confirms recent real Wayland input. `0` disables synthetic activity and `100` preserves the previous behavior; invalid or out-of-range values fall back to `100`. Real X11/XInput events are never suppressed, so Time Doctor's final percentage can exceed this target.
- `GRAB_OVERRIDE_ACTIVITY_PROFILE=1` (default off): shape the synthetic stream to resemble human input instead of a single bare RawMotion per interval. Emits weighted bursts — motion runs with real valuator (dx/dy) deltas, keystroke press/release pairs, and clicks/scrolls — restricted to the raw event classes the monitor actually subscribed to, with jittered gaps between bursts. Still gated on real compositor input (`activity_is_fresh()`); it never fabricates activity from nothing, and remains privacy-preserving (a plausible profile, not a replay of real keys/coordinates).

Expected bridge diagnostics (with `GRAB_OVERRIDE_DIAG=1`):

```text
diag:activity wayland watching via get_input_idle_notification
diag:activity monitor display=0x... fd=NN noted (raw-motion subscription)
diag:activity state fresh=1 (in-process wayland watcher)
diag:activity minute=29740035 percent=80 synth_allowed=1
diag:activity synth evtype=RawMotion device=2 cookie=0x5d000003
diag:xinput XNextEvent type=GenericEvent(35) extension=131 evtype=RawMotion(17) synthetic=1
```

## Logs

Preload log:

```text
/tmp/grab_override.log
```

Time Doctor logs:

```text
/home/ashperling/.local/share/TD/timedoctor2/logs
```

KWin script logs can usually be observed with:

```bash
journalctl --user -f | grep screen-doctor-active-window
```

## License

MIT
