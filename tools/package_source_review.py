"""Package tracked source and resolved library sources for private release review.

The SDK manifest does NOT replace precompiled SDK redistribution/source review.
No Git metadata, device data or private production build objects are included.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--environment', required=True)
p.add_argument('--framework', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
framework = a.framework.resolve()
deps = root / '.pio/libdeps' / a.environment
if not deps.is_dir() or not (framework/'package.json').is_file():
    raise SystemExit('Build the environment first and supply its framework package')
entries = []
tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0')
for name in filter(None, tracked):
    entries.append((root/name, 'application/'+name))
for base, prefix in [(deps, 'dependencies')]+[(framework/n, 'framework/'+n) for n in ('cores','libraries','variants')]:
    for f in sorted(base.rglob('*')):
        rel = f.relative_to(base)
        if any(x.startswith('.') or x in ('__pycache__', 'examples') for x in rel.parts):
            continue
        if f.is_symlink():
            raise SystemExit('Refusing symlink in source archive: '+str(rel))
        if f.is_file():
            entries.append((f, prefix+'/'+str(rel)))
entries.append((framework/'package.json', 'framework/package.json'))
# Include the exact S3 SDK headers/objects needed to relink the LGPL libraries.
# Its notices are retained in the application tree; no other chip targets,
# example credentials, package cache metadata or user settings are included.
for f in sorted((framework/'tools/sdk/esp32s3').rglob('*')):
    if f.is_symlink():
        raise SystemExit('Refusing symlink in SDK: '+str(f))
    if f.is_file():
        entries.append((f, 'framework/'+str(f.relative_to(framework))))
for f in sorted((framework/'tools').glob('platformio-build*.py')):
    entries.append((f, 'framework/tools/'+f.name))
# The optional GNU FreeFont bitmap headers retain their own font license.
# Supply the upstream editable font sources with those generated headers.
font_url='https://ftp.gnu.org/gnu/freefont/freefont-src-20120503.tar.gz'
with urllib.request.urlopen(font_url, timeout=60) as response:
    font_source=response.read(16*1024*1024+1)
if hashlib.sha256(font_source).hexdigest() != '0d62e2e22655f841c3ac5fb8c8059a52fe2c09c695d9e8eda9903406e942657a':
    raise SystemExit('GNU FreeFont source checksum mismatch')
manifest=[]
a.output.parent.mkdir(parents=True,exist_ok=True)
with tarfile.open(a.output,'w:gz') as archive:
    def put(name, data):
        info=tarfile.TarInfo(name);info.size=len(data);info.mode=0o644;info.mtime=0
        archive.addfile(info,io.BytesIO(data))
    for f,n in entries:
        if f.is_symlink():raise SystemExit('Refusing tracked symlink: '+n)
        b=f.read_bytes();put(n,b);manifest.append({'path':n,'sha256':hashlib.sha256(b).hexdigest()})
    name='font-source/freefont-src-20120503.tar.gz'
    put(name,font_source)
    manifest.append({'path':name,'sha256':hashlib.sha256(font_source).hexdigest()})
    sdk=[]
    for f in sorted((framework/'tools/sdk/esp32s3').rglob('*')):
        if f.is_file() and not f.is_symlink():sdk.append({'path':str(f.relative_to(framework)),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()})
    put('SOURCE-MANIFEST.json',(json.dumps(manifest,indent=2)+'\n').encode())
    put('SDK-INVENTORY-REQUIRES-REVIEW.json',(json.dumps(sdk,indent=2)+'\n').encode())
    put('REVIEW-STATUS.txt',b'Private source companion; not complete binary clearance.\nIncludes exact locally resolved application and library source.\nPrecompiled SDK source/license closure remains required.\nNo assertion of byte-identical reproduction of the original official release.\n')
print('Packaged',len(manifest),'source files; SDK manifest entries:',len(sdk))
