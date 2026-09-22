#!/usr/bin/env python3
"""Conservative mode-switch guard; print app IDs, never process environments."""
import os
import sys
from pathlib import Path

# Inspect all readable same-user environments (including reparented Wine
# children), but fail closed on unreadable live descendants of this Steam.
# KWin and unrelated desktop services may intentionally deny /proc access.
parents = {}
for process in Path("/proc").iterdir():
    if not process.name.isdecimal():
        continue
    try:
        status = dict(line.split(":", 1) for line in process.joinpath("status").read_text().splitlines())
        if status["State"].strip().startswith("Z"):
            continue
        parents[int(process.name)] = int(status["PPid"])
    except (FileNotFoundError, ProcessLookupError):
        continue
descendants = {int(sys.argv[1])} if len(sys.argv) > 1 else set()
while True:
    expanded = descendants | {pid for pid, parent in parents.items() if parent in descendants}
    if expanded == descendants:
        break
    descendants = expanded

games = set()
for process in Path("/proc").iterdir():
    if not process.name.isdecimal() or int(process.name) not in parents:
        continue
    try:
        if process.stat().st_uid != os.getuid():
            continue
        environment = process.joinpath("environ").read_bytes().split(b"\0")
        for item in environment:
            key, _, value = item.partition(b"=")
            if key in (b"SteamAppId", b"SteamGameId") and value.isdigit():
                if int(value) not in (0, 7, 769):
                    games.add(value.decode("ascii"))
    except (FileNotFoundError, ProcessLookupError):
        continue
    except PermissionError:
        if int(process.name) in descendants:
            raise
print(", ".join(sorted(games)))
