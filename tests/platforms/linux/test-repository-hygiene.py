#!/usr/bin/env python3
"""Check publishable files, not ignored runtime profiles or Git metadata.

This is a portability guard, not a replacement for a secret scanner.
The fixed /home/retro and /home/ubuntu paths belong to the container ABI.
"""
from pathlib import Path
import re
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]


class RepositoryHygiene(unittest.TestCase):
    def test_no_workstation_home_paths(self):
        paths = subprocess.check_output([
            'git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z',
        ], cwd=ROOT).decode().split('\0')
        pattern = re.compile(r'/home/(?!retro\b|ubuntu\b)[\w.-]+|/Users/[\w.-]+')
        failures = []
        for name in filter(None, paths):
            path = ROOT / name
            if not path.is_file():
                continue
            data = path.read_bytes()
            if b'\0' in data:
                continue
            for number, line in enumerate(data.decode(errors='replace').splitlines(), 1):
                if pattern.search(line):
                    # Do not echo a potentially sensitive match into CI logs.
                    failures.append(f'{name}:{number}')
        self.assertEqual(failures, [], 'Host-specific home paths: ' + ', '.join(failures))

    def test_runtime_secrets_are_ignored(self):
        samples = ['.env', 'deployment.key', 'deployment.pem', 'wolf-hdr-config.toml',
                   'profile-data/account.json', 'heroic-data/.config/heroic/store.json']
        for sample in samples:
            result = subprocess.run(['git', 'check-ignore', '--no-index', '-q', sample], cwd=ROOT)
            self.assertEqual(result.returncode, 0, sample)


if __name__ == '__main__':
    unittest.main()
