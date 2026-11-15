# Background of the update window (updater.cpp): the title screen logo (sword, "GRANDIA III", wings,
# feathers, "OPEN SKIES") over the sky, fading into the window's dark blue where the text and buttons go.
#   python make_updater_art.py <textures dir> <sword PNG> <sky screenshot> <out JPG>
#   <textures dir>: the HD replacements (16450f57978f8788 logo, 43c22ddd5315a58f glow, 90caf764fe05e3bc
#                   feathers / (R)); <sword PNG>: the HD sword 2fcf534e5a0ca4b5.png without the text (drawn here, below the glow)
# The logo is put together from its sprites like on the title screen. Placement in the internal frame
# (2048 x 1792, 4x), measured on a title screen picture:
#   sword + glow quad:  x = 322.5 + 1.7344 u, y = 111 + 2.702 v   (u, v in 512ths of the texture)
#   "III" + wings:      x = 626.5 + 0.3536 u, y = 712.3 + 0.5448 v (1024 logo texture, columns < 700)
#   "GRANDIA":          the logo texture turned 90 deg clockwise, x = 439.3 + 0.5434 u, y = 685.8 + 0.8349 v
#   feathers / (R):     the scale of the sword quad, centred where they float on the title screen
# The frame's pixels are not square: shown at 4:3 its height shrinks by 1536 / 1792.
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageEnhance, ImageFont

TEX, SWORD, SKY, OUT = sys.argv[1:5]
W, H = 760, 1400                 # 2x of the logical window size (380 x 700)
LOGO_W = 470                     # logo width in the image (2x)
LOGO_Y = 18
NAVY = (12, 22, 52)              # window background below (updater.cpp: BG)
PALATINO = 'C:/Windows/Fonts/palab.ttf'
K = 2                            # composition at twice the frame resolution
ASPECT = 1536 / 1792

canvas = Image.new('RGBA', (2048 * K, 1792 * K), (0, 0, 0, 0))


def put(img, ox, oy, sx, sy):
    """img's pixel (x, y) goes to frame (ox + sx x, oy + sy y)"""
    w, h = max(1, round(img.width * sx * K)), max(1, round(img.height * sy * K))
    canvas.alpha_composite(img.resize((w, h), Image.LANCZOS), (round(ox * K), round(oy * K)))


def tex(key):
    return Image.open(f'{TEX}/{key}.png').convert('RGBA')


sword = Image.open(SWORD).convert('RGBA')
glow = tex('43c22ddd5315a58f')
logo = tex('16450f57978f8788')
sheet = tex('90caf764fe05e3bc')
sm = sheet.width / 512            # the feather sheet is 512 originally

put(glow, 322.5, 111, 1.7344 * 512 / glow.width, 2.702 * 512 / glow.height)
put(sword, 322.5, 111, 1.7344 * 512 / sword.width, 2.702 * 512 / sword.height)
# feathers: (sheet box at 512 size, centre in the frame)
for (x0, y0, x1, y1), (cx, cy) in [((105, 211, 182, 274), (569, 760)), ((139, 288, 251, 355), (602, 948)),
                                   ((381, 306, 463, 369), (1057, 1013)), ((77, 333, 140, 381), (507, 1067))]:
    f = sheet.crop((x0 * sm, y0 * sm, x1 * sm, y1 * sm))
    sx, sy = 1.7344 / sm, 2.702 / sm
    put(f, cx - f.width * sx / 2, cy - f.height * sy / 2, sx, sy)
lm = logo.width / 1024
put(logo.crop((0, 0, 700 * lm, logo.height)), 626.5, 712.3, 0.3536 / lm, 0.5448 / lm)
rot = logo.rotate(-90, expand=True)
put(rot.crop((0, 700 * lm, rot.width, rot.height)), 439.3, 685.8 + 0.8349 * 700, 0.5434 / lm, 0.8349 / lm)
reg = sheet.crop((260 * sm, 422 * sm, 318 * sm, 480 * sm))
sx, sy = 1.7344 / sm * 0.5, 2.702 / sm * 0.5
put(reg, 985 - reg.width * sx / 2, 1312 - reg.height * sy / 2, sx, sy)

bb = canvas.getbbox()
art = canvas.crop(bb)
art = art.resize((art.width, round(art.height * ASPECT)), Image.LANCZOS)
f = LOGO_W / art.width                      # frame px (x K) -> logo px
art = art.resize((LOGO_W, round(art.height * f)), Image.LANCZOS)

# "OPEN SKIES" below the sword's glow (on the title screen it sits on the sword sprite itself,
# make_title_tex.py; here there is room for it under the glow): Palatino Bold, cap height 62 % of
# GRANDIA's, centred under GRANDIA, white with a soft shadow
fx = lambda x: (x * K - bb[0]) * f
cap = 116 * K * ASPECT * f * 0.62
SS = 4
size = 10
while True:
    b = ImageFont.truetype(PALATINO, (size + 1) * SS).getbbox('O')
    if b[3] - b[1] > cap * SS:
        break
    size += 1
font = ImageFont.truetype(PALATINO, size * SS)
track = size * SS * 0.12
d0 = ImageDraw.Draw(Image.new('L', (1, 1)))
tw = sum(d0.textlength(c, font=font) for c in 'OPEN SKIES') + track * 9
ob = font.getbbox('O')
gap = cap * 0.25
th = art.height + round(gap + cap * 1.3)
lay = Image.new('L', (LOGO_W * SS, th * SS), 0)
import os
CENTER = os.environ.get('CENTER', 'grandia')  # grandia: under GRANDIA; window: in the window's middle
x = ((fx(450) + fx(991)) / 2 if CENTER == 'grandia' else W / 2 - (W - LOGO_W) // 2) * SS - tw / 2
y = (art.height + gap) * SS - ob[1]
def tracked(img, x, y):
    dd = ImageDraw.Draw(img)
    for ch in 'OPEN SKIES':
        dd.text((x, y), ch, font=font, fill=255)
        x += dd.textlength(ch, font=font) + track
tracked(lay, x, y)
shadow = Image.new('L', lay.size, 0)
tracked(shadow, x + size * SS * 0.05, y + size * SS * 0.07)
shadow = shadow.filter(ImageFilter.GaussianBlur(size * SS * 0.06))
lay, shadow = lay.resize((LOGO_W, th), Image.LANCZOS), shadow.resize((LOGO_W, th), Image.LANCZOS)
full = Image.new('RGBA', (LOGO_W, th), (0, 0, 0, 0))
full.paste(art, (0, 0))
sh = Image.new('RGBA', (LOGO_W, th), (0, 0, 0, 255))
sh.putalpha(shadow)
full.alpha_composite(sh)
wt = Image.new('RGBA', (LOGO_W, th), (255, 255, 255, 255))
wt.putalpha(lay)
full.alpha_composite(wt)
# decoration next to / under the text (env DECOR: lines = default, none, underline, feathers)
import os
DECOR = os.environ.get('DECOR', 'lines')  # chosen: lines (user, 2026-11-15)
GOLD = (236, 178, 64)
tx0, tx1 = x / SS, (x + tw - track) / SS            # text extent in logo px
ty0, ty1 = (y + ob[1]) / SS, (y + ob[3]) / SS
mid = (ty0 + ty1) / 2
deco = None
if DECOR in ('lines', 'underline'):
    # drawn over the whole window width (the logo area is narrower than the lines: the left one was cut)
    OX = (W - LOGO_W) // 2
    tx0, tx1 = tx0 + OX, tx1 + OX
    L = Image.new('L', (W * SS, th * SS), 0)
    ld = ImageDraw.Draw(L)
    t = max(1, round(cap * (0.06 if DECOR == "lines" else 0.09) * SS))
    if DECOR == 'lines':  # thin gold lines left and right, fading outwards
        ln = cap * 2.2
        for side in (-1, 1):
            xa = tx0 - cap * 0.5 if side < 0 else tx1 + cap * 0.5
            n = 60
            for i in range(n):
                a = 1 - i / n
                xs, xe = xa + side * ln * i / n, xa + side * ln * (i + 1) / n
                ld.line([(min(xs, xe) * SS, mid * SS), (max(xs, xe) * SS, mid * SS)], fill=int(255 * a), width=t)
    else:  # underline, fading at both ends
        uy = ty1 + cap * 0.32
        n = 120
        for i in range(n):
            u = i / n
            a = min(1, 4 * u, 4 * (1 - u))
            xs = tx0 - cap * 0.3 + (tx1 - tx0 + cap * 0.6) * u
            xe = xs + (tx1 - tx0 + cap * 0.6) / n
            ld.line([(xs * SS, uy * SS), (xe * SS, uy * SS)], fill=int(255 * a), width=t)
    L = L.resize((W, th), Image.LANCZOS)
    deco = Image.new('RGBA', (W, th), GOLD + (255,))
    deco.putalpha(L)
    # lines only inside the logo area (chosen by the user: the left one is shorter, like the brush stroke)
    if os.environ.get('CLIP', '1') == '1':
        # frame x of the line ends (for make_title_tex.py)
        to_frame = lambda px: (px - OX) / f / K + bb[0] / K
        print('lines in the frame: left end %.1f, right end %.1f' % (to_frame(OX), to_frame(tx1 + cap * 0.5 + cap * 2.2)))
        deco.paste((0, 0, 0, 0), (0, 0, OX, th))
        deco.paste((0, 0, 0, 0), (OX + LOGO_W, 0, W, th))
elif DECOR == 'feathers':  # the logo's small feather, mirrored, left and right of the text
    fe = sheet.crop((77 * sm, 333 * sm, 140 * sm, 381 * sm))
    fh = cap * 0.95
    fe = fe.resize((round(fe.width * fh / fe.height), round(fh)), Image.LANCZOS)
    right = fe
    left = fe.transpose(Image.FLIP_LEFT_RIGHT)
    full.alpha_composite(left, (round(tx0 - cap * 0.3 - left.width), round(mid - fh / 2)))
    full.alpha_composite(right, (round(tx1 + cap * 0.3), round(mid - fh / 2)))
art = full
lh = art.height

# sky behind the upper part, fading into the navy
src = Image.open(SKY).convert('RGB')
rows = [y for y in range(src.height) if max(src.getpixel((x, y))[c] for x in range(0, src.width, 97) for c in range(3)) > 12]
src = src.crop((0, rows[0] + 4, src.width, rows[-1] - 4))
SKY_H = LOGO_Y + lh
s = max(W / src.width, SKY_H / src.height)
sky = src.resize((int(src.width * s + 0.5), int(src.height * s + 0.5)), Image.LANCZOS)
x0 = (sky.width - W) // 2
sky = sky.crop((x0, 0, x0 + W, SKY_H)).filter(ImageFilter.GaussianBlur(2))
sky = ImageEnhance.Color(sky).enhance(1.1)
img = Image.new('RGB', (W, H), NAVY)
img.paste(sky, (0, 0))
# (tinted first: white clouds faded straight into the navy would turn grey)
fade0, fade1 = int(SKY_H * 0.45), SKY_H
tint = Image.new('L', (W, H), 0)
grad = Image.new('L', (W, H), 0)
td, gd = ImageDraw.Draw(tint), ImageDraw.Draw(grad)
for y in range(fade0, H):
    t = min(1.0, (y - fade0) / (fade1 - fade0))
    td.line([(0, y), (W, y)], fill=int(255 * min(1.0, t * 1.6)))
    gd.line([(0, y), (W, y)], fill=int(255 * (t * t * (3 - 2 * t))))
dark = Image.merge('RGB', [c.point(lambda v, k=k: int(v * k)) for c, k in zip(img.split(), (0.30, 0.42, 0.75))])
img = Image.composite(dark, img, tint)
img = Image.composite(Image.new('RGB', (W, H), NAVY), img, grad).convert('RGBA')
img.alpha_composite(art, ((W - LOGO_W) // 2, LOGO_Y))
if deco is not None:
    img.alpha_composite(deco, (0, LOGO_Y))
img.convert('RGB').save(OUT, quality=92, optimize=True)
print(OUT, img.size, 'logo', art.size, 'ends at', LOGO_Y + lh, '(logical', (LOGO_Y + lh) // 2, ')')
