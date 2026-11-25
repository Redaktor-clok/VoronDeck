# SPDX-License-Identifier: GPL-3.0-or-later
"""Public test offers: bounded metadata, exact consent and archive verification."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from urllib.error import HTTPError

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('public_releases', ROOT / 'assets/updates/vorondeck_public_releases.py')
public = importlib.util.module_from_spec(spec)
spec.loader.exec_module(public)


class PublicReleases(unittest.TestCase):
    def setUp(self):
        self.data = b'archive fixture'
        self.package = {'architecture': 'aarch64', 'libc': 'glibc',
                        'path': 'packages/VoronDeck-1.0.2-T.1-aarch64-glibc.tar.gz',
                        'sha256': hashlib.sha256(self.data).hexdigest(), 'binary_sha256': 'b' * 64,
                        'source_commit': 'c' * 40, 'size': len(self.data)}
        self.index = {'schema': 1, 'project': 'VoronDeck', 'channel': 'test',
                      'version': '1.0.2-T.1', 'packages': [self.package]}
        self.release = {'draft': False, 'prerelease': True, 'tag_name': 'v1.0.2-T.1',
                        'assets': [{'name': 'release.json', 'size': 600},
                                   {'name': Path(self.package['path']).name, 'size': len(self.data)}]}
        self.listing = [self.release]
        self.calls = []

    def read(self, url, limit):
        self.calls.append(url)
        if '/releases?' in url:
            return json.dumps(self.listing).encode()
        if '/releases/tags/' in url:
            return json.dumps(self.release).encode()
        if url.endswith('/release.json'):
            return json.dumps(self.index).encode()
        return self.data

    def latest(self, installed='1.0.1', arch='aarch64'):
        return public.latest(arch, 'glibc', installed, self.read)

    def test_stable_user_gets_optional_test_offer_without_archive_download(self):
        offer = self.latest()
        self.assertEqual(offer['display_version'], '1.0.2 T1')
        self.assertEqual(offer['kind'], 'test')
        self.assertEqual(len(self.calls), 2)
        self.assertFalse(any(url.endswith('.tar.gz') for url in self.calls))

    def test_final_release_sorts_after_its_tests_and_numeric_sequence(self):
        self.assertLess(public.version_key('1.0.2-T.9'), public.version_key('1.0.2-T.10'))
        self.assertLess(public.version_key('1.0.2-T.10'), public.version_key('1.0.2'))
        self.assertIsNone(self.latest('1.0.2'))

    def test_channel_discovery_without_installed_version_filter(self):
        self.assertEqual(self.latest(None)['version'], '1.0.2-T.1')
        self.assertIsNone(self.latest('1.0.2-beta.2'))

    def test_draft_and_stable_entries_are_not_test_offers(self):
        for change in ({'draft': True}, {'prerelease': False}, {'tag_name': 'v1.0.2'}):
            with self.subTest(change=change):
                self.listing = [dict(self.release, **change)]
                self.assertIsNone(self.latest())

    def test_incompatible_architecture_is_not_offered(self):
        self.assertIsNone(self.latest(arch='armv7l'))

    def test_invalid_index_or_archive_is_not_offered(self):
        for key, value in [('path', '../payload.tar.gz'), ('sha256', 'bad'),
                           ('source_commit', 'main'), ('size', -1), ('size', True)]:
            with self.subTest(key=key, value=value):
                previous = self.package[key]
                self.package[key] = value
                self.assertIsNone(self.latest())
                self.package[key] = previous
        self.release['assets'].pop()
        self.assertIsNone(self.latest())

    def test_duplicate_compatible_packages_are_rejected(self):
        self.index['packages'].append(dict(self.package))
        self.assertIsNone(self.latest())

    def test_wrong_project_version_or_channel_is_rejected(self):
        for key, value in [('project', 'Other'), ('version', '1.0.3-T.1'), ('channel', 'stable')]:
            with self.subTest(key=key):
                previous = self.index[key]
                self.index[key] = value
                self.assertIsNone(self.latest())
                self.index[key] = previous

    def test_network_failure_is_not_reported_as_no_update(self):
        def offline(*_):
            raise OSError('offline')
        with self.assertRaises(OSError):
            public.latest('aarch64', 'glibc', '1.0.1', offline)

    def test_selection_revalidated_and_replaced_asset_refused(self):
        selection = self.latest()
        self.assertEqual(public.selected(selection, 'aarch64', 'glibc', '1.0.1', self.read)['sha256'], selection['sha256'])
        self.package['sha256'] = 'd' * 64
        with self.assertRaises(ValueError):
            public.selected(selection, 'aarch64', 'glibc', '1.0.1', self.read)

    def test_downgrade_and_changed_tag_refused_before_network(self):
        selection = self.latest()
        self.calls.clear()
        for value in (selection, dict(selection, tag='../../other')):
            with self.assertRaises(ValueError):
                public.selected(value, 'aarch64', 'glibc', '1.0.3', self.read)
        self.assertEqual(self.calls, [])

    def test_bad_download_never_written(self):
        package = self.latest()
        with tempfile.TemporaryDirectory() as temp:
            destination = Path(temp) / 'package.tar.gz'
            with self.assertRaises(ValueError):
                public.download(package, destination, lambda *_: b'wrong')
            self.assertFalse(destination.exists())
            public.download(package, destination, self.read)
            self.assertEqual(destination.read_bytes(), self.data)

    def test_metadata_requests_are_bounded(self):
        self.listing = [copy.deepcopy(self.release) for _ in range(100)]
        self.index['packages'] = []
        self.assertIsNone(self.latest())
        self.assertEqual(len(self.calls), 6)


    def limited_read(self, url, limit):
        if url.startswith(public.API):
            raise HTTPError(url, 403, 'rate limit', {}, None)
        if url.endswith('/releases.atom'):
            return ('<feed xmlns="http://www.w3.org/2005/Atom"><entry><link href="https://github.com/' +
                    public.REPO + '/releases/tag/v1.0.2-T.1"/></entry></feed>').encode()
        if '/expanded_assets/' in url:
            return ('<a href="/' + public.REPO + '/releases/download/v1.0.2-T.1/release.json">index</a>' +
                    '<a href="/' + public.REPO + '/releases/download/v1.0.2-T.1/' +
                    Path(self.package['path']).name + '">archive</a>').encode()
        return self.read(url, limit)

    def test_rate_limit_fallback_and_exact_selection(self):
        offer = public.latest('aarch64', 'glibc', '1.0.1', self.limited_read)
        self.assertEqual(offer['version'], '1.0.2-T.1')
        self.assertEqual(public.selected(offer, 'aarch64', 'glibc', '1.0.1', self.limited_read), offer)
        self.package['sha256'] = 'e' * 64
        with self.assertRaises(ValueError):
            public.selected(offer, 'aarch64', 'glibc', '1.0.1', self.limited_read)

    def test_fallback_rejects_missing_archive_and_stable_index(self):
        def missing(url, limit):
            if '/expanded_assets/' in url:
                return ('<a href="/' + public.REPO + '/releases/download/v1.0.2-T.1/release.json">index</a>').encode()
            return self.limited_read(url, limit)
        self.assertIsNone(public.latest('aarch64', 'glibc', '1.0.1', missing))
        self.index['channel'] = 'stable'
        self.assertIsNone(public.latest('aarch64', 'glibc', '1.0.1', self.limited_read))

    def test_feed_is_bounded_and_external_tags_ignored(self):
        self.assertEqual(public.published_tags(lambda *_: b'<feed xmlns="http://www.w3.org/2005/Atom"><entry><link href="https://other/releases/tag/v9.0.0-T.1"/></entry></feed>'), set())
        for data in (b'bad xml', b'<!DOCTYPE feed><feed/>', b'<html/>'):
            with self.assertRaises(ValueError):
                public.published_tags(lambda *_: data)


if __name__ == '__main__':
    unittest.main()
