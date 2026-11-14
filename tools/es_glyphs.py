# Spanish letters for the game font: prototype of the runtime's glyph builder (text_hook.cpp
# make_glyphs, language "es") plus a preview.  python es_glyphs.py <GR3.MDT> <out.png> [text...]
# Spanish reuses glyph slots of German/French letters it does not need (letters.py EXTRA_ES):
# é, É, ü, Ü stay where they are. New: acute over the dotless i, a tilde for ñ/Ñ, and ¡ ¿ as the
# game's ! and ? turned by 180 degrees.
import os
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fr_glyphs as fg
from PIL import Image

fg.NARROW['acute'] = ['..#', '.#.']
fg.LOW['tilde'] = ['......', '.##..#', '#..##.']
fg.CAP['tilde'] = ['.##..#', '#..##.']

# (char, glyph, base, accent); accent 'turn' = the base character turned by 180 degrees
ES = [('á', 0x98, 'a', 'acute'), ('ó', 0x99, 'o', 'acute'), ('Á', 0x9B, 'A', 'acute'), ('Ó', 0x9C, 'O', 'acute'),
      ('ñ', 0x9E, 'n', 'tilde'), ('í', 0x9F, 'i', 'acute'), ('ú', 0xA0, 'u', 'acute'), ('Ñ', 0xA1, 'N', 'tilde'),
      ('é', 0xA2, 'e', 'acute'), ('Í', 0xA3, 'I', 'acute'), ('Ú', 0xA4, 'U', 'acute'), ('¡', 0xA5, '!', 'turn'),
      ('¿', 0xA6, '?', 'turn'), ('É', 0xAD, 'E', 'acute')]


def turn(gl, wd, base):
    g0 = gl[ord(base) - 0x20]
    off, adv = wd[ord(base) - 0x20]
    ink = [(x, y) for y in range(16) for x in range(16) if g0[y][x] > 4]
    x0, x1 = min(x for x, y in ink), max(x for x, y in ink)
    y1 = max(y for x, y in ink)
    g = [[g0[15 - y][15 - x] for x in range(16)] for y in range(16)]
    dx = x0 - (15 - x1)              # same columns as the original
    dy = (y1 + 1) - y1               # one row lower: the turned sign reaches below the line... see below
    # turned, the old top row becomes the bottom: put the bottom one row below the original's bottom
    ink2 = [(x, y) for y in range(16) for x in range(16) if g[y][x] > 4]
    b2 = max(y for x, y in ink2)
    dy = (y1 + 1) - b2
    out = [[0] * 16 for _ in range(16)]
    for y in range(16):
        for x in range(16):
            if g[y][x] and 0 <= y + dy < 16 and 0 <= x + dx < 16:
                out[y + dy][x + dx] = g[y][x]
    return out, (off, adv)


def make(gl, wd, base, accent):
    if accent == 'turn':
        return turn(gl, wd, base)
    g, (off, adv) = fg.make(gl, wd, base, accent)
    if base == 'i':  # the acute leans right: no extra room on the left, one column more instead of two
        off, adv = off - 1, adv - 1
    return g, (off, adv)


if __name__ == '__main__':
    gl, wd = fg.font(sys.argv[1])
    extra = {c: make(gl, wd, b, a) for c, _, b, a in ES}
    # ü/Ü (German dots) for the preview
    extra['ü'] = fg.make(gl, wd, 'u', 'dots')
    extra['Ü'] = fg.make(gl, wd, 'U', 'dots')
    lines = sys.argv[3:] or ['áéíóúñü ÁÉÍÓÚÑÜ ¡! ¿?',
                             '¡Hola! ¿Qué pasa, señora?',
                             'Año, niño, mañana, después, aquí.',
                             'Ángel, Último, Ícaro, Óscar, ÑANDÚ',
                             '¿Cómo viviste en el bosque? ¡Ya!']
    imgs = [fg.render(gl, wd, extra, t) for t in lines]
    out = Image.new('RGB', (max(i.width for i in imgs), sum(i.height for i in imgs)))
    y = 0
    for i in imgs:
        out.paste(i, (0, y))
        y += i.height
    out.save(sys.argv[2])
