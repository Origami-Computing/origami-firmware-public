"""Ordinary Camera settings controls for serial Device test fixtures."""
import time

def scene_lines(device):
    device.send('scene');lines=[];deadline=time.monotonic()+1.5
    try:
        while True:lines.append(device.line(deadline))
    except TimeoutError:return lines

def select_video_with_audio(device):
    # Camera starts in Photo on each activation. Settings retains the workflow
    # while releasing camera hardware; selecting video must precede pressing B.
    device.send('cp');device.drain(.35);device.send('cr')
    device.wait_for('[cam] Camera stopped',15);device.drain(.5)
    lines=scene_lines(device)
    assert any('Recording  Photo' in line for line in lines), 'Expected Photo default'
    autofocus=any('Autofocus  ' in line for line in lines)
    device.send('rot -30');device.drain(.3)
    device.send('rot '+str(3 if autofocus else 2));device.drain(.3)
    device.send('c');device.drain(.5);device.send('rot 2');device.drain(.3)
    device.send('c');device.drain(.5)
    lines=scene_lines(device)
    assert any('Recording  Video + mic' in line for line in lines)
    device.send('a');device.wait_for('[cam] Viewfinder started',25)
