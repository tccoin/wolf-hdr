#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('heroic_profile', ROOT / 'docker/heroic-profile-init.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class HeroicProfile(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.home = Path(self.temp.name) / 'home'
        self.shared = Path(self.temp.name) / 'shared'
        self.home.mkdir()

    def shared_dirs(self):
        for name in ('.config/heroic', '.config/legendary', '.local/share/umu', 'Games/Heroic'):
            (self.shared / name).mkdir(parents=True)

    def test_fresh_host(self):
        module.prepare(self.home, self.shared)
        self.assertEqual(list(self.home.iterdir()), [])

    def test_shared_profile_and_idempotence(self):
        self.shared_dirs()
        (self.home / '.config/heroic').mkdir(parents=True)
        module.prepare(self.home, self.shared)
        module.prepare(self.home, self.shared)
        for name in ('.config/heroic', '.config/legendary', '.local/share/umu', 'Games/Heroic'):
            self.assertEqual((self.home / name).resolve(), (self.shared / name).resolve())

    def test_conflict_preserves_all_data(self):
        self.shared_dirs()
        target = self.home / 'Games/Heroic'
        target.mkdir(parents=True)
        marker = target / 'keep.txt'
        marker.write_text('existing game')
        with self.assertRaises(RuntimeError):
            module.prepare(self.home, self.shared)
        self.assertEqual(marker.read_text(), 'existing game')
        self.assertFalse((self.home / '.config/heroic').exists())

    def test_different_symlink_is_not_replaced(self):
        self.shared_dirs()
        target = self.home / '.config/heroic'
        target.parent.mkdir()
        target.symlink_to(self.home / 'unrelated')
        with self.assertRaises(RuntimeError):
            module.prepare(self.home, self.shared)
        self.assertEqual(target.readlink(), self.home / 'unrelated')


if __name__ == '__main__':
    unittest.main()
