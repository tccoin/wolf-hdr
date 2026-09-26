#!/usr/bin/env python3
"""Run inside KDE: Heroic PID [--game-wrapper] [--hdr-probe /path/to/probe.exe].

Checks existing Proton profiles with winepath, never a game executable. Wine
may initialize/update its normal prefix metadata, just as Heroic's preflight
does. Account configuration and game launch settings are not modified.
"""
import json
import argparse
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('pid', type=int)
parser.add_argument('--game-wrapper', action='store_true')
parser.add_argument('--hdr-probe', type=Path)
args = parser.parse_args()
pid = args.pid
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
        command = [str(umu), 'winepath', '-u', r'C:\windows\command\EpicGamesLauncher.exe']
        if args.hdr_probe:
            assert args.hdr_probe.is_file(), 'Build/copy the Windows HDR probe first'
            game_env['PRESSURE_VESSEL_FILESYSTEMS_RW'] = ':'.join(filter(None, [
                game_env.get('PRESSURE_VESSEL_FILESYSTEMS_RW', ''), str(args.hdr_probe.resolve())]))
            # Proton's /unix launcher can detach a console executable and hide
            # its exit status/output. cmd /c keeps both attached for assertions.
            command = [str(umu), 'cmd', '/c', 'Z:' + str(args.hdr_probe.resolve()).replace('/', '\\'), '1']
        if args.game_wrapper:
            command.insert(0, '/usr/local/bin/wolf-heroic-game')
        result = subprocess.run(command,
                                env=game_env, capture_output=True, text=True, timeout=60)
        count += 1
        assert result.returncode == 0, f'Proton profile #{count} failed ({result.returncode}); inspect runtime logs'
        if args.hdr_probe:
            assert 'supported=1 enabled=1 bits=10' in result.stdout, f'Profile #{count}: Windows HDR capability missing'
            print(f'PASS Proton profile #{count}: Windows Advanced Color supported=1 enabled=1 bits=10', flush=True)
        else:
            assert 'EpicGamesLauncher.exe' in result.stdout, f'Profile #{count}: winepath destination missing'
            print(f'PASS Proton profile #{count}: UMU starts and Epic launcher destination resolves', flush=True)
assert count, 'No configured Proton profiles to verify'
print('PASS existing Heroic Proton profiles; no game was launched')
