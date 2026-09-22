#!/usr/bin/env python3
"""Source-only regression checks for the self-contained HDR build graph."""
from pathlib import Path
import re
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[3]


class BuildLayout(unittest.TestCase):
    def test_public_roots(self):
        for name in ('kde-hdr.Dockerfile', 'wolf.nvcodec-runtime.Dockerfile'):
            text = (ROOT / 'docker' / name).read_text()
            args = dict(re.findall(r'^ARG (\w+)=(.+)$', text, re.M))
            stages = set()
            for line in text.splitlines():
                if not line.startswith('FROM '):
                    continue
                parts = line.split()
                image = re.sub(r'\$\{(\w+)\}', lambda m: args[m[1]], parts[1])
                self.assertTrue(image == 'scratch' or image in stages or re.fullmatch(
                    r'ghcr\.io/[^\s]+@sha256:[0-9a-f]{64}', image), image)
                if len(parts) == 4:
                    stages.add(parts[3])
            self.assertNotRegex(text, r'/home/(?!retro(?:/|\b)|ubuntu(?:/|\b))[^\s/]+')
            for line in re.sub(r'\\\n\s*', ' ', text).splitlines():
                if not line.startswith('COPY ') or '--from=' in line:
                    continue
                for source in [p for p in line.split()[1:-1] if not p.startswith('--')]:
                    self.assertTrue(list(ROOT.glob(source)), f'{name}: missing {source}')

    def test_dependency_lock(self):
        with (ROOT / 'third_party/gst-wayland-display/Cargo.lock').open('rb') as stream:
            lock = tomllib.load(stream)
        self.assertTrue(lock['package'])
        self.assertIn('!third_party/gst-wayland-display/Cargo.lock',
                      (ROOT / '.gitignore').read_text())

    def test_patch_failures_abort(self):
        # `set -e` alone is suppressed inside a loop used on the left of &&.
        # An early failed patch must not be hidden by a later successful one.
        text = (ROOT / 'docker/kde-hdr.Dockerfile').read_text()
        commands = [line for line in text.splitlines()
                    if 'patch -p1 <' in line or 'git apply /tmp/' in line]
        self.assertEqual(len(commands), 2)
        self.assertTrue(all('|| exit 1' in line for line in commands))

    def test_default_kde_is_portable(self):
        text = (ROOT / 'src/moonlight-server/state/default/config.include.toml').read_text()
        data = tomllib.loads(text.split('R"for_c++_include(', 1)[1].rsplit(')for_c++_include"', 1)[0])
        apps = [app for profile in data['profiles'] for app in profile['apps']]
        app = next(app for app in apps if app['title'] == 'KDE Plasma HDR')
        self.assertEqual(app['runner']['image'], 'wolf-kde:hdr')
        self.assertEqual(app['runner']['mounts'], [])
        self.assertTrue(app['support_hdr'])


if __name__ == '__main__':
    unittest.main()
