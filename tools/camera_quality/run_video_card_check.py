"""On-device check of full-screen video playback and its title card.

Run with Photos open on a video item (run_photos_ui_check.py leaves it there).
Plays the video with RB, records decoder/presentation timing for a few seconds,
pauses, and saves the paused frame (cover-cropped video + solid card) as PNG.
The fade itself cannot be captured: the device only exposes the video surface
while paused. Never deletes or uploads anything.

    python -X utf8 tools/camera_quality/run_video_card_check.py --port COM46
"""

import argparse
import datetime as dt
import re
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_photos_ui_check import fields, require, write_png  # noqa: E402
from run_settle_sweep import Device  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="COM46")
    parser.add_argument("--seconds", type=float, default=5.0, help="playback time to profile before pausing")
    args = parser.parse_args()
    folder = Path("logs") / dt.datetime.now().strftime("video-card-%Y%m%d-%H%M%S")
    folder.mkdir(parents=True, exist_ok=False)
    print(f"Output: {folder.resolve()}", flush=True)
    port = serial.Serial(None, 115200, timeout=0.1, write_timeout=5)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    with port, (folder / "serial.log").open("w", encoding="utf-8") as log:
        device = Device(port, log)
        device.drain(1)

        def collect(seconds):
            lines, deadline = [], time.monotonic() + seconds
            while True:
                try:
                    lines.append(device.line(deadline))
                except TimeoutError:
                    return lines

        def videos():
            device.send("videos status")
            for line in collect(1.5):
                if line.startswith("[videos] active="):
                    return fields(line)
            return {}

        def capture(name):
            device.send("morning frame")
            lines = device.wait_for("[morning-frame] done", timeout=25)
            rows = {}
            for line in lines:
                m = re.match(r"\[morning-frame\] row=(\d+) ([0-9a-f]+)", line)
                if m:
                    d = m.group(2)
                    rows[int(m.group(1))] = [int(d[i:i + 4], 16) for i in range(0, len(d), 4)]
            require(len(rows) == 240, f"Frame {name} incomplete: {len(rows)} rows")
            write_png(folder / f"{name}.png", 280, 240, rows)
            print(f"  shot {name}.png", flush=True)
            return rows

        device.send("photos status")
        photo = fields(device.wait_for("[photos] status ", timeout=5)[-1])
        require(photo.get("active") == "1" and photo.get("path", "").lower().endswith(".avi"),
                f"Open Photos on a video first: {photo}")
        print(f"  video {photo['path']}", flush=True)

        device.send("b")  # RB plays the video under the cursor
        log_lines = collect(args.seconds)
        state = videos()
        require(state.get("active") == "1" and state.get("playing") == "1", f"Video is not playing: {state}")
        decode = [l for l in log_lines if l.startswith("[video-native-decode]") or l.startswith("[video-present]")]
        for line in decode[:6]:
            print("  " + line, flush=True)
        (folder / "timing.txt").write_text("\n".join(decode), encoding="utf-8")
        print(f"  playing frame={state.get('frame')}/{state.get('total')} skipped={state.get('skipped')} "
              f"decode_max_ms={state.get('decode_max_ms')} present_max_ms={state.get('present_max_ms')}", flush=True)

        device.send("b")  # pause: the card returns, solid
        device.drain(0.8)
        state = videos()
        require(state.get("playing") == "0" and state.get("ended") == "0", f"Video did not pause: {state}")
        frame = capture("01-paused-card")

        # Cover crop: no black letterbox rows at the top/bottom, no side bars.
        def bar(pixels):
            return sum(p == 0 for p in pixels) > len(pixels) * 9 // 10
        require(not bar(frame[0]), "Top row is black: video is letterboxed")
        require(not bar([frame[y][0] for y in range(40, 160)]), "Left column is black: video is pillarboxed")
        print("  ok   cover crop (no bars)", flush=True)

        device.send("b")  # resume, then let the card fade before leaving
        device.drain(2.0)
        device.send("a")  # back to Photos
        device.drain(2.5)
        device.send("photos status")
        back = fields(device.wait_for("[photos] status ", timeout=5)[-1])
        require(back.get("active") == "1" and back.get("path") == photo["path"], f"Did not return to the video: {back}")
        print("  ok   back to Photos on the same video", flush=True)
    print(f"PASS: frames and timing in {folder}", flush=True)


if __name__ == "__main__":
    main()
