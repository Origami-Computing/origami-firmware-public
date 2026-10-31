"""Capture HD and sensor-maximum modes through UI; restore the capture preference.

Saves/queues two normal photos. Keeps raw logs and JPEGs private in --output.
Writes the original preference to restore.json before changing it.
"""
import argparse,base64,json,re,time,zlib
from pathlib import Path
from morning_alarm_device_check import Device

LABELS=['VGA 640x480','HD 1280x720','Max resolution']
def dimensions(data):
    assert data[:2]==b'\xff\xd8' and data[-2:]==b'\xff\xd9'
    pos=2
    while pos<len(data):
        assert data[pos]==255
        while data[pos]==255:pos+=1
        marker=data[pos];pos+=1
        if marker in (0xd8,0xd9) or 0xd0<=marker<=0xd7:continue
        length=int.from_bytes(data[pos:pos+2],'big')
        if marker in (0xc0,0xc1,0xc2):return (int.from_bytes(data[pos+5:pos+7],'big'),int.from_bytes(data[pos+3:pos+5],'big'))
        assert length>=2 and marker!=0xda,'JPEG dimensions missing'
        pos+=length
    raise AssertionError('JPEG dimensions missing')

def readback(d,output,rows):
    for row in rows:
        for attempt in range(3):
            reply=d.command('sdget '+row['path'],.3);end=time.monotonic()+45
            while 'SDGET END ' not in reply and time.monotonic()<end:reply+=d.read(.3)
            reply+=d.read(.5)
            assert 'SDGET END ' in reply,'Readback did not finish; no automatic replay'
            try:
                data=bytearray()
                for offset,count,encoded in re.findall(r'SDGET DATA offset=(\d+) bytes=(\d+) ([A-Za-z0-9+/=]+)',reply):
                    chunk=base64.b64decode(encoded,validate=True)
                    assert int(offset)==len(data) and int(count)==len(chunk)
                    data.extend(chunk)
                finish=re.search(r'SDGET END size=(\d+) crc=([0-9a-f]+)',reply)
                assert finish and int(finish[1])==len(data) and int(finish[2],16)==zlib.crc32(data)
                row['dimensions']=dimensions(data);row['bytes']=len(data)
                (output/Path(row['path']).name).write_bytes(data)
                break
            except (ValueError,AssertionError):
                if attempt==2:raise
                print('Retry completed read-only transfer after corrupt/missing serial chunk',flush=True)
                d.read(1)

def run(port,output):
    d=Device(port,output);original=None;changed=False;rows=[]
    def until(command,marker,timeout=35):
        reply=d.command(command,.3);end=time.monotonic()+timeout
        while marker not in reply and time.monotonic()<end:reply+=d.read(.3)
        assert marker in reply,'Missing '+marker+'; inspect private log'
        return reply
    def settings():
        d.command('cp',.4);until('cr','[cam] Camera stopped');d.read(1)
        return until('scene','Capture  ')
    def preset(scene):
        return next(i for i,label in enumerate(LABELS) if 'Capture  '+label.split()[0]+' ' in scene)
    def select(index):
        # Called only in settings and outside edit mode.
        has_autofocus = 'Autofocus  ' in until('scene','Capture  ')
        d.command('rot -30');d.command('rot '+str(2 if has_autofocus else 1));d.command('c')
        d.command('rot -30')
        if index:d.command('rot '+str(index))
        d.command('c')
        assert preset(until('scene','Capture  '))==index,'Capture preference did not commit'
    try:
        d.read(45);d.command('home',2);until('camera open','[cam] Viewfinder started')
        original=preset(settings())
        (output/'restore.json').write_text(json.dumps({'original_index':original,'original_label':LABELS[original],'restored':False},indent=2))
        for index in [1,2]:
            changed=True;select(index);until('a','[cam] Viewfinder started');d.read(1)
            shot=until('b','[queue] Enqueued ',45)
            end=time.monotonic()+25
            while '[camera-vga] preview samples=' not in shot and time.monotonic()<end:shot+=d.read(.5)
            assert 'captureToFile: saved ' in shot and '[camera-vga] preview samples=' in shot,'Save/preview recovery incomplete'
            match=re.search(r'\[queue\] Enqueued (\S+\.jpg)',shot)
            assert match,'Photo path missing'
            rows.append({'requested':LABELS[index],'path':'/photos/'+Path(match[1]).name})
            (output/'captures.json').write_text(json.dumps(rows,indent=2))
            settings()
        select(original);changed=False
        (output/'restore.json').write_text(json.dumps({'original_index':original,'original_label':LABELS[original],'restored':True},indent=2))
        until('a','[cam] Viewfinder started');d.command('home',2)
        readback(d,output,rows)
        assert tuple(rows[0]['dimensions'])==(1280,720),'HD image dimensions differ from the selected mode'
        assert tuple(rows[1]['dimensions']) in ((2048,1536),(2560,1920)), 'Maximum-mode image unexpectedly fell back below sensor maximum'
        (output/'results.json').write_text(json.dumps(rows,indent=2))
        print('PASS high-resolution capture/save/queue, preview recovery and CRC readback; original preference restored',flush=True)
        for row in rows:print(row['requested'],'->',row['dimensions'],row['bytes'],'bytes',flush=True)
    finally:
        try:
            if changed and original is not None:
                d.command('home',2);until('camera open','[cam] Viewfinder started');settings();select(original)
                (output/'restore.json').write_text(json.dumps({'original_index':original,'original_label':LABELS[original],'restored':True},indent=2))
        finally:
            try:d.command('home',2)
            finally:d.close()
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port',required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--recover-from',type=Path);a=p.parse_args()
    if a.recover_from:
        assert json.loads((a.recover_from/'restore.json').read_text())['restored'],'Restore preferences before readback recovery'
        rows=json.loads((a.recover_from/'captures.json').read_text());d=Device(a.port,a.output)
        try:
            d.read(45);readback(d,a.output,rows)
            (a.output/'results.json').write_text(json.dumps(rows,indent=2))
            print('PASS recovered CRC-checked JPEG readbacks',flush=True)
            for row in rows:print(row['requested'],'->',row['dimensions'],row['bytes'],'bytes',flush=True)
        finally:d.close()
    else:run(a.port,a.output)
