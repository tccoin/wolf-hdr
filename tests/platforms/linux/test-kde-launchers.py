#!/usr/bin/env python3
"""Source regression guards for portable desktop launchers."""
import configparser
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class DesktopLaunchers(unittest.TestCase):
    def test_english_default_labels(self):
        files = list((ROOT / 'docker/kde').glob('*.desktop'))
        self.assertEqual(len(files), 4)
        for path in files:
            config = configparser.ConfigParser(interpolation=None)
            config.read(path)
            for section in config.values():
                for key in ('Name', 'Comment'):
                    if key in section:
                        self.assertTrue(section[key].isascii(), f'{path.name}: {key}')

    def test_proton_does_not_inherit_appliance_system_bus(self):
        for name in ('wolf-kde-heroic.sh', 'wolf-kde-steam.sh'):
            text = (ROOT / 'docker' / name).read_text()
            cleanup = text.index('unset DBUS_SYSTEM_BUS_ADDRESS')
            first_launch = text.index('/opt/Heroic/heroic') if 'heroic' in name else text.index('"$steam"')
            self.assertLess(cleanup, first_launch)
            self.assertNotIn('unset DBUS_SESSION_BUS_ADDRESS', text)
            self.assertIn('export BWRAP=/usr/bin/bwrap', text)

    def test_heroic_shared_mount_is_visible_to_proton(self):
        text = (ROOT / 'docker/wolf-kde-heroic.sh').read_text()
        self.assertIn('WOLF_HEROIC_SHARED_HOME:-/var/lib/wolf-shared-heroic', text)
        self.assertIn('export PRESSURE_VESSEL_FILESYSTEMS_RW=', text)
        self.assertLess(text.index('export PRESSURE_VESSEL_FILESYSTEMS_RW='),
                        text.index('/opt/Heroic/heroic'))


if __name__ == '__main__':
    unittest.main()
