#!/usr/bin/env python3
"""Install the pinned, user-selected redscript mod; never overwrite other mods.

Usage: python3 install-cyberpunk-no-intro.py '/path/to/Cyberpunk 2077'
Close the game first. Re-running is safe and verifies installed file hashes.
"""
import argparse
import hashlib
import io
from pathlib import Path
import urllib.request
import zipfile

REDSCRIPT_URL = 'https://github.com/jac3km4/redscript/releases/download/v0.5.31/redscript-v0.5.31-windows.zip'
REDSCRIPT_SHA = '799bbd88863f6728616f8d723c941ea15dff71ed5bbfd50c525f0d39ea0bf46b'
MOD_URL = ('https://raw.githubusercontent.com/djkovrik/CP77Mods/'
           '729fc702e7ec1b1c211ea804a9c9c7a774d33095/'
           'src/No%20Intro%20Videos%20-%20redscript/r6/scripts/NoIntroVideos.reds')
MOD_SHA = '1d305b61765b507efbea24e56423a7bb447b59a533377c3b797d732aa9a4e340'
FILES = ('engine/config/base/scripts.ini', 'engine/tools/scc.exe',
         'engine/tools/scc_lib.dll', 'r6/config/cybercmd/scc.toml')


def download(url, expected):
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if hashlib.sha256(data).hexdigest() != expected:
        raise SystemExit(f'Checksum mismatch: {url}')
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    args = parser.parse_args()
    game = args.game.resolve(strict=True)
    if not (game / 'bin/x64/Cyberpunk2077.exe').is_file():
        raise SystemExit('Not a Cyberpunk 2077 installation')
    for proc in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            if any(arg.lower().endswith(b'cyberpunk2077.exe')
                   for arg in proc.read_bytes().split(b'\0')):
                raise SystemExit('Close Cyberpunk 2077 before installing')
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            continue
    with zipfile.ZipFile(io.BytesIO(download(REDSCRIPT_URL, REDSCRIPT_SHA))) as archive:
        payloads = {name: archive.read(name) for name in FILES}
    payloads['r6/scripts/NoIntroVideos.reds'] = download(MOD_URL, MOD_SHA)
    # Preflight all files before writing any, including symlinks outside game.
    for name, data in payloads.items():
        dest = game / name
        if not dest.resolve().is_relative_to(game):
            raise SystemExit(f'Refusing path outside game: {dest}')
        if dest.exists() and dest.read_bytes() != data:
            raise SystemExit(f'Existing different version, left untouched: {dest}')
    for name, data in payloads.items():
        dest = game / name
        if not dest.exists():
            dest.parent.mkdir(parents=True, exist_ok=True)
            with dest.open('xb') as output:
                output.write(data)
        print('VERIFIED', name, hashlib.sha256(data).hexdigest())


if __name__ == '__main__':
    main()
