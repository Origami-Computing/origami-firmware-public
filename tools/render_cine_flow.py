"""Render the current Cine UI into one shareable flow PNG; never embeds the input photo in Git."""

from pathlib import Path
import argparse, json, math, re, struct, subprocess, tempfile
from PIL import Image, ImageDraw, ImageFont, ImageOps

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    i = source.index("{", start)
    depth = 1
    end = i + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def emulated_color_photos(photo):
    """Relative Sunny→mode previews from canonical ISP tables, not sensor simulation.

    The supplied encoded RGB photo represents Sunny. Reconstruct a CMX input via
    the Sunny inverse, apply the selected CMX, then convert YUV back to RGB.
    Mono additionally remaps the shared gamma output knots to its own knots.
    This cannot reproduce sensor WB, clipping, ISP ordering or JPEG artifacts.
    """
    tables = "\n".join((ROOT / name).read_text() for name in (
        "include/core/cine_isp.h", "include/core/cine_film_matrices.h"))

    def table(name):
        values = re.search(r"constexpr uint8_t " + name + r"\[\d+\] = \{([^}]+)\}", tables)
        if not values:
            raise ValueError("Missing canonical ISP table: " + name)
        return [int(value) for value in values[1].split(",")]

    def matrix(name):
        values = table(name)
        signs = (values[9] << 8) | values[10]
        return [[(-1 if signs & (1 << (row * 3 + col)) else 1)
                 * values[row * 3 + col] / 128 for col in range(3)] for row in range(3)]

    def multiply(a, b):
        return [[sum(a[i][k] * b[k][j] for k in range(3))
                 for j in range(3)] for i in range(3)]

    def inverse(a):
        # Small Gauss-Jordan inverse; no scientific-Python dependency for diagrams.
        rows = [list(row) + [float(i == j) for j in range(3)] for i, row in enumerate(a)]
        for col in range(3):
            pivot = max(range(col, 3), key=lambda i: abs(rows[i][col]))
            rows[col], rows[pivot] = rows[pivot], rows[col]
            scale = rows[col][col]
            if abs(scale) < 1e-12:
                raise ValueError("Singular preview matrix")
            rows[col] = [value / scale for value in rows[col]]
            for row in range(3):
                if row != col:
                    factor = rows[row][col]
                    rows[row] = [a - factor * b for a, b in zip(rows[row], rows[col])]
        return [row[3:] for row in rows]

    yuv_to_rgb = [[1, 0, 1.402], [1, -.344136, -.714136], [1, 1.772, 0]]
    sunny_to_seed = inverse(multiply(yuv_to_rgb, matrix("matrix")))
    result = [photo.copy()]
    for mode, name in enumerate(("goldMatrix", "portraMatrix", "monoMatrix", "coolMatrix", "vividMatrix"), 1):
        transform = multiply(multiply(yuv_to_rgb, matrix(name)), sunny_to_seed)
        coefficients = tuple(value for row in transform for value in (*row, 0))
        image = photo.convert("RGB", coefficients)
        if mode == 3:
            # Fifteen output knots plus endpoints; the final ISP slope is only
            # approximated by the endpoint segment for this display-domain mock.
            xs = [0] + table("gamma")[:15] + [255]
            ys = [0] + table("monoGamma")[:15] + [255]
            lut = []
            segment = 0
            for value in range(256):
                while value > xs[segment + 1]:
                    segment += 1
                t = (value - xs[segment]) / (xs[segment + 1] - xs[segment])
                lut.append(round(ys[segment] + t * (ys[segment + 1] - ys[segment])))
            image = image.point(lut * 3)
        result.append(image)
    return result


def native_frames(image, output, shutter_us=16667):
    with tempfile.TemporaryDirectory(prefix="cine-flow-render-") as td:
        p = Path(td)
        (p / "Arduino.h").write_text(
            "#pragma once\n#include <stdint.h>\n#include <stddef.h>\n#include <pgmspace.h>\nusing std::min; using std::max;\n"
        )
        (p / "pgmspace.h").write_text(
            "#pragma once\n#define PROGMEM\n#define pgm_read_byte(a) (*(const unsigned char*)(a))\n"
        )
        (p / "Fonts/GFXFF").mkdir(parents=True)
        (p / "Fonts/GFXFF/gfxfont.h").write_text(
            "#pragma once\n#include <stdint.h>\nstruct GFXglyph{uint32_t bitmapOffset;uint8_t width,height,xAdvance;int8_t xOffset,yOffset;};struct GFXfont{uint8_t* bitmap;GFXglyph* glyph;uint16_t first,last;uint8_t yAdvance;};\n"
        )
        (p / "TB_TFT_eSPI.h").write_text(
            "#pragma once\nstruct Canvas; using TFT_eSPI=Canvas;\n"
            "constexpr unsigned TL_DATUM=0, TC_DATUM=1, TR_DATUM=2, MC_DATUM=4;\n"
        )
        photo = (
            ImageOps.exif_transpose(Image.open(image))
            .convert("RGB")
            .resize((224, 168), Image.Resampling.BOX)
        )
        for mode, preview in enumerate(emulated_color_photos(photo)):
            pixels = preview.tobytes()
            (p / f"photo-{mode}.rgb565").write_bytes(
                b"".join(
                    struct.pack(
                        "<H",
                        ((pixels[i] >> 3) << 11)
                        | ((pixels[i + 1] >> 2) << 5)
                        | (pixels[i + 2] >> 3),
                    )
                    for i in range(0, len(pixels), 3)
                )
            )
        source = (ROOT / "src/apps/cine_camera_app.cpp").read_text()
        code = (
            (ROOT / "tools/fixtures/cine_flow_renderer.cpp")
            .read_text()
            .replace(
                "// @APP_FUNCTIONS@",
                "\n".join(
                    function(source, s)
                    for s in [
                        "void shutterValue(",
                        "ui::ListItem settingsItem(",
                        "void build(",
                    ]
                ),
            )
        )
        tft = (ROOT / "lib/TB_TFT_eSPI/TFT_eSPI.cpp").read_text()
        code = code.replace(
            "// @TFT_FUNCTIONS@",
            "\n".join(
                function(tft, signature).replace("TFT_eSPI::", "")
                for signature in [
                    "void TFT_eSPI::fillCircleHelper(",
                    "void TFT_eSPI::fillRoundRect(",
                    "uint16_t TFT_eSPI::alphaBlend(uint8_t alpha, uint16_t fgc, uint16_t bgc)",
                    "int16_t TFT_eSPI::drawFlashFont(",
                ]
            ),
        )
        lists = (ROOT / "src/ui/list_view.cpp").read_text()
        code = code.replace(
            "// @LIST_FUNCTIONS@",
            "struct ListGeometry { safe_area::Box firstRow, scrollbar; };\n"
            + "\n".join(
                function(lists, signature)
                for signature in [
                    "static int fittingRows(",
                    "static ListGeometry prepareLayout(",
                    "void ListView::clampScroll(",
                    "void ListView::buildScene(",
                ]
            ),
        )
        status = (ROOT / "src/ui/system_status_bar.cpp").read_text()
        wifi = (ROOT / "src/ui/wifi_icon.cpp").read_text()
        alpha = (ROOT / "src/ui/alpha_bitmap.cpp").read_text()
        code = code.replace(
            "// @STATUS_FUNCTIONS@",
            '#include "ui/text_field.h"\n'
            '#include "ui/battery_indicator.h"\n'
            '#include "ui/status_bar_palette.h"\n'
            '#include "config/ui_layout.h"\n'
            '#include "assets/battery_icon.h"\n'
            '#include "assets/wifi_icons.h"\n'
            "namespace hal { namespace display { Canvas& canvas() { assert(false); static Canvas c; return c; } }}\n"
            "namespace ui {\nconstexpr int kWifiIconSize=22;\n"
            "static TextField s_clockField;\n"
            # This fixture renders disconnected states with no queued transfers.
            "static void drawSyncGlyph(TFT_eSPI&, int, int, uint8_t, uint8_t, uint8_t count, uint16_t) { assert(count == 0); }\n"
            "enum BarPart : uint8_t { WifiPart=1, BatteryPart=2, ClockPart=4, AllParts=7 };\n"
            "uint32_t batteryAnimationTime() { return 1200; }\n"
            + function(wifi, "static uint16_t blendWifiInk(")
            + "\n"
            + function(wifi, "void drawWifiIcon(").replace(
                "bool transparent)", "bool transparent=false)"
            )
            + "\n"
            + function(alpha, "void drawAlphaBitmap(")
            + "\n"
            + "\n".join(
                function(status, signature)
                for signature in [
                    "static void drawBleGlyph(",
                    "static void paintClockCentered(",
                    "static void drawBarGlyphs(",
                ]
            )
            + "\n} // namespace ui\n",
        )
        # The platform adapter and the actual safe-area painter share one TU.
        code += '\n#include "' + str(ROOT / "src/ui/safe_area_draw.cpp") + '"\n'
        (p / "render.cpp").write_text(code)
        subprocess.run(
            [
                "c++",
                "-std=c++17",
                "-O1",
                "-DORIGAMI_INSTRUMENT_FONT",
                "-I" + str(p),
                "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / "lib/TB_TFT_eSPI"),
                str(p / "render.cpp"),
                str(ROOT / "src/ui/list_row.cpp"),
                "-o",
                str(p / "render"),
            ],
            check=True,
        )
        subprocess.run(
            [str(p / "render"), str(p / "photo-"), str(output), str(shutter_us)],
            check=True,
        )
    return {f.stem: Image.open(f).convert("RGB") for f in output.glob("*.ppm")}


def focused_sheet(frames, screen_scale=2, transparent=False):
    """Show the core interaction loops, preserving native screenshot pixels."""
    ratio = screen_scale / 2
    q = lambda value: round(value * ratio)
    width, height = q(2160), q(2440)
    ink = (0, 0, 0, 255) if transparent else "white"
    border = ink if transparent else "#555555"
    sheet = Image.new("RGBA", (width, height), (0, 0, 0, 0)) if transparent else Image.new("RGB", (width, height), "black")
    draw = ImageDraw.Draw(sheet)
    fontroot = Path("/usr/share/fonts/truetype/dejavu")
    states = []

    def font(size, bold=False):
        return ImageFont.truetype(
            str(fontroot / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")), q(size)
        )

    def text(x, y, value, size=23, bold=False):
        draw.text((q(x), q(y)), value, font=font(size, bold), fill=ink)

    def wrap(x, y, value, max_width=550, size=23):
        row = ""
        for word in value.split():
            trial = (row + " " + word).strip()
            if row and draw.textlength(trial, font=font(size)) > q(max_width):
                text(x, y, row, size)
                y += 33
                row = word
            else:
                row = trial
        if row:
            text(x, y, row, size)
            y += 33
        return y

    def card(col, y, key, title, body):
        x = 80 + col * 700
        states.append(key)
        text(x, y, title, 29, True)
        frame = frames[key].resize((280 * screen_scale, 240 * screen_scale), Image.Resampling.NEAREST)
        px, py = q(x), q(y + 52)
        sheet.paste(frame, (px, py))
        stroke = max(1, q(1))
        draw.rectangle(
            (px - stroke, py - stroke, px + frame.width + stroke - 1,
             py + frame.height + stroke - 1),
            outline=border, width=stroke,
        )
        wrap(x, y + 550, body)

    def arrow(x1, x2, y, label, reverse=False):
        draw.line(tuple(q(v) for v in (x1, y, x2, y)), fill=ink, width=q(3))
        draw.polygon([(q(x), q(yy)) for x, yy in [(x2, y), (x2 - 12, y - 7), (x2 - 12, y + 7)]], fill=ink)
        if reverse:
            draw.polygon([(q(x), q(yy)) for x, yy in [(x1, y), (x1 + 12, y - 7), (x1 + 12, y + 7)]], fill=ink)
        tw = draw.textlength(label, font=font(21, True)) / ratio
        text((x1 + x2 - tw) / 2, y - 37, label, 21, True)

    def note(col, y, title, entries):
        x = 80 + col * 700
        text(x, y, title, 29, True)
        yy = y + 70
        for heading, body in entries:
            text(x, yy, heading, 24, True)
            yy = wrap(x, yy + 40, body) + 32

    text(80, 45, "Cine camera", 68, True)
    text(84, 135, "Key screens & user flows", 30)
    text(
        84,
        198,
        "A  Settings / back     B  Take photo     C  Click wheel     Turn  Adjust / browse",
        25,
    )
    draw.line(tuple(q(v) for v in (80, 265, 2080, 265)), fill=border, width=q(2))

    card(
        0, 310, "home", "01   Home", "B opens Cine. A+B exits Cine to the previous app."
    )
    card(
        1,
        310,
        "live",
        "02   Viewfinder",
        "Starts in Auto exposure with Sunny color. The wheel starts in Color; A opens settings.",
    )
    card(
        2,
        310,
        "saved",
        "03   Photo saved",
        "B captures a 3MP JPEG. Saved feedback clears after 1.8s; live preview continues.",
    )
    arrow(660, 760, 595, "B")
    arrow(1360, 1460, 595, "B")

    card(
        0,
        1000,
        "color-mono",
        "04   Adjust color",
        "Turn cycles Sunny, Gold, Portra, Mono, Cool and Vivid. This example selects Mono; exposure stays Auto.",
    )
    card(
        1,
        1000,
        "shutter-mono",
        "05   Adjust shutter",
        "C switches to SS, retaining Mono. Turn selects a manual shutter from 1/1000–1/10s. C returns to Color.",
    )
    arrow(660, 760, 1285, "C", reverse=True)
    note(
        2, 1000, "Wheel interaction",
        [("Color first · Auto exposure", "Color is above SS. Click C to switch the active row; the selected row is blue."),
         ("Hold C → Auto", "Restores Auto exposure and selects SS; keeps the selected Color."),
         ("2s → clean view", "Readouts snap away after 2s. Mode and values are retained.")],
    )

    card(
        0,
        1690,
        "settings-ss",
        "06   Settings",
        "Sunny settings example. A opens this list; turn to browse and C to edit.",
    )
    card(
        1,
        1690,
        "settings-ss-edit",
        "07   Edit a setting",
        "Turn changes the value. C or A finishes editing; A from the list returns to the viewfinder.",
    )
    arrow(660, 760, 1975, "C", reverse=True)
    note(
        2,
        1690,
        "Return & recovery",
        [
            (
                "Settings → viewfinder",
                "A while browsing, or C on Back to camera. The camera stays initialized.",
            ),
            (
                "Save failure",
                "A brief failure badge appears in the viewfinder. Press B to retry.",
            ),
            (
                "Camera unavailable",
                "A returns to the previous app. Reopen Cine to retry.",
            ),
        ],
    )
    text(
        80,
        2382,
        f"280 × 240 screens at {screen_scale}×  ·  reference-photo.jpg  ·  Native firmware pixels · supplied photo in previews",
        22,
    )
    return sheet, states


def render(
    image, output, home=None, layout="full", captured_screens=None, shutter_us=16667, screen_scale=2
):
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="cine-flow-frames-") as td:
        frames = native_frames(image, Path(td), shutter_us)
        if home:
            frames["home"] = Image.open(home).convert("RGB")
            if frames["home"].size != (280, 240):
                raise ValueError("Home capture must be native 280x240 pixels")
        else:
            # Context fallback is explicitly schematic; the app frames use source renders.
            h = Image.new("RGB", (280, 240), "white")
            d = ImageDraw.Draw(h)
            d.rounded_rectangle((64, 81, 115, 145), radius=18, fill="#173256")
            d.rounded_rectangle((159, 81, 210, 145), radius=18, fill="#173256")
            frames["home"] = h
        if captured_screens:
            for key in frames:
                capture = Path(captured_screens) / (key + ".png")
                if capture.exists():
                    im = Image.open(capture).convert("RGB")
                    if im.size != (280, 240):
                        raise ValueError(
                            f"Device capture must be native 280x240: {capture}"
                        )
                    frames[key] = im
        if layout in ("focused-dark", "focused-transparent"):
            sheet, states = focused_sheet(frames, screen_scale, layout == "focused-transparent")
            sheet.save(output)
            frames_dir = output.parent / (output.stem + "-screens")
            frames_dir.mkdir(exist_ok=True)
            for key in frames:
                frames[key].save(frames_dir / (key + ".png"))
            output.with_suffix(".json").write_text(
                json.dumps(
                    {
                        "source_photo": str(Path(image).resolve()),
                        "output": str(output.resolve()),
                        "size": list(sheet.size),
                        "native_screen": [280, 240],
                        "scale": screen_scale,
                        "transparent": layout == "focused-transparent",
                        "states": states,
                        "exported_screens": list(frames),
                        "layout": layout,
                        "shutter_us": shutter_us,
                        "ui_source": "production Cine raster, list layout, safe-area painter and TFT glyph/shape raster",
                        "captured_screens": (
                            str(captured_screens) if captured_screens else None
                        ),
                        "home": "device capture" if home else "schematic context",
                        "state_sources": {
                            key: (
                                "device framebuffer"
                                if (key == "home" and home)
                                or (
                                    captured_screens
                                    and (
                                        Path(captured_screens) / (key + ".png")
                                    ).exists()
                                )
                                else (
                                    "production renderer with supplied photo"
                                    if key in ("live", "saved", "shutter")
                                    or key.startswith(("color-", "shutter-"))
                                    else "production renderer; illustrative system status"
                                )
                            )
                            for key in states
                        },
                        "preview_color": "host relative CMX emulation from current firmware tables; Mono output-knot remap; supplied photo is Sunny reference, not a sensor simulation",
                        "private_photo_committed": False,
                    },
                    indent=2,
                )
                + "\n"
            )
            print(output)
            return
        W, H = 3480, 4220
        sheet = Image.new("RGB", (W, H), "#f3f1ed")
        draw = ImageDraw.Draw(sheet)
        ink = "#182431"
        muted = "#586775"
        line = "#b7c3cd"
        accent = "#205eaa"
        teal = "#17665f"
        fontroot = Path("/usr/share/fonts/truetype/dejavu")

        def font(size, bold=False):
            return ImageFont.truetype(
                str(fontroot / ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")),
                size,
            )

        def text(x, y, s, size=24, color=ink, bold=False):
            draw.text((x, y), s, font=font(size, bold), fill=color)

        def wrap(x, y, s, width, size=23, color=muted, lineheight=32):
            words = s.split()
            row = ""
            for word in words:
                trial = (row + " " + word).strip()
                if draw.textlength(trial, font=font(size)) > width and row:
                    text(x, y, row, size, color)
                    y += lineheight
                    row = word
                else:
                    row = trial
            if row:
                text(x, y, row, size, color)
                y += lineheight
            return y

        def pill(x, y, s, color=accent):
            w = int(draw.textlength(s, font=font(21, True))) + 28
            draw.rounded_rectangle((x, y, x + w, y + 39), radius=18, fill="#e3eaf3")
            text(x + 14, y + 5, s, 21, color, True)
            return w

        def arrow(points, label=None, where=None, color=accent):
            draw.line(points, fill=color, width=4, joint="curve")
            a, b = points[-2:]
            angle = math.atan2(b[1] - a[1], b[0] - a[0])
            n = 14
            draw.polygon(
                [
                    b,
                    (
                        b[0] - n * math.cos(angle - 0.48),
                        b[1] - n * math.sin(angle - 0.48),
                    ),
                    (
                        b[0] - n * math.cos(angle + 0.48),
                        b[1] - n * math.sin(angle + 0.48),
                    ),
                ],
                fill=color,
            )
            if label:
                x, y = where if where else ((a[0] + b[0]) / 2, (a[1] + b[1]) / 2 - 22)
                chunks = label.split("\n")
                tw = max(draw.textlength(s, font=font(18, True)) for s in chunks)
                draw.rounded_rectangle(
                    (x - tw / 2 - 7, y - 3, x + tw / 2 + 7, y + len(chunks) * 25),
                    radius=7,
                    fill="#f3f1ed",
                )
                for i, s in enumerate(chunks):
                    text(
                        x - draw.textlength(s, font=font(18, True)) / 2,
                        y + i * 25,
                        s,
                        18,
                        color,
                        True,
                    )

        cards = {}

        def card(col, y, key, title, number, body, badge=None):
            x = 88 + col * 676
            draw.rounded_rectangle(
                (x, y, x + 600, y + 700),
                radius=22,
                fill="white",
                outline="#d8dfe4",
                width=2,
            )
            text(x + 20, y + 15, f"{number:02d}", 23, accent, True)
            text(x + 71, y + 15, title, 25, ink, True)
            frame = frames[key].resize((560, 480), Image.Resampling.NEAREST)
            sheet.paste(frame, (x + 20, y + 58))
            draw.rounded_rectangle(
                (x + 17, y + 55, x + 583, y + 541), radius=8, outline="#253442", width=3
            )
            wrap(x + 20, y + 555, body, 560, 23)
            if badge:
                pill(x + 20, y + 652, badge)
            cards[key] = (x, y)
            return x, y

        def context(col, y, title, lines):
            x = 88 + col * 676
            draw.rounded_rectangle((x, y, x + 600, y + 700), radius=22, fill="#e7ecee")
            text(x + 32, y + 30, title, 32, ink, True)
            yy = y + 96
            for heading, body in lines:
                text(x + 32, yy, heading, 25, teal, True)
                yy = wrap(x + 32, yy + 40, body, 530, 24) + 38

        def lane(y, n, title, description):
            text(88, y, n, 28, accent, True)
            text(146, y - 4, title, 38, ink, True)
            text(146, y + 47, description, 24, muted)

        text(88, 48, "Cine camera", 76, ink, True)
        text(92, 146, "Interaction map  /  Firmware design review", 31, muted)
        text(2450, 66, "18 screen / interaction states", 26, accent, True)
        text(2450, 108, "280 × 240 pixels · enlarged 2×", 24, muted)
        x = 92
        for value in [
            "A  settings / back",
            "B  take photo",
            "C  click wheel: switch Color / SS",
            "Turn wheel: adjust value",
        ]:
            x += pill(x, 210, value) + 18
        lane(
            284,
            "01",
            "Enter Cine",
            "Home B is the quick launch. Stock Camera remains in the main application selector.",
        )
        y = 378
        card(
            0,
            y,
            "home",
            "Home",
            1,
            "B opens Cine. Turning the wheel opens the application selector.",
            "B → Cine",
        )
        card(
            1,
            y,
            "opening",
            "Opening splash",
            2,
            "Immediate feedback while the camera starts.",
            "Transient",
        )
        card(
            2,
            y,
            "preparing",
            "Preparing camera",
            3,
            "Shown while resources are finishing; normally this state may be skipped.",
            "Transient",
        )
        card(
            3,
            y,
            "live",
            "Live viewfinder",
            4,
            "Clean view at rest. The current wheel mode is remembered when its readout hides.",
            "Default: Sunny · shutter",
        )
        context(
            4,
            y,
            "Controls at a glance",
            [
                ("A / gear", "Open the conventional settings list."),
                (
                    "Wheel / C",
                    "Turn to adjust; click to switch mode with a Select haptic.",
                ),
                (
                    "B / camera",
                    "Capture a native 3MP JPEG. A+B returns to the previous app.",
                ),
            ],
        )
        for col, label in [(0, "B"), (1, "wait"), (2, "ready")]:
            arrow(
                [(88 + col * 676 + 602, y + 298), (88 + (col + 1) * 676 - 4, y + 298)],
                label,
            )
        arrow(
            [(1064, 1080), (1064, 1120), (2416, 1120), (2416, 1080)],
            "Can proceed straight to live",
            where=(1740, 1106),
            color=teal,
        )
        lane(
            1170,
            "02",
            "Direct manipulation",
            "Both values appear together. The active mode is blue; labels and values snap off after 2 seconds of inactivity.",
        )
        y = 1264
        card(
            0,
            y,
            "shutter",
            "Adjust shutter",
            5,
            "Turn changes shutter speed. Every movement restarts the 2-second hold.",
            "SS · 1/1000–1/10 s",
        )
        card(
            1,
            y,
            "color-sunny",
            "Color · Sunny",
            6,
            "Click C switches from shutter to Color and plays the standard Select haptic.",
            "Default color",
        )
        card(
            2,
            y,
            "color-gold",
            "Color · Gold",
            7,
            "Turning the wheel cycles through the six Color modes.",
            "Turn to cycle",
        )
        card(
            3,
            y,
            "color-portra",
            "Color · Portra",
            8,
            "The sensor stays on Sunny WB. Shutter remains visible.",
            "Turn to cycle",
        )
        card(
            4,
            y,
            "color-vivid",
            "Color · Vivid",
            9,
            "After Mono and Cool comes Vivid; the next turn wraps to Sunny. Click C returns to shutter.",
            "Turn to cycle",
        )
        for col, label in [(0, "C +\nhaptic"), (1, "turn"), (2, "turn"), (3, "turn x3")]:
            arrow(
                [(88 + col * 676 + 602, y + 298), (88 + (col + 1) * 676 - 4, y + 298)],
                label,
            )
        arrow([(760, y + 340), (690, y + 340)])
        arrow(
            [(3092, y + 702), (3092, y + 740), (1064, y + 740), (1064, y + 703)],
            "Color wraps both ways",
            where=(2070, y + 726),
            color=teal,
        )
        text(
            102,
            2042,
            "Any adjustment state → LIVE after 2s. Instant appearance / disappearance; 50% black pills stay fixed; selected mode and values persist.",
            24,
            teal,
            True,
        )
        lane(
            2096,
            "03",
            "Settings",
            "A opens settings with the current wheel mode selected. Turn to browse; C toggles editing; A finishes editing before returning to the viewfinder.",
        )
        y = 2190
        card(
            0,
            y,
            "settings-ss",
            "Shutter selected",
            10,
            "Turn navigates the list. C begins editing the selected setting.",
            "Browse",
        )
        card(
            1,
            y,
            "settings-ss-edit",
            "Edit shutter",
            11,
            "Blue row text denotes editing. Turn changes shutter. C or A finishes editing.",
            "Edit",
        )
        card(
            2,
            y,
            "settings-color",
            "Color selected",
            12,
            "After leaving edit mode, turn to the Color row. C begins editing.",
            "Browse",
        )
        card(
            3,
            y,
            "settings-color-edit",
            "Edit color",
            13,
            "Turn cycles Sunny / Gold / Portra / Mono / Cool / Vivid. C or A finishes editing.",
            "Edit",
        )
        card(
            4,
            y,
            "settings-back",
            "Back to camera",
            14,
            "C on Back to camera returns to the viewfinder. A from list browsing does too.",
            "→ LIVE",
        )
        for col, label in [
            (0, "C"),
            (1, "C, then\nturn"),
            (2, "C"),
            (3, "C, then\nturn"),
        ]:
            arrow(
                [(88 + col * 676 + 602, y + 298), (88 + (col + 1) * 676 - 4, y + 298)],
                label,
            )
        text(
            102,
            2922,
            "A from LIVE → settings.   A while editing → stop editing.   A while browsing / C on Back to camera → LIVE.   Camera stays initialized.",
            24,
            teal,
            True,
        )
        lane(
            2990,
            "04",
            "Capture, feedback and recovery",
            "B captures without leaving the viewfinder. Save failure is recoverable in place; a camera-opening / control error returns to the previous app.",
        )
        y = 3084
        card(
            0,
            y,
            "capture",
            "Capturing",
            15,
            "B briefly pulses the image and raises the photographer.",
            "B → capture",
        )
        card(
            1,
            y,
            "saved",
            "Saved",
            16,
            "Photo saved. Badge clears after 1.8 seconds; the viewfinder continues.",
            "Success → LIVE",
        )
        card(
            2,
            y,
            "save-failed",
            "Save failed",
            17,
            "Failure badge clears after 1.8 seconds. Press B to try again.",
            "Failure → retry",
        )
        context(
            3,
            y,
            "One capture flow",
            [
                ("Success", "Capture → Saved → Live."),
                ("Save failure", "Capture → Save failed → Live. B can retry."),
                ("Exit", "A+B returns to the previous app from any Cine screen."),
            ],
        )
        card(
            4,
            y,
            "error",
            "Camera unavailable",
            18,
            "Representative resource error. A goes back; reopen Cine to retry.",
            "A → previous app",
        )
        arrow([(690, y + 298), (760, y + 298)], "OK")
        arrow(
            [(388, y + 702), (388, y + 742), (1740, y + 742), (1740, y + 703)],
            "write failed",
            where=(1064, y + 728),
            color="#98533a",
        )
        text(88, 3910, "Review notes", 28, ink, True)
        wrap(
            88,
            3955,
            "Screens use the supplied reference-photo.jpg as viewfinder content. Preview frames use the production raster and Instrument font; menus use the current Scene builder, list styling and safe-area geometry. Opening/error examples are representative. Variable system status is omitted from source-rendered screens.",
            3304,
            24,
        )
        text(
            88,
            4145,
            "Live image pipeline: C+0 S+1 · B0   /   Sunny default   /   Native 3MP JPEG   /   Static design sheet; timings noted above",
            24,
            muted,
        )
        sheet.save(output)
        frames_dir = output.parent / (output.stem + "-screens")
        frames_dir.mkdir(exist_ok=True)
        for key, im in frames.items():
            im.save(frames_dir / (key + ".png"))
        manifest = {
            "source_photo": str(Path(image).resolve()),
            "output": str(output.resolve()),
            "size": [W, H],
            "native_screen": [280, 240],
            "scale": 2,
            "states": list(cards),
            "ui_source": "production Cine raster / Scene builder / Instrument font",
            "home": "device capture" if home else "schematic context",
            "preview_color": "host relative CMX emulation from current firmware tables; Mono output-knot remap; supplied photo is Sunny reference, not a sensor simulation",
                        "private_photo_committed": False,
        }
        output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(output)


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--image", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--home", type=Path)
    p.add_argument("--layout", choices=("full", "focused-dark", "focused-transparent"), default="full")
    p.add_argument(
        "--captured-screens", type=Path, help="Native device PNGs keyed by state name"
    )
    p.add_argument("--shutter-us", type=int, default=16667)
    p.add_argument("--screen-scale", type=int, choices=(2, 3), default=2, help="Integer screenshot scale in focused layouts; 3 produces 3240×3660")
    a = p.parse_args()
    render(a.image, a.output, a.home, a.layout, a.captured_screens, a.shutter_us, a.screen_scale)
