#!/usr/bin/env python3
"""Compile the real version comparator; channels have independent version series."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'src/services/ota_client.cpp').read_text()
source=source[source.index('struct ParsedVersion {'):source.index('static void setError(',source.index('struct ParsedVersion {'))]
prefix='''#include <cstdio>
#include <cstring>
#include <cassert>
enum class ReleaseChannel {Public, Staging, Private};
#define OTA_FIRMWARE_VERSION "v1.0.0"
#define OTA_BUILD_CHANNEL "public"
bool official=true;
bool isOfficialBuild(){return official;}
'''
tests='''int main(){
 assert(!isReleaseNewer(ReleaseChannel::Public,"v0.1.215"));
 assert(!isReleaseNewer(ReleaseChannel::Public,"v1.0.0"));
 assert(isReleaseNewer(ReleaseChannel::Public,"v1.0.1"));
 assert(isReleaseNewer(ReleaseChannel::Private,"v0.1.215"));
 assert(isReleaseNewer(ReleaseChannel::Staging,"v0.1.215-staging.224"));
 assert(!isReleaseNewer(ReleaseChannel::Public,"v1.0.0-staging.224"));
 assert(!isReleaseNewer(ReleaseChannel::Private,"garbage"));
 official=false;
 assert(isReleaseNewer(ReleaseChannel::Public,"v1.0.0"));
 assert(!isReleaseNewer(ReleaseChannel::Public,"v0.1.215"));
}
'''
with tempfile.TemporaryDirectory() as directory:
    p=Path(directory);(p/'test.cpp').write_text(prefix+source+tests)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
print('PASS explicit cross-channel version changes; same-channel rollback blocked')
