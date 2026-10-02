#!/usr/bin/env python3
"""Validate the 40 dinosaur records and regenerate their firmware catalog."""

import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
CATALOG = ROOT / 'assets/data/dinosaurs.json'
OUTPUT = ROOT / 'main/dino_catalog.c'
IDS = (
    'tyrannosaurus triceratops stegosaurus brachiosaurus diplodocus ankylosaurus '
    'spinosaurus velociraptor allosaurus apatosaurus camarasaurus iguanodon '
    'parasaurolophus corythosaurus lambeosaurus edmontosaurus maiasaura '
    'pachycephalosaurus homalocephale psittacosaurus protoceratops styracosaurus '
    'centrosaurus pachyrhinosaurus carnotaurus ceratosaurus dilophosaurus '
    'coelophysis compsognathus gallimimus ornithomimus oviraptor therizinosaurus '
    'deinonychus microraptor baryonyx suchomimus giganotosaurus argentinosaurus saltasaurus'
).split()


def han_count(text):
    return len(re.findall(r'[\u4e00-\u9fff]', text))


def validate(records):
    if [entry['id'] for entry in records] != IDS:
        raise ValueError('Expected exactly 40 dinosaurs in the fixed catalog order')
    for index, entry in enumerate(records):
        for key in ('name', 'latin', 'period', 'diet', 'trait', 'question', 'hint', 'anatomy_en', 'source'):
            if not isinstance(entry.get(key), str) or not entry[key].strip():
                raise ValueError('Missing catalog text: %s/%s' % (entry['id'], key))
        for key, lower, upper in (('facts', 25, 40), ('spoken_facts', 16, 20)):
            if len(entry[key]) != 2 or not all(lower <= han_count(s) <= upper for s in entry[key]):
                raise ValueError('Fact length/count mismatch: %s/%s' % (entry['id'], key))
        if len(entry['options']) != 2 or not all(0 < len(s) <= 10 for s in entry['options']):
            raise ValueError('Expected two short options: ' + entry['id'])
        if entry['correct'] != index % 2 or len(entry['hint']) > 30:
            raise ValueError('Answer alternation/hint length mismatch: ' + entry['id'])
        if not all(url.startswith('https://') for url in [entry['source']] + entry.get('sources', [])):
            raise ValueError('Missing HTTPS source: ' + entry['id'])


def render(records):
    quote = lambda value: json.dumps(value, ensure_ascii=False)
    lines = ['#include "dino_catalog.h"',
             '/* Generated from assets/data/dinosaurs.json by tools/generate_dino_catalog.py. */',
             'const dino_entry_t dino_catalog[DINO_COUNT] = {']
    for entry in records:
        lines.append('    { ' + ', '.join(quote(entry[k]) for k in ('name', 'latin', 'period', 'diet', 'trait')) + ',')
        lines.append('      { ' + ', '.join(map(quote, entry['facts'])) + ' },')
        lines.append('      ' + quote(entry['question']) + ', { ' + ', '.join(map(quote, entry['options'])) +
                     ' }, ' + quote(entry['hint']) + ', ' + str(entry['correct']) + ' },')
    return '\n'.join(lines + ['};', ''])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', action='store_true', help='validate records and generated C without writing')
    args = parser.parse_args()
    records = json.loads(CATALOG.read_text(encoding='utf-8'))
    validate(records)
    expected = render(records)
    if args.verify:
        if OUTPUT.read_text(encoding='utf-8') != expected:
            raise ValueError('Generated firmware catalog is stale')
    else:
        OUTPUT.write_text(expected, encoding='utf-8')
    print('Validated 40 dinosaurs, 80 facts and 40 observation questions.')


if __name__ == '__main__':
    main()
