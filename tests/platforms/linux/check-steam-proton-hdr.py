#!/usr/bin/env python3
"""Check Steam's early HDR flags and Proton's Windows API with a fresh prefix.

Run inside KDE as its desktop user. Requires the optional wine-hdr-probe
artifact and Heroic's installed UMU runtime. Does not launch Steam or a game,
and never opens Steam's existing compatdata. The temporary prefix is retained
for inspection. This is a Proton/API test, not a Steam gameplay test.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('steam_pid', type=int)
parser.add_argument('--proton', required=True, type=Path)
parser.add_argument('--hdr-probe', required=True, type=Path)
args = parser.parse_args()
env = dict(item.split('=', 1) for item in Path(f'/proc/{args.steam_pid}/environ').read_text().split('\0') if '=' in item)
assert env.get('DXVK_HDR') == '1' and env.get('PROTON_ENABLE_HDR') == '1', 'Steam is missing early Wine HDR flags'
assert (args.proton / 'proton').is_file() and args.hdr_probe.is_file()
home = Path(env['HOME'])
umu = home / '.config/heroic/tools/runtimes/umu/umu-run'
assert umu.is_file()
prefix = Path(tempfile.mkdtemp(prefix='wolf-steam-hdr-api-'))
print(f'Isolated test prefix retained: {prefix}', flush=True)
mounts = [env.get('PRESSURE_VESSEL_FILESYSTEMS_RW', ''), str(args.proton.resolve()),
          str((home / '.local/share/umu').resolve()), str(args.hdr_probe.resolve())]
env.update(BWRAP='/usr/bin/bwrap', PRESSURE_VESSEL_FILESYSTEMS_RW=':'.join(filter(None, mounts)),
           WINEPREFIX=str(prefix), PROTONPATH=str(args.proton), GAMEID='umu-default', STORE='none',
           PROTONFIXES_DISABLE='1', PROTON_LOG='0', STEAM_COMPAT_INSTALL_PATH=str(args.hdr_probe.parent),
           ENABLE_GAMESCOPE_WSI='1', ENABLE_HDR_WSI='1', GAMESCOPE_WSI_OVERLAY_BOOTSTRAP='0',
           WAYLAND_DISPLAY='wayland-kde')
for key in ('DBUS_SYSTEM_BUS_ADDRESS', 'LD_LIBRARY_PATH', 'LD_PRELOAD', 'STEAM_RUNTIME',
            'GAMESCOPE_WAYLAND_DISPLAY', 'VK_INSTANCE_LAYERS'):
    env.pop(key, None)
command = ['/usr/games/gamescope', '--backend', 'wayland', '-e', '-f', '--hdr-enabled',
           '--virtual-connector-strategy', 'SingleApplication', '--hdr-sdr-content-nits', '100',
           '-W', '1280', '-H', '720', '--', 'env', 'VK_INSTANCE_LAYERS=VK_LAYER_FROG_gamescope_wsi_x86_64',
           str(umu), str(args.hdr_probe), '1', r'C:\wolf-hdr-api.txt']
result = subprocess.run(command, env=env, text=True, capture_output=True, timeout=90)
report = prefix / 'pfx/drive_c/wolf-hdr-api.txt'
assert result.returncode == 0 and report.is_file(), 'Steam Proton API probe did not complete'
text = report.read_text()
assert 'supported=1 enabled=1 bits=10' in text, 'Steam Proton did not expose Windows HDR'
print(text.strip())
print('PASS Steam early HDR flags + Steam Gamescope mode + Proton Windows HDR API')
