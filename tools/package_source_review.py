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
manifest=[]
a.output.parent.mkdir(parents=True,exist_ok=True)
with tarfile.open(a.output,'w:gz') as archive:
    def put(name, data):
        info=tarfile.TarInfo(name);info.size=len(data);info.mode=0o644;info.mtime=0
        archive.addfile(info,io.BytesIO(data))
    for f,n in entries:
        if f.is_symlink():raise SystemExit('Refusing tracked symlink: '+n)
        b=f.read_bytes();put(n,b);manifest.append({'path':n,'sha256':hashlib.sha256(b).hexdigest()})
    sdk=[]
    for f in sorted((framework/'tools/sdk/esp32s3').rglob('*')):
        if f.is_file() and not f.is_symlink():sdk.append({'path':str(f.relative_to(framework)),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()})
    put('SOURCE-MANIFEST.json',(json.dumps(manifest,indent=2)+'\n').encode())
    put('SDK-INVENTORY-REQUIRES-REVIEW.json',(json.dumps(sdk,indent=2)+'\n').encode())
    put('REVIEW-STATUS.txt',b'Private source companion; not complete binary clearance.\nIncludes exact locally resolved application and library source.\nPrecompiled SDK source/license closure remains required.\nNo assertion of byte-identical reproduction of the original official release.\n')
print('Packaged',len(manifest),'source files; SDK manifest entries:',len(sdk))
