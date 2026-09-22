// Restore the single Steam client, native or wrapped in Gamescope.
// Inner X11 activation alone cannot restore an outer Wayland window.
for (const w of workspace.windowList()) {
    if (String(w.resourceClass) === "gamescope" ||
        (String(w.resourceClass).toLowerCase() === "steam" &&
         String(w.caption) === "Steam")) {
        w.minimized = false;
        if (w.desktops.length > 0)
            workspace.currentDesktop = w.desktops[0];
        workspace.activeWindow = w;
    }
}
