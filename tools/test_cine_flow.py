"""Check native Cine design states and lossless placement in the focused sheet."""

from pathlib import Path
import importlib.util
import json
import tempfile
from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "cine_flow", ROOT / "tools/render_cine_flow.py"
)
flow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(flow)

# Distinct color patches exercise all six current CMX profiles without UI pixels.
patches = Image.new("RGB", (6, 1))
patches.putdata([(180, 95, 55), (60, 160, 90), (65, 100, 190),
                 (110, 110, 110), (200, 170, 125), (35, 60, 95)])
looks = flow.emulated_color_photos(patches)
assert len(looks) == 6 and looks[0].tobytes() == patches.tobytes()
assert len({im.tobytes() for im in looks}) == 6
mono_bytes = looks[3].tobytes()
assert mono_bytes[0::3] == mono_bytes[1::3] == mono_bytes[2::3]
# Mono has its own contrast treatment, beyond neutral desaturation.
assert looks[3].getpixel((3, 0))[0] != patches.getpixel((3, 0))[0]

with tempfile.TemporaryDirectory(prefix="cine-flow-check-") as td:
    directory = Path(td)
    photo = directory / "photo.png"
    Image.new("RGB", (640, 480), (80, 112, 144)).save(photo)
    home = directory / "home.png"
    Image.new("RGB", (280, 240), (24, 32, 48)).save(home)
    frames_dir = directory / "native"
    frames_dir.mkdir()
    frames = flow.native_frames(photo, frames_dir, 40001)
    assert len(frames) == 21 and all(im.size == (280, 240) for im in frames.values())
    menu = frames["settings-ss"]
    # Color replaces the old WB menu row, so the 2026-11-09 pre-Color
    # framebuffer digests no longer apply. Keep the native geometry/font and
    # lossless sheet-placement checks below; all six current modes are rendered.
    for mode in ["sunny", "gold", "portra", "mono", "cool", "vivid"]:
        assert "color-" + mode in frames
    # Photo-region differences must exist before the independently colored UI.
    region = (80, 110, 120, 150)
    sunny = frames["color-sunny"].crop(region)
    for mode in ("gold", "portra", "mono", "cool", "vivid"):
        assert ImageChops.difference(sunny, frames["color-" + mode].crop(region)).getbbox(), mode
    # Native list geometry and the TFT midpoint-circle raster, not a smooth circle.
    assert menu.getpixel((14, 40)) == (255, 255, 255)
    assert menu.getpixel((19, 40)) == (0, 0, 0)
    assert menu.getpixel((14, 45)) == (0, 0, 0)
    # The system band must contain the real disconnected Wi-Fi, clock and battery.
    for box in [(24, 8, 46, 30), (95, 8, 180, 35), (210, 8, 253, 35)]:
        assert ImageChops.difference(
            menu.crop(box),
            Image.new("RGB", (box[2] - box[0], box[3] - box[1]), "white"),
        ).getbbox()
    assert (
        ImageChops.difference(
            menu.crop((0, 0, 280, 38)), frames["settings-ss-edit"].crop((0, 0, 280, 38))
        ).getbbox()
        is None
    )
    # Editing changes only the selected row; the rest of the native screen is stable.
    difference = ImageChops.difference(menu, frames["settings-ss-edit"]).getbbox()
    assert difference and difference[1] >= 40 and difference[3] <= 75
    output = directory / "flows.png"
    flow.render(photo, output, home, "focused-dark", shutter_us=40001)
    manifest = json.loads(output.with_suffix(".json").read_text())
    assert len(manifest["states"]) == 7 and manifest["scale"] == 2
    assert "color-mono" in manifest["states"] and "shutter-mono" in manifest["states"]
    assert "supplied photo" in manifest["state_sources"]["color-mono"]
    assert len(manifest["exported_screens"]) == len(frames) + 1
    screens_dir = output.parent / (output.stem + "-screens")
    assert all(Image.open(screens_dir / (key + ".png")).size == (280, 240)
               for key in manifest["exported_screens"])
    sheet = Image.open(output)
    assert sheet.size == (2160, 2440) and sheet.getpixel((0, 0)) == (0, 0, 0)
    for col, y, key in [
        (0, 310, "home"),
        (1, 310, "live"),
        (2, 310, "saved"),
        (0, 1000, "color-mono"),
        (1, 1000, "shutter-mono"),
        (0, 1690, "settings-ss"),
        (1, 1690, "settings-ss-edit"),
    ]:
        x = 80 + col * 700
        source = Image.open(home) if key == "home" else frames[key]
        assert (
            ImageChops.difference(
                sheet.crop((x, y + 52, x + 560, y + 532)),
                source.resize((560, 480), Image.Resampling.NEAREST),
            ).getbbox()
            is None
        ), key
    # Both high-resolution themes preserve screenshots exactly, including edges.
    frames["home"] = Image.open(home).convert("RGB")
    for transparent in (False, True):
        high, states = flow.focused_sheet(frames, 3, transparent)
        assert high.size == (3240, 3660) and len(states) == 7
        assert high.getpixel((0, 0)) == ((0, 0, 0, 0) if transparent else (0, 0, 0))
        outside = high.copy() if transparent else None
        for col, y, key in [
            (0, 310, "home"), (1, 310, "live"), (2, 310, "saved"),
            (0, 1000, "color-mono"), (1, 1000, "shutter-mono"),
            (0, 1690, "settings-ss"), (1, 1690, "settings-ss-edit"),
        ]:
            x, yy = round((80 + col * 700) * 1.5), round((y + 52) * 1.5)
            box = (x, yy, x + 840, yy + 720)
            crop = high.crop(box)
            expected = frames[key].resize((840, 720), Image.Resampling.NEAREST)
            assert ImageChops.difference(crop.convert("RGB"), expected).getbbox() is None, key
            if transparent:
                assert crop.getchannel("A").getextrema() == (255, 255)
                outside.paste((0, 0, 0, 0), box)
        if transparent:
            # Only black annotations remain outside the opaque screenshot rectangles.
            assert outside.convert("RGB").getextrema() == ((0, 0), (0, 0), (0, 0))
    # Native captures override source renders without interpolation or recoloring.
    captures = directory / "captures"
    captures.mkdir()
    Image.new("RGB", (280, 240), (48, 80, 112)).save(captures / "settings-ss.png")
    flow.render(photo, output, home, "focused-dark", captures)
    assert Image.open(output).getpixel((80, 1742)) == (48, 80, 112)
print(
    "PASS Cine flow: production native states, menu raster/chrome, seven screens, exact 2x/3x placement, transparent black annotations and native capture overrides"
)
