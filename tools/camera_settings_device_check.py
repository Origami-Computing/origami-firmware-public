"""Check Camera configuration edits/re-entry, restoring mode, AF and exposure.

No photos are taken. Raw logs and original values stay in --output. Tests one
signed manual field plus mode/AF; the host policy test covers every saved field.
"""
import argparse
import json
import re
import time
from pathlib import Path
from morning_alarm_device_check import Device


def run(port, output):
    d = Device(port, output)
    original = None
    exposure = None
    dirty = False

    def until(command, marker, timeout=30):
        reply = d.command(command, .4)
        end = time.monotonic()+timeout
        while marker not in reply and time.monotonic()<end:
            reply += d.read(.3)
        assert marker in reply, 'Missing '+marker+'; inspect private log'
        return reply

    def scene():
        return until('scene', '[scene] nodes=')

    def top_values():
        text = scene()
        label = re.search(r'text="Camera mode  ([^"]+)"', text)[1]
        mode = next(i for i, prefix in enumerate(['Def','Bri','Dar','Man']) if label.startswith(prefix))
        af_match = re.search(r'text="Autofocus  (ON|OFF)"', text)
        af = af_match[1] == 'ON' if af_match else None
        capture = re.search(r'text="Capture  ([^"]+)"', text)[1]
        return {'mode': mode, 'autofocus': af, 'capture_label': capture}

    def row(index):
        d.command('rot -30')
        if index: d.command('rot '+str(index))

    def open_top():
        d.command('home',2)
        until('camera open','[cam] Viewfinder started')
        d.read(1)
        d.command('cp',.4)
        until('cr','[cam] Camera stopped')
        d.read(1)
        return top_values()

    def mode(value):
        row(0); d.command('c'); d.command('rot -30')
        if value: d.command('rot '+str(value))
        d.command('c')
        # Committing Manual goes straight to its subview.
        if value == 3: assert 'Exposure' in scene()
        else: assert top_values()['mode'] == value

    def manual_exposure():
        row(0)
        return int(re.search(r'text="Exposure\s+([+-]?\d+)"', scene())[1])

    def set_exposure(value):
        row(0); d.command('c'); d.command('rot -30')
        if value+2: d.command('rot '+str(value+2))
        d.command('c')
        assert manual_exposure() == value

    def restore():
        if not dirty: return
        open_top()
        if exposure is not None:
            mode(3); set_exposure(exposure); d.command('a')
        if top_values()['autofocus'] != original['autofocus']:
            row(1); d.command('c'); d.command('rot 1'); d.command('c')
        mode(original['mode'])
        if original['mode'] == 3: d.command('a')
        assert top_values() == original
        assert open_top() == original  # Verify persisted restoration after activation.
        if exposure is not None and original['mode'] == 3:
            row(4 if original['autofocus'] is not None else 3);d.command('c');assert manual_exposure()==exposure;d.command('a')
        (output/'restore.json').write_text(json.dumps({'original':original,'exposure':exposure,'restored':True},indent=2)+'\n')

    try:
        d.read(20)
        original = open_top()
        (output/'restore.json').write_text(json.dumps({'original':original,'restored':False},indent=2)+'\n')
        dirty = True
        # AF changes save individually and survive activation.
        if original['autofocus'] is not None:
            row(1);d.command('c');d.command('rot 1');d.command('c')
            assert open_top()['autofocus'] != original['autofocus']
        mode(3)
        exposure = manual_exposure()
        (output/'restore.json').write_text(json.dumps({'original':original,'exposure':exposure,'restored':False},indent=2)+'\n')
        delta = -1 if exposure == 2 else 1
        d.command('c');d.command('rot '+str(delta));d.command('a')
        assert manual_exposure() == exposure
        set_exposure(exposure+delta)
        d.capture('manual-edited')
        d.command('a')  # Manual to settings.
        until('a','[cam] Viewfinder started')  # Apply the manual profile to real preview.
        d.read(2)
        values = open_top()
        assert values['mode']==3
        assert values['autofocus'] == (None if original['autofocus'] is None else not original['autofocus'])
        assert values['capture_label']==original['capture_label']
        row(4 if original['autofocus'] is not None else 3);d.command('c');assert manual_exposure()==exposure+delta
        restore();dirty=False
        d.capture('settings-restored')
        (output/'summary.json').write_text(json.dumps({'complete':True,'restored':True,
            'checks':[('AF save/re-entry' if original['autofocus'] is not None else 'AF row absent/re-entry'),'Manual mode save/re-entry','Exposure cancel','Exposure save/re-entry','Manual preview restart','Original settings restored'],
            'scope':'No photos; host checks cover all other persisted fields.'},indent=2)+'\n')
        print('PASS Camera capability-aware settings, mode/exposure save, cancel, re-entry and preview restart; original settings restored',flush=True)
    finally:
        try:
            restore()
        finally:
            try:d.command('home',2)
            finally:d.close()


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port',required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();run(a.port,a.output)
