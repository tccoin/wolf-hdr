#!/usr/bin/env python3
"""Run inside KDE as its desktop user; argument: the Heroic main-process PID.

Checks existing Proton profiles with winepath, never a game executable. Wine
may initialize/update its normal prefix metadata, just as Heroic's preflight
does. Account configuration and game launch settings are not modified.
"""
import json
from pathlib import Path
import subprocess
import sys

pid = int(sys.argv[1])
env = dict(item.split('=', 1) for item in
           Path(f'/proc/{pid}/environ').read_text().split('\0') if '=' in item)
assert 'DBUS_SYSTEM_BUS_ADDRESS' not in env, 'Stale appliance system bus inherited'
assert env.get('BWRAP') == '/usr/bin/bwrap', 'Wrong bubblewrap helper'
home = Path(env['HOME'])
shared = Path(env.get('WOLF_HEROIC_SHARED_HOME', '/var/lib/wolf-shared-heroic'))
if shared.is_dir():
    assert str(shared.resolve()) in env.get('PRESSURE_VESSEL_FILESYSTEMS_RW', '').split(':')
umu = home / '.config/heroic/tools/runtimes/umu/umu-run'
assert umu.is_file(), 'No existing Heroic UMU runtime'
count = 0
for config in sorted((home / '.config/heroic/GamesConfig').glob('*.json')):
    for settings in json.loads(config.read_text()).values():
        if not isinstance(settings, dict):
            continue
        wine = settings.get('wineVersion', {})
        if wine.get('type') != 'proton' or not settings.get('winePrefix'):
            continue
        game_env = env.copy()
        game_env.update({item['key']: item['value'] for item in settings.get('enviromentOptions', [])})
        game_env.update(PROTONPATH=str(Path(wine['bin']).parent), WINEPREFIX=settings['winePrefix'],
                        GAMEID='umu-default', STORE='egs')
        result = subprocess.run([str(umu), 'winepath', '-u', r'C:\windows\command\EpicGamesLauncher.exe'],
                                env=game_env, capture_output=True, text=True, timeout=60)
        count += 1
        assert result.returncode == 0, f'Proton profile #{count} failed ({result.returncode}); inspect runtime logs'
        assert 'EpicGamesLauncher.exe' in result.stdout, f'Profile #{count}: winepath destination missing'
        print(f'PASS Proton profile #{count}: UMU starts and Epic launcher destination resolves', flush=True)
assert count, 'No configured Proton profiles to verify'
print('PASS existing Heroic Proton profiles; no game was launched')
