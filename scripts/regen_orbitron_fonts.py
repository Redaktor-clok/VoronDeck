#!/usr/bin/env python3
"""Generate NEMESIS VoronDeck Orbit faces with Latin and original Russian glyphs.

Requires fonttools and lv_font_conv 1.5.3. The renamed OFL source faces are built
by design/fonts/vorondeck-orbit/build_font.py; see VoronDeckOrbit-NOTICE.txt.
The historic orbitron_* C symbols are retained as internal implementation names.
"""

import argparse
from pathlib import Path
import subprocess

from fontTools.ttLib import TTFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--converter", default="lv_font_conv")
    parser.add_argument("--esp32-binary", action="store_true",
                        help="Write compressed runtime fonts for ESP32 (through 40px)")
    args = parser.parse_args()
    fonts = Path(__file__).resolve().parents[1] / "assets/fonts"
    for style, prefix, sizes in [
        ("Regular", "orbitron", [8, 10, 11, 12, 14, 16, 18, 20, 24, 26, 28, 32, 40, 48, 64]),
        ("SemiBold", "orbitron_bold", [14, 16, 18, 20, 24, 28, 32, 40, 48, 64]),
    ]:
        face = fonts / f"VoronDeckOrbit-{style}.ttf"
        cmap = TTFont(face).getBestCmap()
        assert all(c in cmap for c in list(range(0x410, 0x450)) + [0x401, 0x451])
        for size in sizes:
            if args.esp32_binary and size > 40:
                continue
            out = (fonts.parent.parent / "firmware/helixscreen-esp32/components/helixcore/fonts"
                   / f"{prefix}_{size}.bin" if args.esp32_binary else fonts / f"{prefix}_{size}.c")
            converter = ["node", args.converter] if args.converter.endswith(".js") else [args.converter]
            subprocess.run(converter + ["--font", str(face), "--size", str(size),
                "--bpp", "4", "--format", "bin" if args.esp32_binary else "lvgl", "--range",
                "0x20-0x7E,0xB0,0xB7,0xD7,0x401,0x410-0x44F,0x451,0x2013-0x2014,0x2116,0x2212,0x2248",
                "--no-kerning", "--output", str(out)]
                + ([] if args.esp32_binary else ["--no-compress"]), check=True)
            if args.esp32_binary:
                assert out.stat().st_size > 0
                continue
            text = out.read_text(encoding="utf-8")
            # Remove machine-specific paths from the checked-in output.
            text = text.replace(str(face), face.name)
            text = text.replace(str(out), out.name)
            out.write_text(text, encoding="utf-8", newline="\n")
    print("Generated VoronDeck Orbit faces with Russian Cyrillic; navigation retains Noto")


if __name__ == "__main__":
    main()
