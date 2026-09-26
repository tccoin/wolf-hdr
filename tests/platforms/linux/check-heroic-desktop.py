#!/usr/bin/env python3
"""Start the native Heroic launcher in a DISPOSABLE KDE test container only.

Arguments: Plasma PID. Never use this against an authenticated user profile.
The container's teardown owns process cleanup; leave Heroic open for the HDR
foreground/capture test that follows. No store account or game is needed.
"""
import json
from pathlib import Path
import subprocess
import sys
import time

env = dict(item.split('=', 1) for item in Path(f'/proc/{int(sys.argv[1])}/environ').read_text().split('\0') if '=' in item)
assert env.get('WOLF_SESSION_ID') == 'regression', 'Only run in the isolated regression container'
with open('/tmp/heroic-desktop-test.log', 'w') as log:
    process = subprocess.Popen(['/usr/local/bin/heroic'], env=env, stdout=log, stderr=log, start_new_session=True)
for _ in range(100):
    assert process.poll() is None, 'Heroic launcher exited; inspect /tmp/heroic-desktop-test.log'
    windows = subprocess.check_output(['xwininfo', '-root', '-tree'], env=env, text=True)
    if '"Heroic Games Launcher"' in windows:
        break
    time.sleep(.2)
else:
    raise AssertionError('Heroic did not create a window on KDE XWayland')
settings = json.loads((Path(env['HOME']) / '.config/heroic/config.json').read_text())['defaultSettings']
assert settings['wrapperOptions'][0]['exe'] == '/usr/local/bin/wolf-heroic-game'
print('PASS native Heroic window on KDE; game-only wrapper defaults loaded')
