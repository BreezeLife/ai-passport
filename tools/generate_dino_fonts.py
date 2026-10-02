#!/usr/bin/env python3
"""Reproducible uncompressed LVGL 9 subsets for the fixed DinoBook UI."""
import hashlib
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'assets/fonts/SourceHanSansSC-Normal.otf'
SOURCE_HASH = '1ee89e1669362dee13851129c0a8a791a87521eb4148e5efbf5d26596738e25b'

def characters():
    inventory = set(chr(i) for i in range(32,127))
    for p in sorted((ROOT / 'main').glob('dino_*.c')):
        for literal in re.findall(r'"(?:\\.|[^"\\])*"', p.read_text()):
            inventory.update(c for c in literal[1:-1] if ord(c) >= 128)
    return inventory

def check():
    expected = {ord(c) for c in characters()}
    for size in (14,18,24):
        p = ROOT / f'assets/fonts/dino_font_{size}.c'
        have = {int(x,16) for x in re.findall(r'/\* U\+([0-9A-Fa-f]+)',p.read_text())}
        missing = expected - have
        assert not missing, f'{p.name}: missing {sorted(missing)}'
        assert 0x9F98 not in have, 'negative coverage probe unexpectedly present'
        print(f'{p.name}: PASS, {len(expected)} glyphs; known-missing U+9F98 absent')

if __name__ == '__main__':
    import sys
    if '--check' in sys.argv:
        check()
    else:
        assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == SOURCE_HASH
        converter = ROOT / 'node_modules/.bin/lv_font_conv'
        assert subprocess.check_output([str(converter),'--version'],text=True).strip() == '1.5.3'
        chars = ''.join(sorted(characters()))
        (ROOT/'assets/fonts/characters.txt').write_text(chars+'\n')
        for size in (14,18,24):
            out=ROOT/f'assets/fonts/dino_font_{size}.c'
            subprocess.run([str(converter),'--font',str(SOURCE),'--symbols',chars,
                            '--size',str(size),'--bpp','2','--format','lvgl',
                            '--no-compress','--no-kerning','--lv-include','lvgl.h',
                            '--lv-font-name',f'dino_font_{size}','--output',str(out)],check=True)
            s=out.read_text().replace(str(ROOT),'.')
            out.write_text(s)
        check()
