#!/usr/bin/env python3
"""Export the retail HUD: weapon, unit and grenade HUD interfaces and their sprites.

Reads the wphi tag of every weapon (flattening its parent chain and keeping the
solo elements), the cyborg's unhi, the grenade grhi tags and the HUD globals
from the Xbox campaign caches. Writes packages/halo/hud.txt and copies the
sprite-sheet pages the asset library exported into packages/textures/halo_hud.
Positions are in the retail 640x480 HUD space; colours are ARGB hex; sprite
rectangles and registration points are normalised to their page.
source/src/halo_hud.cpp reads it.
"""
import argparse
import json
import mmap
import shutil
from pathlib import Path

from import_halo_a30 import HaloMap
from halo_weapons import CAMPAIGN, weapon_index

FORMAT = 1
SOLO = (0, 1)                     # element map types kept: any and solo
# Unit HUD roles, written in the state column of unit elements.
UNIT_BACKGROUND, SHIELD_BACK, SHIELD_METER, HEALTH_BACK, HEALTH_METER = 100, 101, 102, 103, 104
MOTION_BACK, MOTION_FRONT, FLASHLIGHT_BACK, FLASHLIGHT_METER = 105, 106, 107, 108
GRENADE_BACK, GRENADE_COUNT_BACK = 110, 111

FIELDS = '''# bitmap <id> <tag>, then page <width> <height> <file> and sequence lines of
#   sprite <page> <left> <right> <top> <bottom> <registration x> <registration y>
# placement columns: <offset x> <offset y> <scale x> <scale y> <scaling flags>
# static <state> <bitmap> <sequence> <placement> <colour> <flash colour> <flash period>
# meter <state> <bitmap> <sequence> <placement> <min colour> <max colour> <flash colour> <empty colour>
#       <flags> <minimum> <alpha multiplier> <alpha bias> <value scale> <opacity> <translucency>
# number <state> <placement> <colour> <flash colour> <digits> <flags> <fraction digits> <weapon flags>
# crosshair <type> <bitmap> <sequence> <placement> <colour> <flash colour> <flash period> <frame rate> <flags>
# overlay <state> <bitmap> <sequence> <placement> <colour> <flash colour> <frame rate> <show on> <flags>
# weapon <weapons.txt index> <anchor> <total cutoff> <loaded cutoff> <heat cutoff> <age cutoff> [<scope mask bitmap>]
# unit <anchor> / grenade <index> <anchor> <flash cutoff> / end closes each
# motion <centre x> <centre y> <range> <scale> <sweep bitmap> <blip bitmap>
# health <mid colour> <max cutoff> <min cutoff>
# counter <bitmap> <character width> <screen width> <x offset> <y offset> <decimal width> <colon width>
'''


class Exporter:
    def __init__(self, root):
        self.root = root
        catalog = json.loads((root / 'build/retail-assets/catalog.json').read_text())
        self.catalog = catalog.get('bitmaps', {})
        self.bitmaps = {}            # tag path -> id
        self.bitmap_lines = []
        self.textures = root / 'packages/textures/halo_hud'

    def bitmap(self, halo, tag):
        """Register a bitm tag; returns its id, copying its pages once."""
        if not tag or tag.group != 'bitm':
            return -1
        if tag.name in self.bitmaps:
            return self.bitmaps[tag.name]
        entry = self.catalog.get(tag.name)
        if not entry:
            return -1
        index = len(self.bitmaps)
        self.bitmaps[tag.name] = index
        lines = [f'bitmap {index} {tag.name}']
        for page, image in enumerate(entry['images']):
            source = self.root / 'build/retail-assets' / image['paths'][0]
            target = self.textures / f'{index}' / f'{page}.png'
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.exists() and (not target.exists() or target.stat().st_mtime < source.stat().st_mtime):
                shutil.copyfile(source, target)
            lines.append(f'page {image["size"][0]} {image["size"][1]} halo_hud/{index}/{page}.png')
        t = halo.tags
        for sequence in t.block(tag.address + 84, 64):
            lines.append('sequence')
            first, count = t.read('hh', sequence + 32)
            sprites = t.block(sequence + 52, 32)
            if sprites:
                for sprite in sprites:
                    page, = t.read('h', sprite)
                    left, right, top, bottom = t.read('4f', sprite + 8)
                    rx, ry = t.read('2f', sprite + 24)
                    lines.append(f'sprite {page} {n(left)} {n(right)} {n(top)} {n(bottom)} {n(rx)} {n(ry)}')
            else:
                # Plain sequences span whole pages.
                for page in range(first, first + max(count, 1)):
                    lines.append(f'sprite {page} 0 1 0 1 0.5 0.5')
        lines.append('end')
        self.bitmap_lines += lines
        return index


def n(value):
    text = f'{value:.6g}'
    return '0' if text == '-0' else text


def placement(t, a):
    ox, oy = t.read('2h', a)
    sx, sy = t.read('2f', a + 4)
    flags, = t.read('h', a + 12)
    # Number elements leave their scale at zero, meaning unscaled.
    return f'{ox} {oy} {n(sx or 1)} {n(sy or 1)} {flags}'


def colours(t, a):
    default, flash = t.read('2I', a)
    period, = t.read('f', a + 8)
    return f'{default:08x} {flash:08x} {n(period)}'


def static_line(exp, halo, a, state):
    t = halo.tags
    bitmap = exp.bitmap(halo, halo.reference(t, a + 0x24))
    sequence, = t.read('h', a + 0x54)
    return f'static {state} {bitmap} {sequence} {placement(t, a)} {colours(t, a + 0x34)}'


def meter_line(exp, halo, a, state):
    t = halo.tags
    bitmap = exp.bitmap(halo, halo.reference(t, a + 0x24))
    low, high, flash, empty = t.read('4I', a + 0x34)
    flags, minimum = t.read('2B', a + 0x44)
    sequence, = t.read('h', a + 0x46)
    multiplier, bias = t.read('2B', a + 0x48)
    scale, = t.read('h', a + 0x4A)
    opacity, translucency = t.read('2f', a + 0x4C)
    return (f'meter {state} {bitmap} {sequence} {placement(t, a)} {low:08x} {high:08x} {flash:08x} {empty:08x} '
            f'{flags} {minimum} {multiplier} {bias} {scale} {n(opacity)} {n(translucency)}')


def number_line(halo, a, state, weapon_flags=0):
    t = halo.tags
    digits, flags = t.read('2B', a + 0x44)
    fraction, = t.read('h', a + 0x46)
    default, flash = t.read('2I', a + 0x24)
    return f'number {state} {placement(t, a)} {default:08x} {flash:08x} {digits} {flags} {fraction} {weapon_flags}'


def weapon_lines(exp, halo, wphi):
    """Flatten a wphi and its parents, keeping solo elements; child elements first."""
    t = halo.tags
    lines, scope = [], -1
    anchor, cutoffs = None, None
    seen = set()
    while wphi and wphi.group == 'wphi' and wphi.name not in seen:
        seen.add(wphi.name)
        a = wphi.address
        if anchor is None:
            anchor, = t.read('h', a + 0x3C)
            cutoffs = t.read('4h', a + 0x14)
        for e in t.block(a + 0x60, 0xB4):
            state, kind = t.read('h', e)[0], t.read('h', e + 4)[0]
            if kind in SOLO:
                lines.append(static_line(exp, halo, e + 0x24, state))
        for e in t.block(a + 0x6C, 0xB4):
            state, kind = t.read('h', e)[0], t.read('h', e + 4)[0]
            if kind in SOLO:
                lines.append(meter_line(exp, halo, e + 0x24, state))
        for e in t.block(a + 0x78, 0xA0):
            state, kind = t.read('h', e)[0], t.read('h', e + 4)[0]
            if kind in SOLO:
                lines.append(number_line(halo, e + 0x24, state, t.read('h', e + 0x78)[0]))
        for c in t.block(a + 0x84, 0x68):
            kind, maptype = t.read('h', c)[0], t.read('h', c + 4)[0]
            if maptype not in SOLO:
                continue
            bitmap = exp.bitmap(halo, halo.reference(t, c + 0x24))
            for item in t.block(c + 0x34, 0x6C):
                rate, sequence = t.read('2h', item + 0x44)
                flags, = t.read('I', item + 0x48)
                lines.append(f'crosshair {kind} {bitmap} {sequence} {placement(t, item)} {colours(t, item + 0x24)} {rate} {flags}')
        for o in t.block(a + 0x90, 0x68):
            state, maptype = t.read('h', o)[0], t.read('h', o + 4)[0]
            if maptype not in SOLO:
                continue
            bitmap = exp.bitmap(halo, halo.reference(t, o + 0x24))
            for item in t.block(o + 0x34, 0x88):
                default, flash = t.read('2I', item + 0x24)
                rate, = t.read('h', item + 0x44)
                sequence, show = t.read('2h', item + 0x48)
                flags, = t.read('I', item + 0x4C)
                lines.append(f'overlay {state} {bitmap} {sequence} {placement(t, item)} {default:08x} {flash:08x} {rate} {show} {flags}')
        effects = t.block(a + 0xAC, 0xB8)
        if effects and scope < 0:
            scope = exp.bitmap(halo, halo.reference(t, effects[0] + 0x18))
        wphi = halo.reference(t, a)
    return anchor or 0, cutoffs or (0, 0, 0, 0), scope, lines


def find(halo, group, name=None):
    for tag in halo.directory:
        if tag.group == group and (name is None or tag.name == name):
            return tag
    return None


def unit_lines(exp, halo):
    t = halo.tags
    cyborg = find(halo, 'bipd', r'characters\cyborg\cyborg')
    huds = t.block(cyborg.address + 680, 48) if cyborg else []
    unhi = halo.reference(t, huds[0]) if huds else None
    if not unhi or unhi.group != 'unhi':
        return []
    a = unhi.address
    lines = [f'unit {t.read("h", a)[0]}',
             static_line(exp, halo, a + 0x24, UNIT_BACKGROUND),
             static_line(exp, halo, a + 0x8C, SHIELD_BACK), meter_line(exp, halo, a + 0x8C + 0x68, SHIELD_METER),
             static_line(exp, halo, a + 0x17C, HEALTH_BACK), meter_line(exp, halo, a + 0x17C + 0x68, HEALTH_METER),
             static_line(exp, halo, a + 0x26C, MOTION_BACK), static_line(exp, halo, a + 0x2D4, MOTION_FRONT)]
    mid, = t.read('I', a + 0x17C + 0xD0)
    high, low = t.read('2f', a + 0x17C + 0xD4)
    lines.append(f'health {mid:08x} {n(high)} {n(low)}')
    cx, cy = t.read('2h', a + 0x35C)
    globals_tag = find(halo, 'matg')
    interface = t.block(globals_tag.address + 0x140, 304)[0]
    hudg = halo.reference(t, interface + 0x60)
    sweep = exp.bitmap(halo, halo.reference(t, interface + 0x70))
    blip = exp.bitmap(halo, halo.reference(t, interface + 0xC0))
    motion_range, _, motion_scale = t.read('3f', hudg.address + 0x2D0) if hudg else (20, 0, 32)
    lines.append(f'motion {cx} {cy} {n(motion_range)} {n(motion_scale)} {sweep} {blip}')
    for aux in t.block(a + 0x3CC, 0x144):
        kind, = t.read('h', aux)
        if kind == 0:   # the flashlight
            lines += [static_line(exp, halo, aux + 0x14, FLASHLIGHT_BACK), meter_line(exp, halo, aux + 0x14 + 0x68, FLASHLIGHT_METER)]
    lines.append('end')
    return lines


def grenade_lines(exp, halo):
    t = halo.tags
    globals_tag = find(halo, 'matg')
    lines = []
    for index, grenade in enumerate(t.block(globals_tag.address + 0x128, 68)):
        grhi = halo.reference(t, grenade + 0x14)
        if not grhi or grhi.group != 'grhi':
            continue
        a = grhi.address
        cutoff, = t.read('h', a + 0x148)
        lines += [f'grenade {index} {t.read("h", a)[0]} {cutoff}',
                  static_line(exp, halo, a + 0x24, GRENADE_BACK), static_line(exp, halo, a + 0x8C, GRENADE_COUNT_BACK),
                  number_line(halo, a + 0xF4, 0)]
        bitmap = exp.bitmap(halo, halo.reference(t, a + 0x14C))
        for item in t.block(a + 0x15C, 0x88):
            default, flash = t.read('2I', item + 0x24)
            rate, = t.read('h', item + 0x44)
            sequence, show = t.read('2h', item + 0x48)
            lines.append(f'overlay 0 {bitmap} {sequence} {placement(t, item)} {default:08x} {flash:08x} {rate} {show} 0')
        lines.append('end')
    interface = t.block(globals_tag.address + 0x140, 304)[0]
    counter = halo.reference(t, interface + 0xB0)
    if counter:
        bitmap = exp.bitmap(halo, halo.reference(t, counter.address))
        metrics = t.read('6B', counter.address + 16)
        lines.append(f'counter {bitmap} ' + ' '.join(map(str, metrics)))
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    weapons = weapon_index(args.output)
    if not weapons:
        raise SystemExit('run tools/halo_weapons.py first to write packages/halo/weapons.txt')
    exp = Exporter(args.output)
    weapon_blocks, done, unit, grenades = [], set(), [], []
    for cache in CAMPAIGN:
        with (args.maps / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            if not unit:
                unit = unit_lines(exp, halo)
                grenades = grenade_lines(exp, halo)
            for tag in halo.directory:
                if tag.group != 'weap' or tag.name in done or tag.name not in weapons:
                    continue
                done.add(tag.name)
                wphi = halo.reference(halo.tags, tag.address + 1152)
                anchor, cutoffs, scope, lines = weapon_lines(exp, halo, wphi)
                weapon_blocks += [f'weapon {weapons[tag.name]} {anchor} ' + ' '.join(map(str, cutoffs)) + f' {scope}',
                                  *lines, 'end']
    out = [f'format {FORMAT}', FIELDS.rstrip(), *exp.bitmap_lines, *unit, *grenades, *weapon_blocks]
    target = args.output / 'packages/halo/hud.txt'
    target.write_text('\n'.join(out) + '\n')
    print(f'{len(done)} weapon HUDs, {len(exp.bitmaps)} sprite sheets -> {target}')


if __name__ == '__main__':
    main()
