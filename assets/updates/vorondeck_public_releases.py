# SPDX-License-Identifier: GPL-3.0-or-later
"""Read public VoronDeck prereleases. Never execute downloaded content."""
import hashlib
from html.parser import HTMLParser
import json
from pathlib import PurePosixPath
import re
from urllib.parse import quote, urlparse
from urllib.request import Request, urlopen
from urllib.error import HTTPError
import xml.etree.ElementTree as ET

REPO = 'Redaktor-clok/VoronDeck'
API = 'https://api.github.com/repos/' + REPO
VERSION = re.compile(r'(\d+)\.(\d+)\.(\d+)(?:-(T|beta)\.(\d+))?')
SHA = re.compile(r'[a-f0-9]{64}')
COMMIT = re.compile(r'[a-f0-9]{40}')
MAX_PACKAGE = 95 * 1024**2


def version_key(version):
    match = VERSION.fullmatch(version)
    if not match:
        raise ValueError('Invalid release version')
    major, minor, patch, suffix, sequence = match.groups()
    return (int(major), int(minor), int(patch), 0 if suffix else 1, int(sequence or 0))


def display_version(version):
    version_key(version)
    return re.sub(r'-(?:T|beta)\.(\d+)$', r' T\1', version)


def read_url(url, limit):
    parsed = urlparse(url)
    if parsed.scheme != 'https' or parsed.netloc not in ('api.github.com', 'github.com'):
        raise ValueError('Untrusted release URL')
    request = Request(url, headers={'User-Agent': 'VoronDeck-updater',
                                    'Accept': 'application/vnd.github+json' if parsed.netloc == 'api.github.com' else 'application/octet-stream'})
    with urlopen(request, timeout=30) as response:
        final = urlparse(response.geturl())
        if final.scheme != 'https' or final.netloc not in (
                'api.github.com', 'github.com', 'release-assets.githubusercontent.com',
                'objects.githubusercontent.com'):
            raise ValueError('Untrusted release redirect')
        data = response.read(limit + 1)
    if len(data) > limit:
        raise ValueError('Release download exceeds size limit')
    return data


def asset_url(tag, name):
    return 'https://github.com/' + REPO + '/releases/download/' + quote(tag, safe='') + '/' + quote(name, safe='')


def published_tags(read):
    """The public release feed remains available when the anonymous API is limited."""
    data = read('https://github.com/' + REPO + '/releases.atom', 2 * 1024**2)
    if b'<!DOCTYPE' in data.upper() or b'<!ENTITY' in data.upper():
        raise ValueError('Invalid release feed')
    try:
        feed = ET.fromstring(data)
    except ET.ParseError as error:
        raise ValueError('Invalid release feed') from error
    if feed.tag != '{http://www.w3.org/2005/Atom}feed':
        raise ValueError('Invalid release feed')
    tags = set()
    prefix = 'https://github.com/' + REPO + '/releases/tag/'
    for link in feed.findall('{http://www.w3.org/2005/Atom}entry/{http://www.w3.org/2005/Atom}link')[:100]:
        url = link.get('href', '')
        tag = url.removeprefix(prefix)
        match = VERSION.fullmatch(tag.removeprefix('v'))
        if url.startswith(prefix) and tag.startswith('v') and match and match.group(4):
            tags.add(tag)
    return tags


def published_release(tag, read):
    # Only release assets, never repository files or arbitrary links. A test-version
    # tag plus its matching test/beta index identifies this fallback's release type.
    class Assets(HTMLParser):
        def __init__(self):
            super().__init__()
            self.urls = []

        def handle_starttag(self, name, attrs):
            if name == 'a':
                self.urls.append(dict(attrs).get('href', ''))

    parser = Assets()
    parser.feed(read('https://github.com/' + REPO + '/releases/expanded_assets/' +
                     quote(tag, safe=''), 2 * 1024**2).decode('utf-8'))
    prefix = '/' + REPO + '/releases/download/' + tag + '/'
    names = [url[len(prefix):] for url in parser.urls if url.startswith(prefix)]
    if names.count('release.json') != 1:
        raise ValueError('Published release index is missing or ambiguous')
    raw = read(asset_url(tag, 'release.json'), 65536)
    index = json.loads(raw)
    packages = index.get('packages', []) if isinstance(index, dict) else []
    if not isinstance(packages, list) or len(packages) > 12:
        raise ValueError('Invalid release packages')
    assets = [{'name': 'release.json', 'size': len(raw)}]
    for package in packages:
        if not isinstance(package, dict) or not isinstance(package.get('path'), str):
            raise ValueError('Invalid release package')
        name = PurePosixPath(package['path']).name
        if names.count(name) != 1:
            raise ValueError('Published archive is missing or ambiguous')
        assets.append({'name': name, 'size': package.get('size')})
    return {'draft': False, 'prerelease': True, 'tag_name': tag, 'assets': assets}


def package_from_release(release, arch, libc, read=read_url):
    if not isinstance(release, dict) or release.get('draft') is not False or release.get('prerelease') is not True:
        raise ValueError('Not a published prerelease')
    tag = release.get('tag_name', '')
    if not isinstance(tag, str):
        raise ValueError('Invalid release tag')
    version = tag.removeprefix('v')
    if tag != 'v' + version or not VERSION.fullmatch(version) or VERSION.fullmatch(version).group(4) is None:
        raise ValueError('Prerelease tag must identify a test version')
    assets = release.get('assets', [])
    if not isinstance(assets, list) or len(assets) > 100:
        raise ValueError('Invalid release assets')
    manifests = [a for a in assets if isinstance(a, dict) and a.get('name') == 'release.json']
    if len(manifests) != 1 or not 0 < manifests[0].get('size', 0) <= 65536:
        raise ValueError('Missing unique release index')
    index = json.loads(read(asset_url(tag, 'release.json'), 65536))
    if (not isinstance(index, dict) or index.get('schema') != 1 or index.get('project') != 'VoronDeck'
            or index.get('version') != version or index.get('channel') not in ('test', 'beta')):
        raise ValueError('Release index does not match prerelease')
    packages = index.get('packages')
    if not isinstance(packages, list) or len(packages) > 12:
        raise ValueError('Invalid release packages')
    compatible = [p for p in packages if isinstance(p, dict) and p.get('architecture') == arch and p.get('libc') == libc]
    if len(compatible) != 1:
        raise ValueError('No unique compatible prerelease package')
    package = compatible[0]
    path = PurePosixPath(package.get('path', ''))
    if (len(path.parts) != 2 or path.parts[0] != 'packages' or str(path) != package['path']
            or not re.fullmatch(r'[A-Za-z0-9_.-]+\.tar\.gz', path.name)
            or not SHA.fullmatch(package.get('sha256', ''))
            or not SHA.fullmatch(package.get('binary_sha256', ''))
            or not COMMIT.fullmatch(package.get('source_commit', ''))
            or type(package.get('size')) is not int or not 0 < package['size'] < MAX_PACKAGE):
        raise ValueError('Invalid prerelease package metadata')
    archives = [a for a in assets if isinstance(a, dict) and a.get('name') == path.name]
    if len(archives) != 1 or archives[0].get('size') != package['size']:
        raise ValueError('Prerelease archive is missing or incomplete')
    return dict(package, version=version, display_version=display_version(version),
                tag=tag, url=asset_url(tag, path.name), kind='test')


def latest(arch, libc, installed=None, read=read_url):
    """Discover the channel independently; an optional floor filters install offers."""
    try:
        releases = json.loads(read(API + '/releases?per_page=100', 2 * 1024**2))
    except HTTPError as error:
        if error.code not in (403, 429):
            raise
        tags = [tag for tag in published_tags(read)
                if installed is None or version_key(tag[1:]) > version_key(installed)]
        for tag in sorted(tags, key=lambda tag: version_key(tag[1:]), reverse=True)[:5]:
            try:
                return package_from_release(published_release(tag, read), arch, libc, read)
            except (ValueError, TypeError, KeyError):
                continue
        return None
    if not isinstance(releases, list):
        raise ValueError('Invalid release listing')
    candidates = []
    for release in releases:
        if not isinstance(release, dict) or release.get('draft') is not False or release.get('prerelease') is not True:
            continue
        tag = release.get('tag_name', '')
        if not isinstance(tag, str):
            continue
        version = tag.removeprefix('v')
        try:
            key = version_key(version)
            if installed is None or key > version_key(installed):
                candidates.append((key, release))
        except (ValueError, TypeError):
            continue
    # Bound network work even when many published test packages are incomplete.
    for _, release in sorted(candidates, key=lambda item: item[0], reverse=True)[:5]:
        try:
            return package_from_release(release, arch, libc, read)
        except (ValueError, TypeError, KeyError):
            # An incompatible/incomplete draft package is never an install offer.
            continue
    return None


def selected(selection, arch, libc, installed, read=read_url):
    """Revalidate exactly the offer accepted by the user, never a newer substitute."""
    if (not isinstance(selection, dict) or selection.get('kind') != 'test'
            or not isinstance(selection.get('tag'), str)
            or selection['tag'] != 'v' + selection.get('version', '')
            or version_key(selection['version']) <= version_key(installed)):
        raise ValueError('Invalid or outdated test selection')
    try:
        release = json.loads(read(API + '/releases/tags/' + quote(selection['tag'], safe=''), 2 * 1024**2))
    except HTTPError as error:
        if error.code not in (403, 429):
            raise
        release = published_release(selection['tag'], read)
    package = package_from_release(release, arch, libc, read)
    if any(package[key] != selection.get(key) for key in ('version', 'tag', 'sha256', 'source_commit')):
        raise ValueError('The selected release changed; check updates again')
    return package


def download(package, destination, read=read_url):
    data = read(package['url'], package['size'])
    if len(data) != package['size'] or hashlib.sha256(data).hexdigest() != package['sha256']:
        raise ValueError('Prerelease package checksum or size mismatch')
    destination.write_bytes(data)
