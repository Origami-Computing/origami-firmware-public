"""Run independently restarted VGA settling trials and retrieve CRC-checked JPEGs."""

import argparse
import base64
import collections
import csv
import datetime as dt
import html
import io
import json
from pathlib import Path, PurePosixPath
import random
import re
import statistics
import time
import zlib

import serial


FIELDS = "run,setting,repeat,discarded,prep_ms,init_ms,settle_ms,acquire_ms,ready_ms,frame_dma_start_us,write_ms,width,height,bytes,saved,restored,path".split(",")
SETTINGS = [0, 1, 2, 3, 5, 8, 12, -1]


class Device:
    def __init__(self, port, log):
        self.port = port
        self.log = log
        self.pending = bytearray()
        self.lines = collections.deque()

    def send(self, command):
        self.log.write(f"{dt.datetime.now().isoformat()} HOST {command}\n")
        self.log.flush()
        self.port.write((command + "\n").encode("ascii"))
        self.port.flush()

    def line(self, deadline):
        while time.monotonic() < deadline:
            if self.lines:
                return self.lines.popleft()
            self.pending.extend(self.port.read(self.port.in_waiting or 1))
            while b"\n" in self.pending:
                raw, _, rest = self.pending.partition(b"\n")
                self.pending = bytearray(rest)
                line = raw.decode("utf-8", errors="replace").rstrip("\r")
                self.log.write(f"{dt.datetime.now().isoformat()} {line}\n")
                self.log.flush()
                self.lines.append(line)
        raise TimeoutError("Timed out waiting for device response")

    def wait_for(self, prefix, timeout=30):
        deadline = time.monotonic() + timeout
        received = []
        while True:
            line = self.line(deadline)
            received.append(line)
            if line.startswith("[camera-settle] ERR") or "Guru Meditation" in line:
                raise RuntimeError(line)
            if line.startswith(prefix):
                return received

    def drain(self, seconds):
        try:
            deadline = time.monotonic() + seconds
            while True:
                self.line(deadline)
        except TimeoutError:
            pass

    def download(self, remote, destination, timeout=60):
        self.send(f"sdget {remote}")
        data = bytearray()
        expected_size = None
        deadline = time.monotonic() + timeout
        while True:
            line = self.line(deadline)
            # Firmware logging can precede a complete transport record on the
            # same line. Base64 contains no spaces, so this marker is unambiguous;
            # packet length, offsets and the final CRC still validate all bytes.
            marker = line.find("SDGET ")
            if marker >= 0:
                line = line[marker:]
            if line.startswith("SDGET ERR"):
                raise RuntimeError(line)
            if line.startswith("SDGET BEGIN "):
                match = re.fullmatch(r"SDGET BEGIN size=(\d+) path=(.+)", line)
                if not match or match[2] != remote:
                    raise RuntimeError(f"Unexpected download: {line}")
                expected_size = int(match[1])
            elif line.startswith("SDGET DATA "):
                match = re.fullmatch(r"SDGET DATA offset=(\d+) bytes=(\d+) (.+)", line)
                if not match or expected_size is None or int(match[1]) != len(data):
                    raise RuntimeError(f"Invalid download packet (expected offset {len(data)})")
                chunk = base64.b64decode(match[3], validate=True)
                if len(chunk) != int(match[2]):
                    raise RuntimeError("Invalid chunk length")
                data.extend(chunk)
            elif line.startswith("SDGET END "):
                match = re.fullmatch(r"SDGET END size=(\d+) crc=([0-9a-fA-F]+) path=(.+)", line)
                if (not match or match[3] != remote or expected_size != len(data)
                        or int(match[1]) != len(data)
                        or int(match[2], 16) != zlib.crc32(data)):
                    raise RuntimeError(f"Download CRC/size mismatch: {remote}")
                destination.write_bytes(data)
                return


def write_report(folder, rows):
    groups = collections.defaultdict(list)
    for row in rows:
        groups[row["setting"]].append(row)
    summary = []
    bands = []
    for setting in [f"D{n:02}" if n >= 0 else "T600" for n in SETTINGS]:
        group = sorted(groups[setting], key=lambda row: int(row["repeat"]))
        valid = [r for r in group if r["saved"] == "1"]
        if not valid:
            continue
        avg = lambda key: statistics.mean(float(r[key]) for r in valid)
        label = "600 ms baseline" if setting == "T600" else f"Discard {int(setting[1:])} frames"
        summary.append(f"<tr><td>{label}</td><td>{len(valid)}</td>"
                       f"<td>{avg('init_ms'):.0f}</td><td>{avg('settle_ms'):.0f}</td>"
                       f"<td>{avg('acquire_ms'):.0f}</td><td>{avg('ready_ms'):.0f}</td>"
                       f"<td>{avg('write_ms'):.0f}</td></tr>")
        images = []
        for row in group:
            name = html.escape(PurePosixPath(row["path"]).name, quote=True)
            images.append(f'<figure><a href="{name}"><img src="{name}" alt="{name}"></a>'
                          f'<figcaption><strong>{setting} / repeat {row["repeat"]}</strong><br>'
                          f'{row["ready_ms"]} ms to frame; {row["settle_ms"]} ms settling<br>'
                          f'<small>{name}</small></figcaption></figure>')
        bands.append(f"<section><h2>{label}</h2><div class='photos'>{''.join(images)}</div></section>")
    page = """<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Camera frame-discard comparison</title><style>
*{box-sizing:border-box}body{font:15px/1.5 system-ui,sans-serif;margin:24px;color:#182220;background:#fff}
main{max-width:1400px;margin:auto}h1{font-size:26px}h2{font-size:20px}section{border-top:1px solid #ccd5d1;padding:16px 0}
.photos{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:18px}figure{margin:0;min-width:0}
img{width:100%;height:auto;aspect-ratio:4/3;object-fit:contain;background:#eee}small{overflow-wrap:anywhere}
.table{overflow-x:auto}table{border-collapse:collapse;margin:20px 0}td,th{padding:8px 14px;border-bottom:1px solid #ddd;text-align:left}
@media(max-width:750px){.photos{grid-template-columns:1fr}body{margin:14px}}
</style><main><h1>Camera frame-discard comparison</h1>
<p>VGA 640 x 480, JPEG quality 6. Three independent camera restarts per setting, in shuffled rounds.
Original JPEGs are unchanged. T600 uses the current 600 ms settle loop, including its 20 ms pauses.</p>
<p>Times below are device-side milliseconds. Frame-ready starts at the simulated shutter callback,
excludes filename/manifest I/O and ends before JPEG saving. It is not a physical button-to-exposure measurement.</p>
<div class="table"><table><thead><tr><th>Setting</th><th>Photos</th><th>Init</th><th>Settle</th>
<th>Acquire</th><th>Frame ready</th><th>SD write</th></tr></thead><tbody>"""
    page += "".join(summary) + "</tbody></table></div>" + "".join(bands) + "</main></html>"
    (folder / "comparison.html").write_text(page, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM20")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--download-only", type=Path, help="Resume retrieval from an existing results.csv")
    args = parser.parse_args()
    run_id = int(time.time())
    folder = args.download_only or args.output or Path("logs") / dt.datetime.now().strftime("camera-settle-%Y%m%d-%H%M%S")
    if not args.download_only:
        folder.mkdir(parents=True, exist_ok=False)
    print(f"Output: {folder.resolve()}", flush=True)
    rows = []
    with serial.Serial(args.port, 115200, timeout=0.1, write_timeout=5) as port, (folder / "serial.log").open("a", encoding="utf-8") as log:
        device = Device(port, log)
        device.drain(3)
        if args.download_only:
            with (folder / "results.csv").open(newline="") as csv_file:
                rows = list(csv.DictReader(csv_file))
            run_id = int(rows[0]["run"])
        else:
            device.send("camera open")
            device.wait_for("[camera-settle] OPEN")
            rng = random.Random(run_id)
            order = []
            for repeat in range(1, 4):
                settings = SETTINGS.copy()
                rng.shuffle(settings)
                order.extend((frames, repeat) for frames in settings)
            (folder / "plan.json").write_text(json.dumps({"run": run_id, "port": args.port, "resolution": "VGA", "order": order}, indent=2), encoding="utf-8")
            with (folder / "results.csv").open("w", newline="") as csv_file:
                writer = csv.DictWriter(csv_file, fieldnames=FIELDS)
                writer.writeheader()
                for index, (frames, repeat) in enumerate(order, 1):
                    device.drain(1.2)
                    device.send(f"camera settle {run_id} {frames} {repeat}")
                    received = device.wait_for("[camera-settle] END", timeout=35)
                    row_lines = [s.removeprefix("[camera-settle] ROW ") for s in received if s.startswith("[camera-settle] ROW ")]
                    if len(row_lines) != 1:
                        raise RuntimeError("Missing trial row")
                    row = next(csv.DictReader(io.StringIO(row_lines[0]), fieldnames=FIELDS))
                    expected = f"D{frames:02}" if frames >= 0 else "T600"
                    if row["run"] != str(run_id) or row["setting"] != expected or row["repeat"] != str(repeat):
                        raise RuntimeError("Mismatched trial response")
                    writer.writerow(row)
                    csv_file.flush()
                    rows.append(row)
                    print(f"{index}/24 {row['setting']} R{repeat}: ready={row['ready_ms']}ms settle={row['settle_ms']}ms saved={row['saved']}", flush=True)
                    if "ok=1 " not in received[-1]:
                        raise RuntimeError(received[-1])
        device.send("home")
        device.drain(3)
        device.download(f"/settle/{run_id}.csv", folder / "device-manifest.csv")
        with (folder / "device-manifest.csv").open(newline="") as csv_file:
            # Early diagnostic firmware omitted the header on newly opened SD files.
            first = csv_file.readline().strip()
            csv_file.seek(0)
            device_rows = list(csv.DictReader(csv_file, fieldnames=None if first == ",".join(FIELDS) else FIELDS))
            if device_rows != rows:
                raise RuntimeError("SD manifest does not match serial trial results")
        for index, row in enumerate(rows, 1):
            if row["saved"] != "1":
                continue
            local = folder / PurePosixPath(row["path"]).name
            device.download(row["path"], local)
            if local.stat().st_size != int(row["bytes"]):
                raise RuntimeError(f"JPEG size mismatch: {local}")
            print(f"Downloaded {index}/{len(rows)} {local.name} (CRC verified)", flush=True)
    write_report(folder, rows)
    print(f"Complete: {(folder / 'comparison.html').resolve()}", flush=True)


if __name__ == "__main__":
    main()
