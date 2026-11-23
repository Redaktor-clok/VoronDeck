# SPDX-License-Identifier: GPL-3.0-or-later
import importlib.util
from datetime import datetime, timezone
from pathlib import Path
import unittest
from urllib.error import HTTPError

spec = importlib.util.spec_from_file_location('prune', Path(__file__).resolve().parents[2] / 'scripts/ci/prune-caches.py')
prune = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prune)
NOW = datetime(2026, 11, 23, tzinfo=timezone.utc)


def cache(number, family='arm64', ref='refs/heads/main', version='one', day=1):
    return dict(id=number, key=f'{family}-' + 'a' * 64 + f'-{number:040x}',
                ref=ref, version=version, created_at=f'2026-11-{day:02}T00:00:00Z', size_in_bytes=100)


class CacheTests(unittest.TestCase):
    def test_preserves_two_newest_per_branch_toolchain_and_version(self):
        entries = [cache(i, day=i) for i in range(1, 5)]
        entries += [cache(5, ref='refs/heads/beta'), cache(6, version='two'),
                    cache(7, family='linux-clang-Linux')]
        entries.append(dict(cache(8), key='arm64-' + 'b' * 64 + f'-{8:040x}'))
        self.assertEqual([c['id'] for c in prune.candidates(entries, NOW)], [1, 2])

    def test_protects_unmanaged_and_recent_entries(self):
        entries = [dict(cache(1), key='ccache-release-arm64-123'),
                   dict(cache(2), key='setup-python-pip-hash'),
                   dict(cache(3), key='arm64-malformed')]
        entries += [cache(i, day=23) for i in range(4, 10)]
        self.assertEqual(prune.candidates(entries, NOW), [])

    def test_pagination_and_dry_run(self):
        calls = []
        def request(repo, suffix, method='GET'):
            calls.append((suffix, method))
            return {'actions_caches': [cache(i) for i in range(1, 101)] if suffix.endswith('&page=1') else [cache(101)]}
        result = prune.run('owner/repo', request=request, now=NOW)
        self.assertEqual(result['listed'], 101)
        self.assertEqual(len(result['candidates']), 99)
        self.assertEqual(result['deleted'], [])
        self.assertEqual(len(calls), 2)

    def test_apply_and_disappeared_cache(self):
        deleted = []
        def request(repo, suffix, method='GET'):
            if method == 'GET':
                return {'actions_caches': [cache(i) for i in range(1, 5)]}
            deleted.append(suffix)
            if suffix == '/1':
                raise HTTPError(suffix, 404, 'gone', {}, None)
        result = prune.run('owner/repo', True, request, NOW)
        self.assertEqual(deleted, ['/1', '/2'])
        self.assertEqual(result['already_missing'], [1])
        self.assertEqual(result['deleted'], [2])
        self.assertEqual(result['freed_bytes'], 100)

    def test_permission_failure_is_not_hidden(self):
        def request(repo, suffix, method='GET'):
            if method == 'GET':
                return {'actions_caches': [cache(i) for i in range(1, 4)]}
            raise HTTPError(suffix, 403, 'forbidden', {}, None)
        with self.assertRaises(HTTPError):
            prune.run('owner/repo', True, request, NOW)

    def test_rejects_invalid_repo_before_network(self):
        with self.assertRaises(ValueError):
            prune.run('../wrong/repo')


if __name__ == '__main__':
    unittest.main()
