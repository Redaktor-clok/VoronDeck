#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prune superseded VoronDeck compiler caches; default is a dry run."""
import argparse
from collections import defaultdict
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
from urllib.error import HTTPError
from urllib.request import Request, urlopen

MANAGED = re.compile(r"^(?:linux-clang-[A-Za-z0-9_.]+|arm64)-[0-9a-f]{64}-[0-9a-f]{40}$")


def candidates(caches, now):
    groups = defaultdict(list)
    for cache in caches:
        if MANAGED.fullmatch(cache['key']):
            # Keep separate toolchain hashes, branch scopes and cache formats.
            group = (cache['ref'], cache['key'].rsplit('-', 1)[0], cache['version'])
            groups[group].append(cache)
    cutoff = now - timedelta(days=1)
    selected = []
    for group in groups.values():
        newest = sorted(group, key=lambda c: (c['created_at'], c['id']), reverse=True)
        for cache in newest[2:]:
            if datetime.fromisoformat(cache['created_at'].replace('Z', '+00:00')) < cutoff:
                selected.append(cache)
    return sorted(selected, key=lambda c: c['id'])


def api(repo, suffix, method='GET'):
    request = Request(
        f'https://api.github.com/repos/{repo}/actions/caches{suffix}',
        headers={'Authorization': f"Bearer {os.environ['GH_TOKEN']}",
                 'Accept': 'application/vnd.github+json',
                 'X-GitHub-Api-Version': '2022-11-28'},
        method=method,
    )
    with urlopen(request, timeout=30) as response:
        return json.load(response) if method == 'GET' else None


def run(repo, apply=False, request=api, now=None):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repo):
        raise ValueError('Invalid repository')
    caches = []
    page = 1
    while True:
        batch = request(repo, f'?per_page=100&page={page}')['actions_caches']
        caches.extend(batch)
        if len(batch) < 100:
            break
        page += 1
    selected = candidates(caches, now or datetime.now(timezone.utc))
    report = {'dry_run': not apply, 'listed': len(caches), 'candidates': selected,
              'deleted': [], 'already_missing': [], 'freed_bytes': 0}
    if apply:
        for cache in selected:
            cache_id = int(cache['id'])
            try:
                request(repo, f'/{cache_id}', 'DELETE')
            except HTTPError as error:
                if error.code != 404:
                    raise
                report['already_missing'].append(cache_id)
            else:
                report['deleted'].append(cache_id)
                report['freed_bytes'] += cache['size_in_bytes']
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    report = run(os.environ['GITHUB_REPOSITORY'], apply=args.apply)
    print(json.dumps(report, indent=2))
    summary = os.environ.get('GITHUB_STEP_SUMMARY')
    if summary:
        with Path(summary).open('a', encoding='utf-8') as stream:
            stream.write('## Кэши / Caches\n\n'
                         f"Dry run: **{report['dry_run']}**; "
                         f"candidates: **{len(report['candidates'])}**; "
                         f"deleted: **{len(report['deleted'])}**; "
                         f"freed: **{report['freed_bytes'] / 1048576:.1f} MiB**.\n")


if __name__ == '__main__':
    main()
