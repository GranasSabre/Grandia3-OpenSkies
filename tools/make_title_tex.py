# Title screen: "OPEN SKIES" under the GRANDIA logo (project name "Grandia III: Open Skies").
#   python make_title_tex.py <sword texture PNG> <out PNG>
# The sword sprite of the title screen (texture key 2fcf534e5a0ca4b5, 512x512) reaches down to just
# below the logo's yellow brush stroke; its transparent bottom area gets the text. The input may be the
# original (texdump) or an integer multiple of it (the 4x HD version); the output has the input's size
# and goes to lang\all_tex\2fcf534e5a0ca4b5.png (shown in every language).
# Geometry from a grid test (title screen, internal frame 2048x1792 = 4x): texture x -> frame
# 322.5 + 1.7344 x, texture y -> frame 111 + 2.702 y. The text is laid out in frame pixels like the
# approved draft (work/titelbildschirm_unter_logo_entwurf.png, left) and mapped into the texture.
import sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

FX0, FSX, FY0, FSY = 322.5, 1.7344, 111.0, 2.702   # texture (512 px) -> frame (2048x1792)
FONT = 'C:/Windows/Fonts/palab.ttf'                 # Palatino Linotype Bold
TEXT = 'OPEN SKIES'
LOGO_X0, LOGO_X1, LOGO_CAP = 450, 991, 116          # GRANDIA (with the (R) mark) in the frame
QUAD_BOTTOM = 111.0 + 512 * 2.702                   # lower edge of the sprite in the frame
SS = 4                                              # supersampling of the frame-space layout

src = Image.open(sys.argv[1]).convert('RGBA')
mul = src.width / 512                                # 1 = original, 4 = HD
sx, sy = mul / FSX, mul / FSY                        # frame px -> output texture px

# font size: cap height 42% of GRANDIA's
size = 10
while True:
    b = ImageFont.truetype(FONT, size + 1).getbbox('O')
    if b[3] - b[1] > LOGO_CAP * 0.42:
        break
    size += 1
f = ImageFont.truetype(FONT, size * SS)
track = int(size * 0.12) * SS
d0 = ImageDraw.Draw(Image.new('L', (1, 1)))
width = sum(d0.textlength(c, font=f) for c in TEXT) + track * (len(TEXT) - 1)
bb = f.getbbox('O')
# layer in frame space (x SS), with room for the shadow
pad = size * SS
cap = bb[3] - bb[1]                                  # cap height (x SS)
# thin gold lines left and right of the text, fading outwards, ending where they end in the update
# window's logo (make_updater_art.py, the version the user chose: left shorter) - here, with the smaller
# text, they end at the ends of the brush stroke under GRANDIA. Frame x of the ends:
LINE_L, LINE_R = 405.0, 1090.0
LINE_GAP = 0.5 * cap
text_x0 = (LOGO_X0 + LOGO_X1) / 2 - width / SS / 2  # frame x of the text
padx = (text_x0 - LINE_L) * SS + pad                 # room for the left line
lw, lh = int(padx + width + (LINE_R - text_x0) * SS - width + pad), int(bb[3] + 2 * pad)
fx = text_x0 - padx / SS                             # frame position of the layer
fy = QUAD_BOTTOM - LOGO_CAP * 0.12 - bb[3] / SS - pad / SS


def draw_text(img, ox, oy, fill):
    d = ImageDraw.Draw(img)
    x = ox
    for ch in TEXT:
        d.text((x, oy), ch, font=f, fill=fill)
        x += d.textlength(ch, font=f) + track


shadow = Image.new('L', (lw, lh), 0)
draw_text(shadow, padx + size * SS * 0.05, pad + size * SS * 0.07, 255)
shadow = shadow.filter(ImageFilter.GaussianBlur(size * SS * 0.06))
layer = Image.new('RGBA', (lw, lh), (0, 0, 0, 0))
layer.putalpha(shadow)
text = Image.new('L', (lw, lh), 0)
draw_text(text, padx, pad, 255)
white = Image.new('RGBA', (lw, lh), (255, 255, 255, 255))
white.putalpha(text)
layer.alpha_composite(white)
# the lines: both fade over the right one's length; the left one is cut at the logo's edge
lines = Image.new('L', (lw, lh), 0)
ld = ImageDraw.Draw(lines)
mid = pad + (bb[1] + bb[3]) / 2
t = max(1, round(cap * 0.06))
x1 = padx + width - track                            # end of the text (layer x)
x_l, x_r = (LINE_L - fx) * SS, (LINE_R - fx) * SS
length = x_r - (x1 + LINE_GAP)
N = 80
for side, xa in ((-1, padx - LINE_GAP), (1, x1 + LINE_GAP)):
    for i in range(N):
        xs, xe = xa + side * length * i / N, xa + side * length * (i + 1) / N
        a, b = min(xs, xe), max(xs, xe)
        if b <= x_l or a >= x_r:
            continue
        ld.line([(max(a, x_l), mid), (min(b, x_r), mid)], fill=int(255 * (1 - i / N)), width=t)
gold = Image.new('RGBA', (lw, lh), (236, 178, 64, 255))
gold.putalpha(lines)
layer.alpha_composite(gold)

# into texture space: x and y scaled differently
tw_, th_ = max(1, round(lw / SS * sx)), max(1, round(lh / SS * sy))
layer = layer.resize((tw_, th_), Image.LANCZOS)
tx, ty = round((fx - FX0) * sx), round((fy - FY0) * sy)
out = src.copy()
# keep inside the texture (the shadow's blur may reach the last row)
if ty + th_ > out.height:
    layer = layer.crop((0, 0, layer.width, out.height - ty))
if tx < 0:
    layer = layer.crop((-tx, 0, layer.width, layer.height))
    tx = 0
if tx + layer.width > out.width:
    layer = layer.crop((0, 0, out.width - tx, layer.height))
out.alpha_composite(layer, (tx, ty))
out.save(sys.argv[2])
print(f'{sys.argv[2]}: {out.size}, text layer {tw_}x{th_} at {tx},{ty} (font {size}px in the frame)')
