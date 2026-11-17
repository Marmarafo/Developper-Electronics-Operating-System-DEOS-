#!/usr/bin/env python3
# DEOS -- font8x16.h uretici. DejaVu Sans Mono'dan 8x16 bitmap font olusturur.
# Normalde calistirmaniz GEREKMEZ (font8x16.h hazir); sadece fontu degistirmek isterseniz.
from PIL import Image, ImageDraw, ImageFont
import sys

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
W, H = 8, 16
font = ImageFont.truetype(FONT, 14)

def glyph(ch):
    # 13 px DejaVu Sans Mono: yazi genisligi ~7.8 px -> 8 piksellik hucreye sigar.
    # Anti-aliased cizim + esik (110): 1-bit hintingten daha duzgun sonuc verir.
    f = ImageFont.truetype(FONT, 13)
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    d.text((0, 0), ch, font=f, fill=255)
    rows = []
    for y in range(H):
        b = 0
        for x in range(W):
            if img.getpixel((x, y)) >= 110:
                b |= 0x80 >> x
        rows.append(b)
    return rows

out = []
out.append("// DEOS -- 8x16 bitmap font, ASCII 0-127. gen_font.py ile uretildi")
out.append("// (DejaVu Sans Mono'dan). Her karakter 16 satir; her satir 1 byte,")
out.append("// en soldaki piksel en anlamli bit.")
out.append("#ifndef DEOS_FONT8X16_H")
out.append("#define DEOS_FONT8X16_H")
out.append("static const unsigned char font8x16[128][16] = {")
for c in range(128):
    if c < 32 or c == 127:
        rows = [0] * 16
    else:
        rows = glyph(chr(c))
    name = repr(chr(c)) if 32 <= c < 127 else "ctrl"
    out.append("    /* %3d %s */ {%s}," % (c, name.replace("*/", "* /"), ", ".join("0x%02X" % r for r in rows)))
out.append("};")
out.append("#endif")
open("font8x16.h", "w").write("\n".join(out) + "\n")

if len(sys.argv) > 1:
    for s in sys.argv[1]:
        print(repr(s))
        for r in glyph(s):
            print("".join("#" if r & (0x80 >> x) else "." for x in range(W)))
