# Extra letters of the patched game font (runtime: text_hook.cpp EXTRA, sys_text.cpp DE).
# Glyphs 0x98.. in this order; font code / script byte = glyph + 0x20 (0xB8..0xD1).
import os

EXTRA = 'äöüÄÖÜß' + 'àâçéèêëîïôùûÀÇÉÈÊÔÂ'
# base letter for width measuring
BASE = dict(zip(EXTRA, 'aouAOUB' + 'aaceeeeiiouuACEEEOA'))

# Spanish (game language "es"; runtime: text_hook.cpp EXTRA_ES, sys_text.cpp ES): the same 26 slots,
# letters Spanish does not need replaced; é É ü Ü stay where they are. ¡ ¿ are ! ? turned by 180
# degrees (preview: es_glyphs.py).
EXTRA_ES = 'áóüÁÓÜñíúÑéÍÚ¡¿' + 'ïôùûÀÇÉÈÊÔÂ'
BASE_ES = dict(zip(EXTRA_ES, 'aouAOUniuNeIU!?' + 'iouuACEEEOA'))

# The tools build for German/French unless G3_LANG=es is set (set G3_LANG=es before make_lang.py,
# make_sys.py, make_radio.py and btl_text.py for Spanish files).
if os.environ.get('G3_LANG') == 'es':
    EXTRA, BASE = EXTRA_ES, BASE_ES
CODE = {ch: 0xB8 + i for i, ch in enumerate(EXTRA)}


def normalize(s):
    """characters the font lacks, replaced like the runtime does"""
    for a, b in (('œ', 'oe'), ('Œ', 'Oe'), ('«', '"'), ('»', '"'), (' ', ' '), (' ', ' ')):
        s = s.replace(a, b)
    return s
