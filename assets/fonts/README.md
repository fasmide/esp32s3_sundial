Font assets used by the watchface renderer.

Current font:
- `DejaVuSansMono-Bold.ttf`
- vendored from the Arch `ttf-dejavu` package
- license: see `DejaVu-LICENSE.txt`

Why this font:
- monospaced digits keep the time display stable
- open source and easy to vendor

To regenerate `main/generated_font.h` after changing sizes or glyph sets:

```bash
python tools/generate_font_header.py
```
