#!/bin/bash
# Renders the firmware's dashboard pages to sim/out/*.png without flashing.
set -e
cd "$(dirname "$0")"
F=../.pio/libdeps/nodemcuv2/TFT_eSPI
mkdir -p out
for f in Font16 Font32rle Font64rle Font72rle; do
  [ "$F/Fonts/$f.c" -nt "$f.o" ] 2>/dev/null || [ ! -f "$f.o" ] && \
    gcc -c -O1 -w -D PROGMEM= -o "$f.o" -x c "$F/Fonts/$f.c"
done
g++ -std=c++17 -O1 -w -D LOAD_GFXFF -I shim -I "$F" -o sim host.cpp shim/TFT_eSPI.cpp \
    Font16.o Font32rle.o Font64rle.o Font72rle.o
./sim
python3 - <<'PY'
from PIL import Image, ImageDraw

# The panel is a backlit IPS: it never reaches black, and its response lifts
# dark values hard. Colours that look near-black on a monitor come out as pale
# grey on the device, so the preview models that rather than showing raw RGB.
def as_panel(image):
    lut = []
    for value in range(256):
        lifted = 26 + (229 * ((value / 255.0) ** 0.62))
        lut.append(int(min(255, lifted)))
    return image.point(lut * 3)

names = ["clock", "river", "weather", "github"]
raw = [Image.open(f"out/{n}.ppm").resize((480, 480), Image.NEAREST) for n in names]
imgs = [as_panel(im) for im in raw]
sheet = Image.new("RGB", (480 * 4 + 30 * 5, 550), (30, 30, 34))
draw = ImageDraw.Draw(sheet)
for i, (n, im) in enumerate(zip(names, imgs)):
    x = 30 + i * (480 + 30)
    sheet.paste(im, (x, 50))
    draw.text((x + 8, 20), n.upper(), fill=(230, 230, 235))
sheet.save("out/sheet.png")
for n, im in zip(names, imgs):
    im.save(f"out/{n}.png")
print("wrote sim/out/sheet.png")
PY
