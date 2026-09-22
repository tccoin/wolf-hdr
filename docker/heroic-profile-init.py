#!/usr/bin/env python3
"""Link an optional existing Heroic home without overwriting KDE/user data."""
import os
from pathlib import Path
import sys


def prepare(home: Path, shared: Path) -> None:
    if not shared.is_dir():
        return  # A new host gets a normal, independent Heroic profile.
    pairs = []
    for relative in ('.config/heroic', '.config/legendary', '.local/share/umu', 'Games/Heroic'):
        source, target = shared / relative, home / relative
        if not source.is_dir():
            continue
        if target.is_symlink():
            if target.resolve() != source.resolve():
                raise RuntimeError(f'Refusing to replace existing link: {target}')
            continue
        if target.exists() and (not target.is_dir() or any(target.iterdir())):
            raise RuntimeError(f'Refusing to replace existing data: {target}')
        pairs.append((source, target))
    # Validate every target before changing any of them.
    for source, target in pairs:
        target.parent.mkdir(parents=True, exist_ok=True)
        if target.is_dir():
            target.rmdir()  # Only the verified-empty placeholder.
        target.symlink_to(source, target_is_directory=True)


if __name__ == '__main__':
    try:
        prepare(Path.home(), Path(os.environ.get('WOLF_HEROIC_SHARED_HOME', '/var/lib/wolf-shared-heroic')))
    except (OSError, RuntimeError) as error:
        print(f'Heroic profile: {error}', file=sys.stderr)
        sys.exit(1)
