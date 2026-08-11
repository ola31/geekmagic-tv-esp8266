#!/usr/bin/env python3
# Generate an Adafruit-GFX / TFT_eSPI compatible font header from a TTF using Pillow.
# Packing matches TFT_eSPI drawChar: each glyph byte-aligned, rows concatenated, MSB-first.
import sys
from PIL import Image, ImageFont, ImageDraw

def gen(ttf, px, varname, first=0x20, last=0x7E, stroke=0, thresh=128):
    font = ImageFont.truetype(ttf, px)
    ascent, descent = font.getmetrics()
    yAdvance = ascent + descent
    bitmap = bytearray()
    glyphs = []  # (offset, w, h, xadv, xoff, yoff)
    for code in range(first, last + 1):
        ch = chr(code)
        adv = round(font.getlength(ch))
        PAD = px + 4
        cw = adv + 2 * PAD
        chh = yAdvance + 2 * PAD
        img = Image.new("L", (cw, chh), 0)
        d = ImageDraw.Draw(img)
        d.text((PAD, PAD + ascent), ch, fill=255, font=font, anchor="ls",
               stroke_width=stroke, stroke_fill=255)
        bbox = img.getbbox()
        offset = len(bitmap)
        if bbox is None:
            glyphs.append((offset, 0, 0, adv, 0, 0))
            continue
        il, it, ir, ib = bbox
        w = ir - il
        h = ib - it
        xoff = il - PAD
        yoff = it - (PAD + ascent)
        crop = img.crop((il, it, ir, ib))
        acc = 0
        n = 0
        for y in range(h):
            for x in range(w):
                acc = (acc << 1) | (1 if crop.getpixel((x, y)) >= thresh else 0)
                n += 1
                if n == 8:
                    bitmap.append(acc)
                    acc = 0
                    n = 0
        if n > 0:
            bitmap.append(acc << (8 - n))
        glyphs.append((offset, w, h, adv, xoff, yoff))

    out = []
    out.append("// Auto-generated from %s at %dpx. Do not edit by hand." % (ttf.split('/')[-1], px))
    out.append("#pragma once")
    out.append("const uint8_t %sBitmaps[] PROGMEM = {" % varname)
    line = "  "
    for i, b in enumerate(bitmap):
        line += "0x%02X, " % b
        if (i + 1) % 16 == 0:
            out.append(line.rstrip())
            line = "  "
    if line.strip():
        out.append(line.rstrip())
    out.append("};")
    out.append("const GFXglyph %sGlyphs[] PROGMEM = {" % varname)
    for i, (off, w, h, adv, xo, yo) in enumerate(glyphs):
        out.append("  { %d, %d, %d, %d, %d, %d },   // 0x%02X '%s'" %
                   (off, w, h, adv, xo, yo, first + i,
                    chr(first + i) if 0x20 < first + i < 0x7F else ' '))
    out.append("};")
    out.append("const GFXfont %s PROGMEM = {" % varname)
    out.append("  (uint8_t*)%sBitmaps, (GFXglyph*)%sGlyphs, 0x%02X, 0x%02X, %d };" %
               (varname, varname, first, last, yAdvance))
    return "\n".join(out) + "\n", len(bitmap)

if __name__ == "__main__":
    ttf, px, varname, outpath = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    first = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0x20
    last = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0x7E
    stroke = int(sys.argv[7]) if len(sys.argv) > 7 else 0
    thresh = int(sys.argv[8]) if len(sys.argv) > 8 else 128
    text, nbytes = gen(ttf, px, varname, first, last, stroke, thresh)
    with open(outpath, "w") as f:
        f.write(text)
    print("wrote %s  (%s, %dpx, bitmap=%d bytes)" % (outpath, varname, px, nbytes))
