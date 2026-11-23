# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('stable_check', Path(__file__).parents[1] / 'scripts/check-stable-package.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


class StablePackage(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / 'source'
        (self.source / 'scripts').mkdir(parents=True)
        (self.source / 'scripts/install-vorondeck.py').write_text(
            'import hashlib,json\n'
            'def verify_package(path,sha,arch):\n'
            ' data=path.read_bytes()\n'
            ' assert hashlib.sha256(data).hexdigest()==sha\n'
            ' return json.loads(data), {"bin/helix-screen":b"binary"}\n')
        self.candidate = self.root / 'candidate'
        (self.candidate / 'packages').mkdir(parents=True)
        self.archive = self.candidate / 'packages/test.tar.gz'
        self.archive.write_text(json.dumps({'version': '1.0.1', 'source_commit': 'a' * 40}))
        self.package = {'path': 'packages/test.tar.gz', 'sha256': hashlib.sha256(self.archive.read_bytes()).hexdigest(),
                        'size': self.archive.stat().st_size, 'source_commit': 'a' * 40,
                        'binary_sha256': hashlib.sha256(b'binary').hexdigest(), 'architecture': 'aarch64', 'libc': 'glibc'}
        self.index = {'schema': 1, 'project': 'VoronDeck', 'channel': 'stable', 'version': '1.0.1',
                      'packages': [self.package]}
        self.write_index()
        (self.candidate / 'SHA256SUMS').write_text(self.package['sha256'] + '  packages/test.tar.gz\n')

    def write_index(self):
        (self.candidate / 'release.json').write_text(json.dumps(self.index))

    def verify(self):
        return check.verify(self.candidate, self.source)

    def test_valid_package_requires_main_ancestry(self):
        with patch.object(check.subprocess, 'run') as run:
            self.assertEqual(self.verify(), '1.0.1')
            self.assertEqual(run.call_args.args[0][-4:], ['merge-base', '--is-ancestor', 'a' * 40, 'HEAD'])

    def test_unmerged_source_is_refused(self):
        with patch.object(check.subprocess, 'run', side_effect=subprocess.CalledProcessError(1, 'git')):
            with self.assertRaises(subprocess.CalledProcessError):
                self.verify()

    def test_test_version_cannot_be_stable(self):
        self.index['version'] = '1.0.2-T.1'
        self.write_index()
        with self.assertRaises(ValueError):
            self.verify()

    def test_package_path_cannot_escape(self):
        self.package['path'] = '../package.tar.gz'
        self.write_index()
        with self.assertRaises(ValueError):
            self.verify()

    def test_size_and_checksum_mismatches_refused(self):
        self.archive.write_bytes(b'corrupt')
        with patch.object(check.subprocess, 'run'), self.assertRaises(ValueError):
            self.verify()

    def test_checksums_file_must_match_index(self):
        (self.candidate / 'SHA256SUMS').write_text('wrong\n')
        with patch.object(check.subprocess, 'run'), self.assertRaises(ValueError):
            self.verify()

    def test_manifest_provenance_and_binary_digest_checked(self):
        self.package['binary_sha256'] = 'f' * 64
        self.write_index()
        with patch.object(check.subprocess, 'run'), self.assertRaises(ValueError):
            self.verify()


if __name__ == '__main__':
    unittest.main()
