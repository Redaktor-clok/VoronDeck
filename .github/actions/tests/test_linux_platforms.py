"""Exercise the build entry points without compiling or deploying anything."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which('c++'), 'C++ preprocessor required')
class ShippingAmsProfile(unittest.TestCase):
    def test_vendor_adapters_cannot_be_enabled(self):
        result = subprocess.run(
            ['c++', '-E', '-dM', '-x', 'c++', '-include', 'include/helix_features.h', '-'],
            input='', cwd=ROOT, text=True, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        macros = dict(line.split()[1:3] for line in result.stdout.splitlines()
                      if line.startswith('#define HELIX_HAS_'))
        for name in ('CFS', 'IFS', 'QIDI', 'SNAPMAKER', 'ACE'):
            with self.subTest(name=name):
                self.assertNotIn('HELIX_HAS_' + name, macros)
        for stem in ('ace', 'multiace', 'ad5x_ifs', 'cfs', 'qidi', 'snapmaker'):
            with self.subTest(adapter=stem):
                self.assertFalse((ROOT / 'src/printer' / ('ams_backend_' + stem + '.cpp')).exists())
                self.assertFalse((ROOT / 'include' / ('ams_backend_' + stem + '.h')).exists())


@unittest.skipUnless(shutil.which('make'), 'GNU make required')
class LinuxPlatforms(unittest.TestCase):
    def probe(self, platform='native', goal='probe'):
        return subprocess.run(
            ['make', '-s', '-f', '-', 'PLATFORM_TARGET=' + platform, goal],
            input='include mk/cross.mk\nprobe:\n\t@echo linux-profile-ok\n',
            cwd=ROOT, text=True, capture_output=True, timeout=30)

    def test_general_linux_targets_remain_available(self):
        for platform in ('native', 'pi', 'pi-fbdev', 'pi-both', 'pi32',
                         'pi32-fbdev', 'pi32-both', 'x86', 'x86-fbdev',
                         'x86-both', 'x86-desktop', 'yocto'):
            with self.subTest(platform=platform):
                result = self.probe(platform)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('linux-profile-ok', result.stdout)

    def test_vendor_platforms_are_rejected_before_build(self):
        for platform in ('ad5m', 'ad5m-br', 'ad5x', 'mips', 'k1', 'k1-dynamic',
                         'k2', 'cc1', 'snapmaker-u1', 'qidi'):
            with self.subTest(platform=platform):
                result = self.probe(platform)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Unsupported VoronDeck platform', result.stderr)

    def test_vendor_shortcuts_cannot_build_or_deploy(self):
        for goal in ('k1-docker', 'ad5x-docker', 'release-k2', 'package-snapmaker-u1',
                     'deploy-k1-bin', 'deploy-ad5m', 'cc1-test', 'creator5-docker'):
            with self.subTest(goal=goal):
                result = self.probe(goal=goal)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('Unsupported vendor targets', result.stderr)


@unittest.skipUnless(shutil.which('sh'), 'POSIX shell required')
class InstallerAlias(unittest.TestCase):
    def test_alias_uses_local_vorondeck_installer_and_preserves_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / 'scripts').mkdir()
            shutil.copyfile(ROOT / 'scripts/install.sh', root / 'scripts/install.sh')
            (root / 'install.sh').write_text('#!/bin/sh\nprintf "[%s]\\n" "$@"\n')
            result = subprocess.run(['sh', str(root / 'scripts/install.sh'),
                                     '--language', 'ru', 'argument with spaces'],
                                    text=True, capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, '[--language]\n[ru]\n[argument with spaces]\n')
