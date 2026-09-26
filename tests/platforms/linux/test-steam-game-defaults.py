#!/usr/bin/env python3
"""Fixtures for Steam's automatic desktop game-wrapper defaults."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('steam_defaults', ROOT / 'docker/steam-game-defaults.py')
defaults = importlib.util.module_from_spec(spec)
spec.loader.exec_module(defaults)


class SteamGameDefaults(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.steam = Path(self.temp.name) / '.steam/steam'
        self.config = self.steam / 'userdata/1/config/localconfig.vdf'
        self.config.parent.mkdir(parents=True)
        appdir = self.steam / 'steamapps'
        appdir.mkdir()
        for app_id, name in (('10', 'Playable Game'), ('20', 'Other Game'), ('40', 'New Game'),
                             ('50', 'Steam Linux Runtime 4.0')):
            (appdir / f'appmanifest_{app_id}.acf').write_text(
                f'"AppState" {{ "name" "{name}" "installdir" "{name}" }}')
        self.config.write_text('''"UserLocalConfigStore"
{
  "Software"
  {
    "Valve"
    {
      "Steam"
      {
        "apps"
        {
          "10"
          {
            "LaunchOptions" "--preserve-me"
          }
          "20"
          {
            "Playtime" "7"
          }
          "30"
          {
            "LaunchOptions" "--not-installed"
          }
        }
      }
    }
  }
}
''')

    def test_reconcile_preserves_existing_options_and_ignores_uninstalled(self):
        state = {'version': defaults.STATE_VERSION, 'original': {}}
        changed, count = defaults.reconcile(self.config, defaults.installed_game_ids(self.steam), state)
        self.assertTrue(changed)
        self.assertEqual(count, 3)
        text = self.config.read_text()
        self.assertIn(f'{defaults.WRAPPER} --appid 10 -- %command% --preserve-me', text)
        self.assertIn(f'{defaults.WRAPPER} --appid 20 -- %command%', text)
        self.assertIn(f'{defaults.WRAPPER} --appid 40 -- %command%', text)
        self.assertIn('"30"\n          {\n            "LaunchOptions" "--not-installed"', text)
        self.assertEqual(len(state['original']), 3)
        changed, count = defaults.reconcile(self.config, defaults.installed_game_ids(self.steam), state)
        self.assertFalse(changed)
        self.assertEqual(count, 0)

    def test_restores_a_previously_wrapped_steam_component(self):
        self.config.write_text(self.config.read_text().replace(
            '"LaunchOptions" "--not-installed"',
            f'"LaunchOptions" "{defaults.WRAPPER} --appid 30 -- %command%"'))
        state = {'version': defaults.STATE_VERSION, 'original': {f'{self.config}:30': '--not-installed'}}
        changed, count = defaults.reconcile(self.config, defaults.installed_game_ids(self.steam), state)
        self.assertTrue(changed)
        self.assertEqual(count, 4)  # three games plus the stale component entry
        self.assertIn('"LaunchOptions" "--not-installed"', self.config.read_text())

    def test_state_requires_known_format(self):
        path = defaults.state_path(self.steam)
        path.write_text(json.dumps({'version': 99, 'original': {}}))
        with self.assertRaises(ValueError):
            defaults.read_state(path)


if __name__ == '__main__':
    unittest.main()
