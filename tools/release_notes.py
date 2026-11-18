"""Validate the release log and package already-verified public candidate assets."""
import argparse
from datetime import datetime
from zoneinfo import ZoneInfo
import hashlib
import json
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REPOSITORY = 'https://github.com/Origami-Computing/origami-firmware-public'
VERSION = r'v(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
ASSETS = ('firmware.bin', 'latest.json', 'REVIEW.json', 'REVIEW.txt', 'LICENSE',
          'THIRD_PARTY_NOTICES.md', 'source-review.tar.gz', 'NOTICES-REVIEW.txt',
          'SHA256SUMS.txt')


def version_tuple(version):
    if not re.fullmatch(VERSION, version):
        raise ValueError('Expected a stable vX.Y.Z version')
    return tuple(map(int, version[1:].split('.')))


def configured_version(root=ROOT):
    versions = re.findall(r'OTA_FIRMWARE_VERSION=.*?(' + VERSION + ')',
                          (root / 'platformio.ini').read_text())
    if len(versions) != 1:
        raise ValueError('Expected one configured firmware version')
    return versions[0]


def release_entry(changelog, version, previous=None):
    version_tuple(version)
    sections = re.split(r'^## (' + VERSION + r')\s*$', changelog, flags=re.M)
    entries = dict(zip(sections[1::2], sections[2::2]))
    if len(entries) != len(sections[1::2]):
        raise ValueError('Duplicate release version in CHANGELOG.md')
    if version not in entries:
        raise ValueError(f'Add a brief, reviewed CHANGELOG.md entry for {version}')
    body = entries[version].strip()
    lines = body.splitlines()
    if not lines or lines[0] != 'Unreleased.':
        published = re.fullmatch(r'Published: (\d{4}-\d{2}-\d{2} at \d{2}:\d{2} [AP]M) (PST|PDT)\.', lines[0] if lines else '')
        if not published:
            raise ValueError('Entry must include Published: YYYY-MM-DD at HH:MM AM/PM PST/PDT.')
        timestamp = datetime.strptime(published[1], '%Y-%m-%d at %I:%M %p')
        pacific = ZoneInfo('America/Los_Angeles')
        if not any(timestamp.replace(tzinfo=pacific, fold=fold).tzname() == published[2]
                   for fold in (0, 1)):
            raise ValueError('Use PST or PDT according to the Pacific publication date')
    lines = lines[1:]
    while lines and not lines[0]:
        lines.pop(0)
    match = re.fullmatch(r'Changes since (' + VERSION + r')\.', lines[0] if lines else '')
    if not match or version_tuple(match[1]) >= version_tuple(version):
        raise ValueError('Entry must identify an older release: Changes since vX.Y.Z.')
    if previous is not None and match[1] != previous:
        raise ValueError(f'Notes must describe changes since the latest published release, {previous}')
    bullets = [line[2:] for line in lines[1:] if line.startswith('- ')]
    if not 1 <= len(bullets) <= 8 or any(not 10 <= len(b) <= 240 for b in bullets):
        raise ValueError('Use 1–8 concrete change bullets, each 10–240 characters')
    if any(line and not line.startswith('- ') for line in lines[1:]):
        raise ValueError('Release entries contain only a baseline and concise change bullets')
    if re.search(r'\b(TODO|TBD|placeholder|coming soon)\b', body, re.I):
        raise ValueError('Replace unfinished release notes before publishing')
    return body, match[1]


def render(changelog, version, metadata, previous=None):
    body, baseline = release_entry(changelog, version, previous)
    if body.startswith('Unreleased.'):
        published = datetime.now(ZoneInfo('America/Los_Angeles')).strftime('%Y-%m-%d at %I:%M %p %Z')
        body = body.replace('Unreleased.', f'Published: {published}.', 1)
    if metadata.get('version') != version:
        raise ValueError('Candidate and release notes versions differ')
    commit, digest = metadata.get('source_commit', ''), metadata.get('firmware_sha256', '')
    if not re.fullmatch('[a-f0-9]{40}', commit) or not re.fullmatch('[a-f0-9]{64}', digest):
        raise ValueError('Missing candidate provenance')
    return (f'## What changed\n\n{body}\n\n'
            f'[Full changes]({REPOSITORY}/compare/{baseline}...{version})\n\n'
            '## Downloads\n\n'
            f'- **[Download firmware]({REPOSITORY}/releases/download/{version}/firmware.bin)** — application image for an existing rev-4 ESP32-S3 N16R8 device.\n'
            f'- **[Download complete bundle]({REPOSITORY}/releases/download/{version}/kamiOS-{version}.zip)** — one ZIP with the same firmware, release notes, corresponding source, licenses and verification files.\n\n'
            'Use the on-device updater for a normal upgrade. The ZIP is not a factory installer; '
            'do not flash firmware.bin at address zero. Preserve settings and the partition layout.\n\n'
            'Use the two download links above. GitHub’s automatic “Source code” archives are repository snapshots, not update packages.\n\n'
            '## OTA not working?\n\n'
            '**[Recover Kami over USB](https://origami-computing.github.io/origami-firmware-public/)** '
            'using Chrome or Edge on a computer and a USB data cable. Follow the page through verification, '
            'then restart and update on-device to the latest Public release. The recovery version shown on '
            'that page may be older than this release. It is for supported existing devices, not factory setup '
            'or recovery of deleted files. It checks the layout and is designed to preserve settings and SD files.\n\n'
            '<details>\n<summary>Build details</summary>\n\n'
            f'Qualified source: [{commit[:7]}]({REPOSITORY}/commit/{commit})\n\n'
            f'Firmware SHA-256: `{digest}`\n\n</details>\n')


def package(directory, output, version, changelog, previous=None):
    # Candidate signature, source identity and distribution checks run upstream.
    # Use an explicit allowlist, never sweep a directory into a public bundle.
    metadata = json.loads((directory / 'REVIEW.json').read_text())
    notes = render(changelog, version, metadata, previous)
    for name in ASSETS:
        path = directory / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('Missing or unsafe candidate asset: ' + name)
    if hashlib.sha256((directory / 'firmware.bin').read_bytes()).hexdigest() != metadata['firmware_sha256']:
        raise ValueError('Firmware changed after candidate verification')
    output.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output / f'kamiOS-{version}.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in ASSETS:
            archive.write(directory / name, name)
        archive.writestr('RELEASE-NOTES.md', notes)
        archive.writestr('CHANGELOG.md', changelog)
    (output / 'RELEASE-NOTES.md').write_text(notes)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version')
    parser.add_argument('--previous-version')
    parser.add_argument('--directory', type=Path)
    parser.add_argument('--output', type=Path, default=Path('release-output'))
    args = parser.parse_args()
    version = args.version or configured_version()
    if version != configured_version():
        raise ValueError('Release notes version differs from platformio.ini')
    changelog = (ROOT / 'CHANGELOG.md').read_text()
    release_entry(changelog, version, args.previous_version)
    if args.directory:
        package(args.directory, args.output, version, changelog, args.previous_version)
    print(f'PASS release notes for {version}')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error))
