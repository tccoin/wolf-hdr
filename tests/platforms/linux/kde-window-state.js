// Read-only diagnostic; one snapshot goes to wolf-selkies-kwin.log.
print("WOLF_WINDOW_SNAPSHOT " + JSON.stringify(workspace.windowList().map(w => ({
        caption: w.caption, pid: w.pid, resourceClass: w.resourceClass,
        minimized: w.minimized, active: w.active, hidden: w.hidden,
        unresponsive: w.unresponsive, opacity: w.opacity,
        geometry: [w.frameGeometry.x, w.frameGeometry.y,
                   w.frameGeometry.width, w.frameGeometry.height]
    }))));
