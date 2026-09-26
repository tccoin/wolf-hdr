#!/usr/bin/env python3
"""Apply the KDE Steam game wrapper to installed apps before Steam starts.

Steam has no global Linux launch-option setting. This utility safely writes the
equivalent default only for installed applications, preserving every existing
option after %command%. It never runs while Steam is present, and keeps the
unmodified value in a private state file for recovery.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import tempfile

WRAPPER = '/usr/local/bin/wolf-steam-game'
STATE_VERSION = 1
TOKEN = re.compile(r'''\s+|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]''')


def unquote(token: str) -> str:
    return bytes(token[1:-1], 'utf-8').decode('unicode_escape')


def quote(value: str) -> str:
    return '"' + value.replace('\\', '\\\\').replace('"', '\\"') + '"'


class Pair:
    def __init__(self, key: str, value: str | None, start: int, value_start: int, value_end: int,
                 children: list['Pair'] | None = None, close: int | None = None):
        self.key, self.value, self.start = key, value, start
        self.value_start, self.value_end, self.children, self.close = value_start, value_end, children, close


def tokens(text: str):
    pos = 0
    for match in TOKEN.finditer(text):
        if match.start() != pos:
            raise ValueError(f'Unsupported VDF syntax at byte {pos}')
        pos = match.end()
        token = match.group()
        if token.isspace() or token.startswith('//'):
            continue
        yield token, match.start(), match.end()
    if text[pos:].strip():
        raise ValueError(f'Unsupported VDF syntax at byte {pos}')


def parse(text: str) -> list[Pair]:
    items = list(tokens(text))
    index = 0

    def object_(closing: bool = False) -> tuple[list[Pair], int | None]:
        nonlocal index
        result = []
        while index < len(items):
            token, start, end = items[index]
            if token == '}':
                if not closing:
                    raise ValueError('Unexpected VDF closing brace')
                index += 1
                return result, start
            if token in '{}':
                raise ValueError('Expected VDF key')
            key = unquote(token)
            index += 1
            if index == len(items):
                raise ValueError('Missing VDF value')
            value, value_start, value_end = items[index]
            index += 1
            if value == '{':
                children, close = object_(True)
                result.append(Pair(key, None, start, value_start, value_end, children, close))
            elif value.startswith('"'):
                result.append(Pair(key, unquote(value), start, value_start, value_end))
            else:
                raise ValueError('Expected VDF string or block')
        if closing:
            raise ValueError('Unterminated VDF block')
        return result, None

    result, _ = object_()
    return result


def child(pairs: list[Pair], key: str) -> Pair | None:
    return next((pair for pair in pairs if pair.key == key), None)


def installed_game_ids(steam: Path) -> set[str]:
    libraries = {steam}
    folders = steam / 'steamapps/libraryfolders.vdf'
    if folders.is_file():
        for value in re.findall(r'"path"\s+"((?:\\.|[^"\\])*)"', folders.read_text(errors='replace')):
            libraries.add(Path(bytes(value, 'utf-8').decode('unicode_escape')))
    manifests = (manifest for library in libraries for manifest in library.glob('steamapps/appmanifest_*.acf'))
    app_ids = set()
    for manifest in manifests:
        match = re.fullmatch(r'appmanifest_(\d+)\.acf', manifest.name)
        text = manifest.read_text(errors='replace') if manifest.is_file() else ''
        name = re.search(r'"name"\s+"([^"]*)"', text)
        install_dir = re.search(r'"installdir"\s+"([^"]*)"', text)
        # Runtime components have manifests too, but Steam owns their startup.
        component = (name.group(1) if name else '') + '\n' + (install_dir.group(1) if install_dir else '')
        if match and not re.search(r'(?i)proton|steam linux runtime|steamlinuxruntime|redistributables|steamworks shared', component):
            app_ids.add(match.group(1))
    return app_ids


def state_path(steam: Path) -> Path:
    return steam / 'wolf-kde-game-defaults.json'


def read_state(path: Path) -> dict:
    if not path.exists():
        return {'version': STATE_VERSION, 'original': {}}
    data = json.loads(path.read_text())
    if data.get('version') != STATE_VERSION or not isinstance(data.get('original'), dict):
        raise ValueError('Unknown Wolf Steam defaults state; refusing to replace launch options')
    return data


def write_atomic(path: Path, data: str, mode: int = 0o600) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix='.wolf-', delete=False) as stream:
        temporary = Path(stream.name)
        try:
            os.chmod(temporary, mode)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def wrapper_option(app_id: str, original: str) -> str:
    suffix = (' ' + original) if original else ''
    return f'{WRAPPER} --appid {app_id} -- %command%{suffix}'


def reconcile(config: Path, app_ids: set[str], state: dict) -> tuple[bool, int]:
    text = config.read_text()
    root = parse(text)
    apps = next((pair for pair in root if pair.children and pair.key == 'UserLocalConfigStore'), None)
    for part in ('Software', 'Valve', 'Steam', 'apps'):
        apps = child(apps.children, part) if apps and apps.children else None
    if not apps or not apps.children:
        raise ValueError(f'No Steam apps block in {config}')
    replacements: list[tuple[int, int, str]] = []
    updated = 0
    configured = set()
    for app in apps.children:
        if not app.children:
            continue
        existing = child(app.children, 'LaunchOptions')
        original = existing.value if existing else ''
        state_key = f'{config}:{app.key}'
        if app.key not in app_ids:
            if existing and original.startswith(f'{WRAPPER} --appid {app.key} -- %command%'):
                replacements.append((existing.value_start, existing.value_end,
                                     quote(state['original'].get(state_key, ''))))
                updated += 1
            continue
        configured.add(app.key)
        expected = wrapper_option(app.key, original)
        if original.startswith(f'{WRAPPER} --appid {app.key} -- %command%'):
            continue
        state['original'].setdefault(state_key, original)
        encoded = quote(expected)
        if existing:
            replacements.append((existing.value_start, existing.value_end, encoded))
        else:
            # Replace the existing closing-brace indentation so it isn't
            # duplicated when the new entry is placed on the preceding line.
            line_start = text.rfind('\n', 0, app.close) + 1
            indent = text[line_start:app.close]
            if not indent.isspace():
                raise ValueError(f'Unexpected VDF closing-brace layout for app {app.key}')
            replacements.append((line_start, app.close,
                                 f'{indent}\t"LaunchOptions"\t\t{encoded}\n{indent}'))
        updated += 1
    missing = sorted(app_ids - configured, key=int)
    if missing:
        line_start = text.rfind('\n', 0, apps.close) + 1
        indent = text[line_start:apps.close]
        if not indent.isspace():
            raise ValueError('Unexpected Steam apps-block closing-brace layout')
        entries = []
        for app_id in missing:
            state_key = f'{config}:{app_id}'
            state['original'].setdefault(state_key, '')
            entries.append(f'{indent}\t"{app_id}"\n{indent}\t{{\n{indent}\t\t"LaunchOptions"\t\t'
                           f'{quote(wrapper_option(app_id, ""))}\n{indent}\t}}')
        replacements.append((line_start, apps.close, '\n'.join(entries) + '\n' + indent))
        updated += len(missing)
    if not replacements:
        return False, 0
    # Non-overlapping edits are applied back-to-front.
    for start, end, value in sorted(replacements, reverse=True):
        text = text[:start] + value + text[end:]
    write_atomic(config, text)
    return True, updated


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--steam-root', type=Path, default=Path.home() / '.steam/steam')
    args = parser.parse_args()
    steam = args.steam_root.resolve()
    steam_running = False
    for comm in Path('/proc').glob('[0-9]*/comm'):
        try:
            if comm.read_text(errors='ignore').strip() != 'steam':
                continue
            status = (comm.parent / 'status').read_text(errors='ignore')
        except OSError:
            continue
        # A terminated process can remain as a zombie until its parent reaps
        # it. It no longer owns Steam's profile or IPC and must not block a
        # fresh launch after a KDE tile restart.
        state_line = next((line for line in status.splitlines() if line.startswith('State:')), '')
        if state_line.split()[1:2] not in (['Z'], ['X']):
            steam_running = True
            break
    if steam_running:
        raise SystemExit('Steam is running; defaults are applied only before it starts')
    app_ids = installed_game_ids(steam)
    state_file = state_path(steam)
    state = read_state(state_file)
    configs = sorted(steam.glob('userdata/*/config/localconfig.vdf'))
    changed = total = 0
    for config in configs:
        did_change, count = reconcile(config, app_ids, state)
        changed += did_change
        total += count
    if changed or not state_file.exists():
        write_atomic(state_file, json.dumps(state, indent=2) + '\n')
    print(f'Wolf Steam HDR defaults: {total} installed game entries updated across {changed} account config(s).')


if __name__ == '__main__':
    main()
