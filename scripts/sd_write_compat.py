"""Opt-in build overlay for the pinned Arduino SD multi-block stop timing.

Generates a replacement translation unit inside the build directory. Never edits
installed framework files. Reject unknown source bytes rather than guessing at
an SDK upgrade. The ordinary target does not load this script.
"""
from hashlib import sha256
from pathlib import Path

PINNED_SHA256 = '0401e23009f06e3a418470a144403e89727f3d007b1d16d429dc370ce8bdc5f0'
BEFORE = '''                sdStop(pdrv);
                sdDeselectCard(pdrv);
'''
AFTER = '''                sdStop(pdrv);
                // NBR allows one byte before the card asserts busy after STOP.
                // Clock that byte before polling; otherwise 0xFF can be mistaken
                // for completion and CMD13 sent while programming has begun.
                card->spi->transfer(0xFF);
                if (!sdWait(pdrv, 500)) {
                    sdDeselectCard(pdrv);
                    return false;
                }
                sdDeselectCard(pdrv);
'''

def patched_source(source: bytes) -> bytes:
    if sha256(source).hexdigest() != PINNED_SHA256:
        raise RuntimeError('SD compatibility overlay requires the reviewed Arduino 2.0.17 source')
    text = source.decode('utf-8')
    if text.count(BEFORE) != 1:
        raise RuntimeError('SD multi-block stop boundary changed')
    return text.replace(BEFORE, AFTER).encode('utf-8')

def install(env):
    def replace(build_env, node):
        source = Path(node.srcnode().get_abspath())
        patched = patched_source(source.read_bytes())
        target = Path(build_env.subst('$BUILD_DIR')) / 'sd-compat' / 'sd_diskio.cpp'
        target.parent.mkdir(parents=True, exist_ok=True)
        if not target.exists() or target.read_bytes() != patched:
            target.write_bytes(patched)
        print('SD compatibility trial: pinned multi-block STOP busy timing')
        return build_env.File(str(target))
    env.AddBuildMiddleware(replace, '*/libraries/SD/src/sd_diskio.cpp')

# Importable without SCons for focused source/hash/timing tests.
if 'Import' in globals():
    Import('env')
    install(env)
