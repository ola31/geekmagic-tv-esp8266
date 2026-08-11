#!/usr/bin/env python3
# Generate a TFT_eSPI VLW (anti-aliased "smooth") font as a PROGMEM C array.
# Format (all ints big-endian uint32):
#   header: gCount, version(11), sizePx, 0, ascent, descent
#   per glyph: unicode, gHeight, gWidth, gxAdvance, gdY, gdX, 0
#   then all glyph alpha bitmaps concatenated (gWidth*gHeight bytes each, 0..255)
#   trailer: nameLen, name, psLen, psName, aaFlag(1)
import sys, struct
from PIL import Image, ImageFont, ImageDraw


def i32(v):
    return struct.pack(">I", v & 0xFFFFFFFF)


def gen(ttf, px, first, last, name, stroke=0, bold=0.0):
    font = ImageFont.truetype(ttf, px)
    ascent, descent = font.getmetrics()
    glyphs = []
    for code in range(first, last + 1):
        ch = chr(code)
        adv = round(font.getlength(ch))
        PAD = px + 8
        cw = adv + 2 * PAD
        chh = ascent + descent + 2 * PAD
        img = Image.new("L", (cw, chh), 0)
        d = ImageDraw.Draw(img)
        base_y = PAD + ascent
        d.text((PAD, base_y), ch, fill=255, font=font, anchor="ls",
               stroke_width=stroke, stroke_fill=255)
        if bold > 0:
            img2 = Image.new("L", (cw, chh), 0)
            ImageDraw.Draw(img2).text((PAD, base_y), ch, fill=255, font=font, anchor="ls",
                                      stroke_width=stroke + 1, stroke_fill=255)
            img = Image.blend(img, img2, bold)
        bbox = img.getbbox()
        if bbox is None:
            glyphs.append(dict(code=code, w=0, h=0, adv=adv, dy=0, dx=0, bmp=b""))
            continue
        il, it, ir, ib = bbox
        w, h = ir - il, ib - it
        dy = base_y - it          # ink top above baseline
        dx = il - PAD             # left bearing
        crop = img.crop((il, it, ir, ib))
        glyphs.append(dict(code=code, w=w, h=h, adv=adv, dy=dy, dx=dx, bmp=crop.tobytes()))

    inked = [g for g in glyphs if g["h"] > 0]
    maxAscent = max((g["dy"] for g in inked), default=ascent)
    maxDescent = max((g["h"] - g["dy"] for g in inked), default=descent)

    out = bytearray()
    out += i32(len(glyphs)) + i32(11) + i32(px) + i32(0) + i32(maxAscent) + i32(maxDescent)
    for g in glyphs:
        out += i32(g["code"]) + i32(g["h"]) + i32(g["w"]) + i32(g["adv"]) + i32(g["dy"]) + i32(g["dx"]) + i32(0)
    for g in glyphs:
        out += g["bmp"]
    nm = name.encode()
    out += bytes([len(nm)]) + nm + bytes([len(nm)]) + nm + bytes([1])
    return bytes(out), maxAscent, maxDescent


def emit(data, varname):
    lines = ["// Auto-generated VLW smooth (anti-aliased) font. Do not edit.",
             "#pragma once",
             "const uint8_t %s[] PROGMEM = {" % varname]
    row = "  "
    for i, b in enumerate(data):
        row += "0x%02X," % b
        if (i + 1) % 20 == 0:
            lines.append(row)
            row = "  "
    if row.strip():
        lines.append(row)
    lines.append("};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    ttf, px, varname, outpath = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    first = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x20
    last = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0x7E
    stroke = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    bold = float(sys.argv[8]) if len(sys.argv) > 8 else 0.0
    data, asc, desc = gen(ttf, px, first, last, varname, stroke, bold)
    open(outpath, "w").write(emit(data, varname))
    print("wrote %s (%d bytes, ascent=%d descent=%d)" % (outpath, len(data), asc, desc))
