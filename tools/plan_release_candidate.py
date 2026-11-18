"""Select a new public candidate when a version upgrade reaches protected main."""
import os
from pathlib import Path
import re
import subprocess
from release_notes import VERSION, configured_version, version_tuple


def plan(event, version, previous=None, requested='', run_id='0', attempt='1'):
    if event == 'workflow_dispatch':
        if not re.fullmatch(re.escape(version) + r'-public-review\.\d+', requested):
            raise ValueError('Candidate identifier must match the configured version')
        return requested
    if event != 'push':
        raise ValueError('Only protected-main pushes and manual builds are supported')
    if version == previous:
        return None
    if version_tuple(version) <= version_tuple(previous):
        raise ValueError('Public upgrade version must increase')
    if not run_id.isdigit() or not attempt.isdigit():
        raise ValueError('Missing Actions run identity')
    return f'{version}-public-review.{run_id}{int(attempt):03d}'


if __name__ == '__main__':
    previous = None
    if os.environ['GITHUB_EVENT_NAME'] == 'push':
        before = os.environ['BEFORE']
        if not re.fullmatch('[a-f0-9]{40}', before):
            raise SystemExit('Missing prior main revision')
        source = subprocess.check_output(['git', 'show', before + ':platformio.ini'], text=True)
        versions = re.findall(r'OTA_FIRMWARE_VERSION=.*?(' + VERSION + ')', source)
        if len(versions) != 1:
            raise SystemExit('Expected one previous firmware version')
        previous = versions[0]
    tag = plan(os.environ['GITHUB_EVENT_NAME'], configured_version(), previous,
               os.environ.get('REQUESTED_TAG', ''), os.environ['GITHUB_RUN_ID'],
               os.environ['GITHUB_RUN_ATTEMPT'])
    with Path(os.environ['GITHUB_OUTPUT']).open('a') as output:
        output.write(f'build={str(tag is not None).lower()}\n')
        if tag:
            output.write(f'tag={tag}\n')
