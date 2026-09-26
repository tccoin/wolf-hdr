#!/usr/bin/env python3
"""Install Heroic's game-only wrapper while its launcher lifetime lock is held.

Only wrapper lists are changed. Existing tools, accounts, prefixes and per-game
options remain intact. First versions of changed files are backed up privately.
"""
import copy
import json
import os
from pathlib import Path
import tempfile

WRAPPER = '/usr/local/bin/wolf-heroic-game'


def add_wrapper(settings: dict) -> None:
    wrappers = settings.get('wrapperOptions', [])
    if not isinstance(wrappers, list) or any(not isinstance(w, dict) for w in wrappers):
        raise ValueError('Invalid Heroic wrapperOptions; refusing to overwrite settings')
    settings['wrapperOptions'] = [{'exe': WRAPPER, 'args': ''}] + [
        w for w in wrappers if w.get('exe') != WRAPPER
    ]


def prepare(profile: Path) -> None:
    config = profile / 'config.json'
    paths = [config, *sorted((profile / 'GamesConfig').glob('*.json'))]
    updates = []
    # Parse and validate all inputs before making any changes.
    for path in paths:
        original = path.read_bytes() if path.exists() else None
        data = json.loads(original) if original is not None else {'version': 'v0', 'defaultSettings': {}}
        updated = copy.deepcopy(data)
        if path == config:
            add_wrapper(updated.setdefault('defaultSettings', {}))
        else:
            settings = updated.get(path.stem)
            if not isinstance(settings, dict):
                continue  # Ancillary data, not game settings.
            # Missing lists inherit the global setting; explicit lists override it.
            if 'wrapperOptions' in settings:
                add_wrapper(settings)
        if updated != data or original is None:
            updates.append((path, original, updated))
    backup = profile / 'wolf-game-wrapper-backup'
    if updates:
        backup.mkdir(mode=0o700, parents=True, exist_ok=True)
    for path, original, updated in updates:
        if original is not None:
            target = backup / path.relative_to(profile)
            target.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            if not target.exists():
                with target.open('xb') as stream:
                    os.chmod(target, 0o600)
                    stream.write(original)
        with tempfile.NamedTemporaryFile(mode='w', dir=path.parent, prefix='.wolf-game-', delete=False) as stream:
            temp = Path(stream.name)
            try:
                json.dump(updated, stream, indent=2)
                stream.write('\n')
                stream.flush()
                os.fsync(stream.fileno())
                temp.replace(path)
            finally:
                temp.unlink(missing_ok=True)


if __name__ == '__main__':
    prepare(Path.home() / '.config/heroic')
