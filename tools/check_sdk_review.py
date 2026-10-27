#!/usr/bin/env python3
"""Check the reviewed SDK archive inventory and TLS override in a build link map."""
from pathlib import Path
import argparse,json,re
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--map',type=Path,required=True);a=p.parse_args()
s=a.map.read_text();inventory=json.loads((root/'third_party/linked-library-licenses.json').read_text())
known={i['archive'] for i in inventory['libraries']}
actual=set(re.findall(r'/(lib[^/\s()]+\.a)\(',s))
assert actual, 'No linked archives found'
assert actual <= known, 'Unreviewed SDK/library archives: '+str(sorted(actual-known))
for i in inventory['libraries']:
 for n in [i['primary_notice']]+i['additional_notices']:assert (root/n).is_file(),n
assert 'src/hal/mbedtls_finished.c.o' in s, 'Missing repaired TLS translation unit'
assert 'libmbedtls_2.a(ssl_tls.c.obj)' not in s, 'Original TLS unit was linked'
print('PASS reviewed archive inventory and repaired TLS linkage:',len(actual),'archives')
