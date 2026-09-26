#!/usr/bin/env python3
"""Profile migration fixtures only; never opens the real Heroic profile."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('defaults', ROOT / 'docker/heroic-game-defaults.py')
defaults = importlib.util.module_from_spec(spec)
spec.loader.exec_module(defaults)


class GameDefaults(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.profile = Path(self.temp.name)
        (self.profile / 'GamesConfig').mkdir()

    def write(self, name, data):
        path = self.profile / name
        path.write_text(json.dumps(data))
        return path

    def test_existing_settings_and_backups(self):
        custom = {'exe': 'custom-tool', 'args': '"two words"'}
        config = self.write('config.json', {'version': 'v0', 'defaultSettings': {'wrapperOptions': [custom], 'language': 'en'}})
        game = self.write('GamesConfig/game.json', {'version': 'v0', 'game': {'wrapperOptions': [], 'winePrefix': '/games/prefix', 'enableHDR': False}})
        inherited = self.write('GamesConfig/inherited.json', {'inherited': {'winePrefix': '/other'}})
        originals = {p: p.read_bytes() for p in (config, game, inherited)}
        defaults.prepare(self.profile)
        settings = json.loads(config.read_text())['defaultSettings']
        self.assertEqual(settings['wrapperOptions'], [{'exe': defaults.WRAPPER, 'args': ''}, custom])
        self.assertEqual(settings['language'], 'en')
        self.assertEqual(json.loads(game.read_text())['game']['winePrefix'], '/games/prefix')
        self.assertFalse(json.loads(game.read_text())['game']['enableHDR'])
        self.assertEqual(inherited.read_bytes(), originals[inherited])
        for path in (config, game):
            backup = self.profile / 'wolf-game-wrapper-backup' / path.relative_to(self.profile)
            self.assertEqual(backup.read_bytes(), originals[path])
            self.assertEqual(backup.stat().st_mode & 0o777, 0o600)
        before = {p: p.read_bytes() for p in (config, game)}
        defaults.prepare(self.profile)
        self.assertEqual(before, {p: p.read_bytes() for p in (config, game)})

    def test_new_profile_defaults(self):
        defaults.prepare(self.profile)
        config = json.loads((self.profile / 'config.json').read_text())
        self.assertEqual(config['version'], 'v0')
        self.assertEqual(config['defaultSettings']['wrapperOptions'][0]['exe'], defaults.WRAPPER)

    def test_invalid_game_does_not_partially_update_defaults(self):
        config = self.write('config.json', {'defaultSettings': {'wrapperOptions': []}})
        self.write('GamesConfig/game.json', {'game': {'wrapperOptions': 'invalid'}})
        original = config.read_bytes()
        with self.assertRaises(ValueError):
            defaults.prepare(self.profile)
        self.assertEqual(config.read_bytes(), original)
        self.assertFalse((self.profile / 'wolf-game-wrapper-backup').exists())

    def test_wrapper_sdr_opt_out_and_nested_passthrough(self):
        for overrides in ({'WOLF_KDE_ENABLE_HDR': '0'},
                          {'WOLF_KDE_ENABLE_HDR': '1', 'WOLF_HEROIC_GAME_HDR': '0'},
                          {'WOLF_KDE_ENABLE_HDR': '1', 'GAMESCOPE_WAYLAND_DISPLAY': 'existing'}):
            env = dict(os.environ, **overrides)
            result = subprocess.run(['bash', str(ROOT / 'docker/wolf-heroic-game.sh'),
                                     'python3', '-c', 'import sys; assert sys.argv[1:] == ["space arg", ""]; sys.exit(23)',
                                     'space arg', ''], env=env)
            self.assertEqual(result.returncode, 23)


if __name__ == '__main__':
    unittest.main()
