"""Fetch one immutable qualification artifact; candidates do not live in Releases."""
import argparse
import io
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import zipfile

ASSETS = {'firmware.bin', 'latest.json', 'REVIEW.json', 'REVIEW.txt', 'LICENSE',
          'THIRD_PARTY_NOTICES.md', 'source-review.tar.gz', 'NOTICES-REVIEW.txt',
          'SHA256SUMS.txt'}


def select_artifact(pages, tag):
    if not re.fullmatch(r'v\d+\.\d+\.\d+-public-review\.\d+', tag):
        raise ValueError('Expected a versioned public-review candidate identifier')
    matches = [a for page in pages for a in page['artifacts']
               if a['name'] == 'candidate-' + tag and not a['expired']]
    if len(matches) != 1:
        raise ValueError('Expected one retained candidate artifact; use a fresh review suffix for each build')
    return matches[0]


def validate_run(run):
    if (run['path'] != '.github/workflows/release.yml' or run['head_branch'] != 'main'
            or run['event'] not in ('workflow_dispatch', 'push') or run['conclusion'] != 'success'):
        raise ValueError('Candidate must come from a successful main qualification build')


def unpack(data, directory):
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)) or set(names) != ASSETS:
            raise ValueError('Unexpected, missing or duplicate candidate files')
        directory.mkdir(parents=True, exist_ok=True)
        if any(directory.iterdir()):
            raise ValueError('Candidate destination must be empty')
        for name in sorted(ASSETS):
            # Fixed basename allowlist; no archive paths or symlinks are extracted.
            (directory / name).write_bytes(archive.read(name))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    repo = os.environ['GITHUB_REPOSITORY']
    pages = json.loads(subprocess.check_output(
        ['gh', 'api', '--paginate', '--slurp', f'repos/{repo}/actions/artifacts?per_page=100']))
    artifact = select_artifact(pages, args.tag)
    run = json.loads(subprocess.check_output(
        ['gh', 'api', f'repos/{repo}/actions/runs/{artifact["workflow_run"]["id"]}']))
    validate_run(run)
    data = subprocess.check_output(['gh', 'api', f'repos/{repo}/actions/artifacts/{artifact["id"]}/zip'])
    if artifact.get('digest') != 'sha256:' + hashlib.sha256(data).hexdigest():
        raise ValueError('Candidate archive differs from the Actions artifact digest')
    unpack(data, args.directory)
    metadata = json.loads((args.directory/'REVIEW.json').read_text())
    if metadata.get('source_commit') != run['head_sha']:
        raise ValueError('Candidate source commit differs from the build run')
    print('Downloaded candidate artifact', artifact['id'], 'from run', run['id'])
    # The caller must next verify source identity, signatures and distribution.


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error))
