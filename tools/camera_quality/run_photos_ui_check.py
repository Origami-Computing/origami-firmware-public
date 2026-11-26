"""Non-destructive on-device check of the full-screen Photos UI and controls.

Opens Photos over the developer serial console, drives the dial and buttons,
checks `photos status` after each step, and saves the real framebuffer as PNGs
(via the `morning frame` canvas dump) in logs/photos-ui-<time>/.

Never deletes: the delete confirmation is opened and cancelled. Uploading adds
a real photo to the upload queue (and so to the account), so it only runs with
--upload. Playing a video only runs with --video.

    python -X utf8 tools/camera_quality/run_photos_ui_check.py --port COM46
"""

import argparse
import datetime as dt
import re
import struct
import sys
import time
import zlib
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_settle_sweep import Device  # noqa: E402


def fields(line):
    return dict(re.findall(r"(\w+)=([^ ]+)", line))


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_png(path, width, height, rows):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        for pixel in rows[y]:
            raw += bytes((((pixel >> 11) & 31) * 255 // 31, ((pixel >> 5) & 63) * 255 // 63, (pixel & 31) * 255 // 31))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="COM46")
    parser.add_argument("--upload", action="store_true", help="also queue the current photo for upload")
    parser.add_argument("--video", action="store_true", help="also play a video with RB and return")
    args = parser.parse_args()
    folder = Path("logs") / dt.datetime.now().strftime("photos-ui-%Y%m%d-%H%M%S")
    folder.mkdir(parents=True, exist_ok=False)
    print(f"Output: {folder.resolve()}", flush=True)

    port = serial.Serial(None, 115200, timeout=0.1, write_timeout=5)
    port.dtr = False
    port.rts = False
    port.port = args.port
    port.open()
    results = []
    with port, (folder / "serial.log").open("w", encoding="utf-8") as log:
        device = Device(port, log)
        device.drain(2)

        def status():
            device.drain(0.1)
            device.send("photos status")
            return fields(device.wait_for("[photos] status ", timeout=5)[-1])

        def state(command=None, settle=0.5, timeout=20, **expected):
            if command:
                device.send(command)
                device.drain(settle)
            deadline = time.monotonic() + timeout
            row = {}
            while time.monotonic() < deadline:
                try:
                    row = status()
                except TimeoutError:
                    continue
                if row.get("ready") == "1" and all(row.get(k) == str(v) for k, v in expected.items()):
                    results.append((command or "-", "ok", row))
                    print(f"  ok   {command or '(status)':10} {' '.join(f'{k}={row.get(k)}' for k in ('index','count','menu','confirm','action','date','notice'))}", flush=True)
                    return row
                device.drain(0.2)
            results.append((command or "-", "FAIL", row))
            raise RuntimeError(f"After '{command}': expected {expected}, last status {row}")

        def capture(name):
            device.drain(0.3)
            device.send("morning frame")
            lines = device.wait_for("[morning-frame] done", timeout=20)
            size = next(l for l in lines if l.startswith("[morning-frame] size="))
            width, height = map(int, size.split("=")[1].split(","))
            rows = {}
            for line in lines:
                m = re.match(r"\[morning-frame\] row=(\d+) ([0-9a-f]+)", line)
                if m:
                    data = m.group(2)
                    rows[int(m.group(1))] = [int(data[i:i + 4], 16) for i in range(0, len(data), 4)]
            require(len(rows) == height, f"Frame {name} incomplete: {len(rows)}/{height} rows")
            write_png(folder / f"{name}.png", width, height, rows)
            print(f"  shot {name}.png", flush=True)
            return rows

        # A freshly flashed/reset board ignores input until the boot animation has
        # handed off to the home eyes; wait for that (or give up waiting after 25 s).
        try:
            device.wait_for("[boot-timing] event=kami_first_flush_done", timeout=25)
        except TimeoutError:
            pass
        device.drain(2)

        def selected_menu_label():
            device.drain(0.3)
            device.send("scene")
            lines = device.wait_for("[scene] nodes=", timeout=5)
            deadline = time.monotonic() + 1.5
            while True:  # keep the node lines (drain() would discard them)
                try:
                    lines.append(device.line(deadline))
                except TimeoutError:
                    break
            selected = None
            for line in lines:
                m = re.search(r"id=list\.row\.(\d+)\.bg .* fill=0x0000", line)
                if m:
                    selected = m.group(1)
            for line in lines:
                m = re.search(r'id=list\.row\.(\d+)\.label .* text="([^"]*)"', line)
                if m and m.group(1) == selected:
                    return m.group(2)
            return None

        # Open Photos from home: menu, top item, then down until Photos is selected
        # (some boards show an extra "Update Firmware" row above Camera).
        first = None
        for attempt in range(3):
            for command in ("home", "menu", "rot -24"):
                device.send(command)
                device.drain(1.2)
            for _ in range(6):
                if selected_menu_label() == "Photos":
                    break
                device.send("down")
                device.drain(0.8)
            try:
                first = state("c", settle=2, timeout=30, active=1, menu=0)
                break
            except RuntimeError as error:
                results.pop()
                print(f"  retry opening Photos ({error})", flush=True)
        require(first is not None, "Could not open Photos from the menu")
        count = int(first["count"])
        require(count > 0, "This check needs at least one photo or video on the SD card")
        frame = capture("01-browse")
        # Full screen: no white status strip, and cover scaling leaves no black
        # letterbox bars at the top/bottom or pillarbox bars at the sides.
        top = [p for y in range(3) for p in frame[y]]
        require(sum(p == 0xFFFF for p in top) < len(top) // 2, "Top rows look like the white status bar")
        def bar(pixels):
            return sum(p == 0 for p in pixels) > len(pixels) * 9 // 10
        require(not bar(frame[0]), "Row 0 is black: the media is letterboxed")
        require(not bar([frame[y][0] for y in range(40, 200)]), "Column 0 is black: the media is pillarboxed")
        require(not bar([frame[y][279] for y in range(40, 200)]), "Column 279 is black: the media is pillarboxed")

        # Each item shows its own index; photos report their EXIF capture date
        # (or "-" when the photo has none), videos never claim one.
        dated = 0
        for step in range(min(count, 6)):
            row = state("down" if step else None, index=step + 1)
            if row.get("path", "").lower().endswith(".avi"):
                require(row.get("date") == "-", f"Video reported a capture date: {row}")
            elif row.get("date") != "-":
                dated += 1
                if dated == 1:
                    capture("02-photo-with-date")
        print(f"  dated photos among the first items: {dated}", flush=True)
        if min(count, 6) > 1:  # browsing wraps, so turn back by the exact distance
            state(f"rot -{min(count, 6) - 1}", settle=1.0, index=1)

        # Find the first photo and the first video among the first few items.
        def find(kind_video, limit=min(count, 12)):
            for _ in range(limit):
                row = status()
                if row.get("path", "").lower().endswith(".avi") == kind_video:
                    return row
                state("down")
            return None

        photo = find(False)
        if photo:
            index = int(photo["index"])
            # RB on a photo does nothing.
            state("b", settle=0.8, index=index, menu=0, path=photo["path"])
            # C opens the sheet with Upload first, uploaded or not (re-upload).
            sheet = state("c", menu=1, confirm=0, index=index, action="upload", rows=2)
            require(sheet.get("cloud") in ("yes", "no"), f"Sheet did not report cloud state: {sheet}")
            capture("04-sheet-photo")
            state("rot -24", menu=1, action=sheet["action"])  # clamps at the top
            state("rot 24", menu=1, action="delete")           # clamps at Delete
            capture("05-sheet-delete-focus")
            state("c", menu=1, confirm=1, action="cancel")
            capture("06-confirm-cancel")
            state("down", confirm=1, action="delete")
            capture("07-confirm-delete-focus")
            state("up", confirm=1, action="cancel")
            state("c", menu=1, confirm=0, action="delete")      # Cancel returns to the sheet
            state("c", menu=1, confirm=1, action="cancel")
            state("a", menu=1, confirm=0)                       # LB backs out of confirm
            after = state("a", menu=0, index=index, count=count, path=photo["path"])
            require(after["pixels"] == photo["pixels"], "Closing the sheet did not restore the same image")
            capture("08-sheet-closed")
        else:
            print("  (no photo found in the first items; photo checks skipped)", flush=True)

        if args.upload:
            # Find the first photo the cloud has not confirmed, so nothing is uploaded twice.
            target = None
            for _ in range(count):
                row = status()
                if not row.get("path", "").lower().endswith(".avi"):
                    opened = state("c", menu=1, path=row["path"])
                    if opened.get("cloud") == "no":
                        target = opened
                        break
                    state("a", menu=0, path=row["path"])
                state("down", settle=0.6)
            require(target is not None, "No photo without an upload receipt was found")
            name = target["path"].rsplit("/", 1)[-1]
            print(f"  queueing {target['path']}", flush=True)
            capture("09-sheet-upload-focus")
            device.send("c")  # run the Upload row
            device.drain(0.4)
            row = status()
            require(row.get("menu") == "0" and row.get("notice") in ("Added", "Already") and row.get("path") == target["path"],
                    f"Upload did not close the sheet and show the queued notice: {row}")
            print(f"  ok   upload     menu=0 notice={row.get('notice')} (still browsing {name})", flush=True)
            capture("10-upload-notice")
            time.sleep(2.2)
            cleared = state(menu=0, path=target["path"])
            require(cleared.get("notice") == "-", f"Notice did not clear: {cleared}")
            capture("11-notice-cleared")
            # Upload again: the item is already queued.
            state("c", menu=1, action="upload", path=target["path"])
            device.send("c")
            device.drain(0.4)
            again = status()
            print(f"  ok   re-upload  notice={again.get('notice')}", flush=True)
            # The queue owns the transfer; browsing pauses it, so check it after leaving.
            device.send("home")
            device.drain(1)
            device.send("queue status")
            deadline = time.monotonic() + 4
            entries = []
            while True:
                try:
                    line = device.line(deadline)
                except TimeoutError:
                    break
                if name in line or line.startswith("QUEUE STATUS"):
                    entries.append(line)
            print("  queue: " + (" | ".join(entries) if entries else "(no line mentions the file)"), flush=True)
            (folder / "queue-after-upload.txt").write_text("\n".join(entries), encoding="utf-8")

        if args.video:
            video = find(True)
            if video:
                # C on a video opens Play / (Upload) / Delete with Play first.
                vsheet = state("c", menu=1, confirm=0, action="play", path=video["path"])
                require(vsheet["rows"] in ("2", "3"), f"Unexpected video sheet rows {vsheet['rows']}")
                capture("11-sheet-video")
                state("a", menu=0, path=video["path"])
                device.send("b")
                device.drain(3)
                row = status()
                require(row.get("active") == "0", f"RB on a video did not open the player: {row}")
                print("  ok   b          video player opened", flush=True)
                state("a", settle=3, timeout=40, active=1, menu=0, path=video["path"])
            else:
                print("  (no video found; video check skipped)", flush=True)

    failed = [r for r in results if r[1] != "ok"]
    print(f"{'PASS' if not failed else 'FAIL'}: {len(results)} checks, frames in {folder}", flush=True)


if __name__ == "__main__":
    main()
