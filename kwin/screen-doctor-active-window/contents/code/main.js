function log(message) {
    console.info("screen-doctor-active-window: " + message);
}

const watchedWindows = new Map();

function stringProperty(window, name) {
    try {
        const value = window ? window[name] : "";
        if (value === undefined || value === null) return "";
        return String(value);
    } catch (error) {
        return "";
    }
}

function numberProperty(window, name) {
    const text = stringProperty(window, name);
    const value = Number(text);
    return Number.isFinite(value) ? value : 0;
}

function boolProperty(window, name) {
    try {
        return Boolean(window && window[name]);
    } catch (error) {
        return false;
    }
}

function sendUpdate(title, resourceClass, resourceName, desktopFileName, pid, normalWindow, internalId) {
    try {
        callDBus(
            "org.screen_doctor.ActiveWindow",
            "/org/screen_doctor/ActiveWindow",
            "org.screen_doctor.ActiveWindow",
            "Update",
            title,
            resourceClass,
            resourceName,
            desktopFileName,
            pid,
            normalWindow,
            internalId,
            function() {}
        );
    } catch (error) {
        log("Update call failed: " + error);
    }
}

function publish(window) {
    if (!window) {
        sendUpdate("", "", "", "", 0, false, "");
        return;
    }

    sendUpdate(
        stringProperty(window, "caption"),
        stringProperty(window, "resourceClass"),
        stringProperty(window, "resourceName"),
        stringProperty(window, "desktopFileName"),
        numberProperty(window, "pid"),
        boolProperty(window, "normalWindow"),
        stringProperty(window, "internalId")
    );
}

function connectIfPresent(window, signalName) {
    try {
        const signal = window[signalName];
        if (!signal || !signal.connect) return;
        signal.connect(function() {
            if (workspace.activeWindow === window) publish(window);
        });
    } catch (error) {
    }
}

function watchWindow(window) {
    if (!window || watchedWindows.has(window)) return;
    watchedWindows.set(window, true);

    connectIfPresent(window, "captionChanged");
    connectIfPresent(window, "windowClassChanged");
    connectIfPresent(window, "desktopFileNameChanged");

    try {
        if (window.closed && window.closed.connect) {
            window.closed.connect(function() {
                watchedWindows.delete(window);
            });
        }
    } catch (error) {
    }
}

function publishActive(window) {
    watchWindow(window);
    publish(window);
}

function main() {
    log("started");
    publishActive(workspace.activeWindow);
    workspace.windowActivated.connect(function(window) {
        publishActive(window);
    });
}

main();
