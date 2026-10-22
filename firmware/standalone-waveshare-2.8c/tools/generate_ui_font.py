"""Generate compact antialiased UI glyphs from DejaVu Sans Bold (Bitstream Vera license)."""

from pathlib import Path
from PIL import Image, ImageDraw, ImageFont


FONT = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
OUTPUT = Path(__file__).resolve().parents[1] / "src" / "ui_font.h"
SIZES = (9, 12, 18, 27)


def make_font(size):
    font = ImageFont.truetype(str(FONT), size)
    glyphs = []
    pixels = []
    for code in range(32, 127):
        image = Image.new("L", (size * 3, size * 2), 0)
        ImageDraw.Draw(image).text((0, 0), chr(code), font=font, fill=255)
        bbox = image.getbbox()
        offset = len(pixels)
        advance = max(1, round(font.getlength(chr(code))))
        if bbox is None:
            glyphs.append((offset, 0, 0, advance, 0, 0))
            continue
        left, top, right, bottom = bbox
        crop = image.crop(bbox)
        values = list(crop.get_flattened_data())
        for index in range(0, len(values), 2):
            high = round(values[index] * 15 / 255)
            low = round(values[index + 1] * 15 / 255) if index + 1 < len(values) else 0
            pixels.append((high << 4) | low)
        glyphs.append((offset, right - left, bottom - top, advance, left, top))
    return glyphs, pixels


def main():
    lines = [
        "#pragma once",
        "#include <Arduino.h>",
        "// Generated from DejaVu Sans Bold; see tools/generate_ui_font.py.",
        "struct UiGlyph { uint32_t offset; uint8_t width, height, advance, x, y; };",
    ]
    for size in SIZES:
        glyphs, pixels = make_font(size)
        lines.append(f"const UiGlyph kUiGlyphs{size}[95] PROGMEM = {{")
        lines.extend("  {" + ", ".join(map(str, glyph)) + "}," for glyph in glyphs)
        lines.append("};")
        lines.append(f"const uint8_t kUiPixels{size}[{len(pixels)}] PROGMEM = {{")
        for index in range(0, len(pixels), 24):
            lines.append("  " + ", ".join(f"0x{value:02X}" for value in pixels[index:index + 24]) + ",")
        lines.append("};")
    OUTPUT.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
