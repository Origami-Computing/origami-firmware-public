#!/usr/bin/env python3
"""Build and publish a signed OTA channel manifest. Requires AWS CLI and OpenSSL."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
PUBLIC_KEY = ROOT / 'infra/ota/signing-public.pem'
MAX_IMAGE_SIZE = 0x600000


def run(args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs).stdout


def version_tuple(version):
    match = re.fullmatch(r'v(\d+)\.(\d+)\.(\d+)(?:-staging\.(\d+))?', version)
    if not match:
        raise ValueError('Invalid firmware version')
    return tuple(int(x) if x is not None else -1 for x in match.groups())


def validate_image(data):
    if not 32 <= len(data) <= MAX_IMAGE_SIZE or data[0] != 0xE9:
        raise ValueError('Expected an ESP application image fitting the OTA slot')
    # App descriptor is after the 24-byte image header and first 8-byte segment header.
    if data[32:36] != bytes.fromhex('3254cdab'):
        raise ValueError('Expected application image, not bootloader/merged flash image')
    if re.search(rb'(github_pat_[A-Za-z0-9_]{20,}|gh[pousr]_[A-Za-z0-9]{20,})', data):
        raise ValueError('Refusing to publish firmware containing a GitHub credential')


def manifest_payload(data, channel, version, hardware, layout):
    if channel not in ("public", "private", "staging"):
        raise ValueError("Invalid channel")
    parsed = version_tuple(version)
    if (channel == 'staging') != (parsed[3] >= 0):
        raise ValueError('Channel and version suffix disagree')
    for value in (hardware, layout):
        if not re.fullmatch(r'[a-z0-9-]{1,48}', value):
            raise ValueError('Invalid hardware/layout identifier')
    validate_image(data)
    # The CI version flag is embedded in the image. Reject accidental publishing
    # of a different release under the requested signed version.
    if (version.encode('ascii') + b'\0') not in data:
        raise ValueError('Requested version is not embedded in firmware')
    digest = hashlib.sha256(data).hexdigest()
    return dict(schema=1, channel=channel, version=version, hardware=hardware,
                layout=layout, size=len(data), sha256=digest,
                path=f'/releases/{channel}/{hardware}/{layout}/{version}/{digest}/firmware.bin')


def sign_manifest(payload, private_key):
    raw = json.dumps(payload, sort_keys=True, separators=(',', ':')).encode('ascii')
    if len(raw) > 2048:
        raise ValueError('Manifest payload too large for firmware client')
    signature = run(['openssl', 'dgst', '-sha256', '-sign', str(private_key)], input=raw)
    if len(signature) != 256:
        raise ValueError('Expected RSA-2048 signing key')
    envelope = dict(payload=raw.decode('ascii'), signature=base64.b64encode(signature).decode('ascii'))
    verify_manifest(envelope)
    return envelope


def verify_manifest(envelope, public_key=PUBLIC_KEY):
    signature = base64.b64decode(envelope['signature'], validate=True)
    if len(signature) != 256:
        raise ValueError('Invalid signature length')
    with tempfile.TemporaryDirectory() as tmp:
        sig = Path(tmp)/'signature'
        sig.write_bytes(signature)
        run(['openssl', 'dgst', '-sha256', '-verify', str(public_key), '-signature', str(sig)],
            input=envelope['payload'].encode('ascii'))
    return json.loads(envelope['payload'])


def access_key():
    key = os.environ.get('OTA_ACCESS_KEY', '')
    if not re.fullmatch('[a-f0-9]{64}', key):
        raise ValueError('Set OTA_ACCESS_KEY to the channel-appropriate private credential')
    return key


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None  # Never forward a bearer credential to another location.


def fetch(url, key):
    request = urllib.request.Request(url, headers={'Authorization': 'Bearer '+key})
    with urllib.request.build_opener(NoRedirect()).open(request, timeout=60) as response:
        return response.read()


def aws(*args):
    return run(['aws', *args])


def publish(args):
    data = args.firmware.read_bytes()
    payload = manifest_payload(data, args.channel, args.version, args.hardware, args.layout)
    envelope = sign_manifest(payload, args.signing_key)
    output = args.output
    output.mkdir(parents=True, exist_ok=True)
    manifest = output/'latest.json'
    manifest.write_text(json.dumps(envelope, separators=(',', ':'))+'\n')
    if args.prepare_only:
        print(f'Validated signed manifest: {manifest}')
        return
    if not args.bucket or not args.base_url:
        raise ValueError('--bucket and --base-url are required to publish')
    if not re.fullmatch(r'https://[a-z0-9.-]+', args.base_url):
        raise ValueError('Expected HTTPS origin without a trailing slash')
    credential = access_key()
    channel_key = f'channels/{args.channel}/{args.hardware}/{args.layout}.json'
    # Never interpret access errors as an empty channel. List distinguishes absence.
    existing = json.loads(aws('s3api', 'list-objects-v2', '--bucket', args.bucket,
                             '--prefix', channel_key, '--output', 'json'))
    if any(item['Key'] == channel_key for item in existing.get('Contents', [])):
        previous = output/'previous.json'
        aws('s3api', 'get-object', '--bucket', args.bucket, '--key', channel_key, str(previous))
        old = verify_manifest(json.loads(previous.read_text()))
        if any(old[k] != payload[k] for k in ('channel', 'hardware', 'layout')):
            raise ValueError('Existing channel identity differs')
        if version_tuple(old['version']) > version_tuple(args.version):
            raise ValueError('Refusing to move channel backwards')
        if old['version'] == args.version and old['sha256'] != payload['sha256']:
            raise ValueError('Version already published with different bytes; bump version')
    key = payload['path'].lstrip('/')
    aws('s3api', 'put-object', '--bucket', args.bucket, '--key', key,
        '--body', str(args.firmware), '--content-type', 'application/octet-stream',
        '--cache-control', 'public,max-age=31536000,immutable')
    # Publish only after the authenticated CDN delivery path is verified.
    downloaded = fetch(args.base_url + payload['path'], credential)
    if hashlib.sha256(downloaded).hexdigest() != payload['sha256']:
        raise ValueError('CloudFront download did not match firmware')
    aws('s3api', 'put-object', '--bucket', args.bucket, '--key', channel_key,
        '--body', str(manifest), '--content-type', 'application/json',
        '--cache-control', 'public,max-age=60,must-revalidate')
    print(f'Published {args.version}: {args.base_url}/{channel_key}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--firmware', type=Path, required=True)
    parser.add_argument('--channel', choices=['public', 'private', 'staging'], required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--hardware', default='n16r8-rev4')
    parser.add_argument('--layout', default='n16r8-ota-v1')
    parser.add_argument('--signing-key', type=Path, required=True)
    parser.add_argument('--bucket', default=os.environ.get('OTA_S3_BUCKET'))
    parser.add_argument('--base-url', default=os.environ.get('OTA_BASE_URL'))
    parser.add_argument('--output', type=Path, default=ROOT/'.pio/ota-publish')
    parser.add_argument('--prepare-only', action='store_true')
    publish(parser.parse_args())


if __name__ == '__main__':
    main()
