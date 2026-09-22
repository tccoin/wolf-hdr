#!/usr/bin/env python3
"""Read-only KWin assertion; arguments: Plasma PID, optional steam/gamescope.

Requires kde-window-state.js at /tmp/wolf-window-state.js. This intentionally
checks the outer Wayland window, not just Steam's private XWayland surface.
"""
import json
import os
import subprocess
import sys
import time

with open(f"/proc/{int(sys.argv[1])}/environ") as source:
    env = dict(item.split("=", 1) for item in source.read().split("\0") if "=" in item)

backend = sys.argv[2] if len(sys.argv) > 2 else "gamescope"

def run(*args):
    return subprocess.check_output(args, env=env, text=True).strip()

for _ in range(30):
    name = f"wolf-window-check-{os.getpid()}"
    sid = run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.loadScript",
              "/tmp/wolf-window-state.js", name)
    try:
        run("qdbus6", "org.kde.KWin", f"/Scripting/Script{sid}", "org.kde.kwin.Script.run")
    finally:
        run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.unloadScript", name)
    lines = run("tail", "-n", "30", "/home/retro/wolf-selkies-kwin.log").splitlines()
    snapshot = [line for line in lines if "WOLF_WINDOW_SNAPSHOT " in line][-1]
    windows = json.loads(snapshot.split("WOLF_WINDOW_SNAPSHOT ", 1)[1])
    visible = [w for w in windows if w["resourceClass"].lower() == backend
               and w["caption"].startswith("Steam") and not w["minimized"] and not w["hidden"]]
    if visible:
        print("PASS Steam window visible in KWin:", json.dumps(visible))
        break
    time.sleep(1)
else:
    raise SystemExit("FAIL: no visible Steam window in KWin within 30 seconds")
