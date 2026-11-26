"""Build and render the Photos app design study (host only, no device).

Compiles photos_preview.cpp against the firmware's font data, renders every
screen at native 280x240, and writes PNGs plus a side-by-side review page to
.cache/photos-preview/. Uses $CXX, or `python -m ziglang c++` if available.

    python tools/mp3-preview/photos_preview.py
"""
import base64
import os
import shlex
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = ROOT / ".cache/photos-preview"
BINARY = OUT / ("photos_preview.exe" if os.name == "nt" else "photos_preview")


def compiler():
    if os.environ.get("CXX"):
        return shlex.split(os.environ["CXX"])
    for name in ("c++", "g++", "clang++"):
        if shutil.which(name):
            return [name]
    try:
        import ziglang  # noqa: F401  (pip install ziglang==0.13.0)
        return [sys.executable, "-m", "ziglang", "c++"]
    except ImportError:
        sys.exit("No C++ compiler: set CXX or `pip install ziglang==0.13.0`.")


def build():
    OUT.mkdir(parents=True, exist_ok=True)
    subprocess.run(compiler() + [
        "-std=c++17", "-O2", "-DORIGAMI_INSTRUMENT_FONT",
        "-I" + str(HERE / "host"), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "lib/TB_TFT_eSPI"),
        str(HERE / "photos_preview.cpp"), "-o", str(BINARY)], check=True)


def ppm_to_png(ppm: Path, png: Path):
    data = ppm.read_bytes()
    header, rest = data.split(b"\n", 3)[:3], data.split(b"\n", 3)[3]
    width, height = map(int, header[1].split())
    rows = b"".join(b"\x00" + rest[y * width * 3:(y + 1) * width * 3] for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    png.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    ppm.unlink()


SECTIONS = [
    ("Browsing a photo", "Today: 38px status bar, white side bars, and a 5x7 pixel-font footer with file name and index. "
     "Proposed: the photo fills the panel edge to edge on black. Turning the dial shows a quiet position readout in the "
     "letterbox under the picture (cleared with one fillRect, no JPEG re-decode).",
     ["before-photo"], ["after-photo", "after-photo-scrolling"]),
    ("Browsing a video", "Today: a thin outline triangle that disappears on bright frames, plus a black 'B: PLAY 0:12' bar "
     "stacked on the footer. Proposed: a solid play badge (press the dial to play) and a small duration chip.",
     ["before-video"], ["after-video"]),
    ("Acting on an item", "Today: three boxed rows in the 5x7 font over the photo, defaulting to 'Esc'. Proposed: a bottom "
     "sheet in Instrument 18px that says what you're acting on and whether it's already uploaded. Upload is always offered (re-upload); "
     "Delete is red and always asks to confirm, so the sheet can default to a safe action.",
     ["before-actions"], ["after-sheet-photo", "after-sheet-video", "after-sheet-uploaded", "after-sheet-delete-focus", "after-sheet-error"]),
    ("Delete, upload, empty", "Delete confirmation defaults to Cancel. Upload adds the item to the upload queue and "
     "shows a brief notice; you keep browsing. The empty state matches the black gallery.",
     [], ["after-confirm-delete", "after-confirm-delete-focus", "after-queued", "after-empty"]),
]

CONTROLS = [
    ("Turn dial", "Previous / next item", "Same"),
    ("Press dial", "Open Esc / Upload / Delete menu", "Open the action sheet (photo or video). In the sheet: run the row; Delete asks to confirm"),
    ("Right button (B)", "Video: play. Photo: sync whole queue", "Video: play. Photo: nothing"),
    ("Left button (A)", "Back / exit", "Close sheet, else exit (unchanged)"),
]


def review_page(names):
    def img(name):
        uri = base64.b64encode((OUT / f"{name}.png").read_bytes()).decode()
        label = name.replace("before-", "").replace("after-", "").replace("-", " ")
        return f'<figure><img src="data:image/png;base64,{uri}" alt="{label}"><figcaption>{label}</figcaption></figure>'
    body = []
    for title, note, before, after in SECTIONS:
        cols = ""
        if before:
            cols += f'<div class="col"><h3>Current (staging)</h3><div class="row">{"".join(img(n) for n in before)}</div></div>'
        cols += f'<div class="col"><h3>Proposed</h3><div class="row">{"".join(img(n) for n in after)}</div></div>'
        body.append(f"<section><h2>{title}</h2><p>{note}</p><div class=\"cols\">{cols}</div></section>")
    rows = "".join(f"<tr><td>{a}</td><td>{b}</td><td>{c}</td></tr>" for a, b, c in CONTROLS)
    return f"""<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Photos Redesign</title>
<style>
:root{{--bg:#f6f6f4;--fg:#18181a;--muted:#66666c;--card:#fff;--line:#e2e2de}}
@media (prefers-color-scheme:dark){{:root{{--bg:#141416;--fg:#ececee;--muted:#9a9aa2;--card:#1d1d20;--line:#2c2c30}}}}
body{{margin:0;background:var(--bg);color:var(--fg);font:15px/1.5 system-ui,sans-serif}}
main{{max-width:1240px;margin:0 auto;padding:28px 16px 64px}}
h1{{font-size:26px;margin:0 0 4px}} h2{{font-size:19px;margin:0 0 6px}} h3{{font-size:12px;letter-spacing:.08em;text-transform:uppercase;color:var(--muted);margin:0 0 10px}}
p{{color:var(--muted);max-width:820px;margin:0 0 16px}}
section{{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:20px;margin:18px 0}}
.cols{{display:flex;gap:28px;flex-wrap:wrap}} .row{{display:flex;gap:16px;flex-wrap:wrap}}
figure{{margin:0}} img{{width:280px;max-width:100%;image-rendering:pixelated;border-radius:10px;display:block;box-shadow:0 0 0 1px var(--line)}}
@media (min-width:700px){{img{{width:420px}}}}
figcaption{{font-size:12px;color:var(--muted);margin-top:6px}}
table{{border-collapse:collapse;width:100%;font-size:14px}} td,th{{text-align:left;padding:8px 10px;border-top:1px solid var(--line);vertical-align:top}}
th{{color:var(--muted);font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.06em}}
</style></head><body><main>
<h1>Photos app redesign</h1>
<p>Rendered at native 280&times;240 by the host Screen Studio renderer using the firmware's own font data
(5&times;7 GLCD for current labels, Instrument for the proposal). Shown at 1.5&times;. Sample images are procedural placeholders; the status bar in
the current frames is an approximation.</p>
<section><h2>Controls</h2><table><tr><th>Input</th><th>Today</th><th>Proposed</th></tr>{rows}</table></section>
{''.join(body)}
</main></body></html>"""


def main():
    build()
    names = subprocess.check_output([str(BINARY), str(OUT)], text=True).split()
    for name in names:
        ppm_to_png(OUT / f"{name}.ppm", OUT / f"{name}.png")
    (OUT / "index.html").write_text(review_page(names), encoding="utf-8")
    print(f"Rendered {len(names)} screens to {OUT} (review: {OUT / 'index.html'})")


if __name__ == "__main__":
    main()
