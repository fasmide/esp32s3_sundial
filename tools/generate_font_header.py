#!/usr/bin/env python3

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


SETS = {
    "time": {
        "size": 36,
        "chars": "0123456789:",
    },
    "info": {
        "size": 20,
        "chars": "0123456789hm ",
    },
    "label": {
        "size": 18,
        "chars": "0123456789",
    },
}

NAME_MAP = {
    ":": "colon",
    " ": "space",
    "-": "dash",
}


def write_font_set(out_file, prefix, font_path, size, chars):
    font = ImageFont.truetype(str(font_path), size)
    bbox = font.getbbox("0")
    cell_width = max(1, bbox[2] - bbox[0])
    ascent, descent = font.getmetrics()
    cell_height = ascent + descent
    entries = []

    for ch in chars:
        image = Image.new("L", (cell_width, cell_height), 0)
        draw = ImageDraw.Draw(image)
        draw.text((0, 0), ch, fill=255, font=font)
        rows = []
        for y in range(cell_height):
            row = []
            for x in range(cell_width):
                row.append(image.getpixel((x, y)))
            rows.append(row)
        entries.append((ch, rows))

    out_file.write(f"#define {prefix.upper()}_FONT_WIDTH {cell_width}\n")
    out_file.write(f"#define {prefix.upper()}_FONT_HEIGHT {cell_height}\n")
    out_file.write(f"#define {prefix.upper()}_FONT_ADVANCE {cell_width}\n\n")

    for ch, rows in entries:
        safe = NAME_MAP.get(ch, ch)
        out_file.write(f"static const uint8_t {prefix}_glyph_{safe}_bitmap[] = {{\n")
        for row in rows:
            out_file.write("    " + ", ".join(str(v) for v in row) + ",\n")
        out_file.write("};\n\n")

    out_file.write(f"static const generated_glyph_t {prefix.upper()}_GLYPHS[] = {{\n")
    for ch, _ in entries:
        safe = NAME_MAP.get(ch, ch)
        out_file.write("    {'%s', %s_glyph_%s_bitmap},\n" % (ch, prefix, safe))
    out_file.write("};\n\n")
    out_file.write(f"#define {prefix.upper()}_GLYPH_COUNT {len(entries)}\n\n")


def main():
    repo_root = Path(__file__).resolve().parent.parent
    font_path = repo_root / "assets/fonts/DejaVuSansMono-Bold.ttf"
    out_path = repo_root / "main/generated_font.h"

    with out_path.open("w", encoding="ascii") as out_file:
        out_file.write("#ifndef GENERATED_FONT_H\n")
        out_file.write("#define GENERATED_FONT_H\n\n")
        out_file.write("#include <stdint.h>\n\n")
        out_file.write("typedef struct {\n")
        out_file.write("    char ch;\n")
        out_file.write("    const uint8_t *bitmap;\n")
        out_file.write("} generated_glyph_t;\n\n")

        for prefix, config in SETS.items():
            write_font_set(out_file, prefix, font_path, config["size"], config["chars"])

        out_file.write("#endif\n")


if __name__ == "__main__":
    main()
