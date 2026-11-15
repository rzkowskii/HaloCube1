#!/usr/bin/env python3
"""Export the retail effects the mission scripts create, the flashlight and the sound classes.

packages/halo/effects.txt (git-ignored like the rest of packages/) is read by
source/src/halo_fx.cpp:

- every `effe` tag a mission script names (the effect constants of effect_new,
  effect_new_on_object_marker and the d40 effect globals): its first sound, its first light
  (radius in world units, colour, duration), the largest area radius of its `jpt!` parts and
  how many particle systems and particles it carries. The engine draws an effect with
  AssaultCube's own particles (fireballs, sparks, smoke) and a flash sprite sized from these;
  the tags' particle systems, decals and debris are not converted, and effect damage is not
  dealt (presentation only).
- the player's flashlight (`characters\\cyborg\\flashlight_cyborg`): radius and cone angles.
- the sound class of every `snd!` tag in each cache (the `classes` lines), which
  sound_class_set_gain fades.

tools/import_halo_audio.py decodes the effects' sounds (the `sound play` lines).
"""
import argparse
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap
from halo_weapons import CAMPAIGN, EFFECT_EVENTS, EVENT_STRIDE, EVENT_PARTS, PART_STRIDE, PART_TYPE, \
    effect_sound, jpt_damage, number

FORMAT = 1
EVENT_PARTICLES, PARTICLE_STRIDE = 56, 232     # an effect event's particles block
# ligh: radius at 4, the cone's falloff and cutoff angles at 16 and 20 (radians), the colour's
# upper bound (ARGB floats) at 72 and the duration at 244 (ticks when above a few, else seconds).
LIGHT_RADIUS, LIGHT_FALLOFF, LIGHT_CUTOFF, LIGHT_COLOR, LIGHT_DURATION = 4, 16, 20, 72, 244
SOUND_CLASS = 4                                # snd!: the class (16-bit) before the sample rate
HS_EFFECT = 25                                 # HaloScript value type of an effect constant
FLASHLIGHT = r'characters\cyborg\flashlight_cyborg'

FIELDS = '''# flashlight <radius> <falloff angle> <cutoff angle> <r> <g> <b>   world units, radians
# effect <index>
#   path <effe tag>
#   kind explosion|burst|smoke|glow <scale>   how the engine draws it with its own particles, and how
#                                         big (1.6 the tags' large particle systems, 0.6 small ones)
#   light <radius> <r> <g> <b> <seconds>  the first light part
#   blast <radius>                        the largest jpt! area radius (size only: no damage)
#   particles <systems> <particles>       particle system and particle parts it carries
#   sound play <snd! tag>                 the first sound part
#   end
# classes <cache> <tag index>:<class> ...  every snd! of the cache; class 32 music, 33-35 ambient,
#                                          23 vehicle engine, 4 weapon fire, 19 unit dialog, ...
'''


def script_effects(halo):
    """effe tag names named by the scenario's script constants (op 25 nodes, globals included)."""
    from import_halo_scripts import Program
    try:
        program = Program(halo)
    except ValueError:
        return set()
    found = set()
    for salt, op, ty, flags, _, _, data in program.nodes:
        if salt and flags & 1 and op == HS_EFFECT and data != 0xffffffff:
            index = data & 0xffff
            if index < len(halo.directory) and halo.directory[index].group == 'effe':
                found.add(halo.directory[index].name)
    return found


def light_values(halo, tag):
    t, a = halo.tags, tag.address
    radius, = t.read('f', a + LIGHT_RADIUS)
    alpha, r, g, b = t.read('4f', a + LIGHT_COLOR)
    duration, = t.read('f', a + LIGHT_DURATION)
    seconds = duration / 30 if duration > 5 else duration
    return max(0.0, radius), min(max(r, 0.0), 1.0), min(max(g, 0.0), 1.0), min(max(b, 0.0), 1.0), max(0.0, seconds)


def effect_record(halo, tag):
    t = halo.tags
    light, blast, strongest, systems, particles, names = None, 0.0, None, 0, 0, [tag.name.lower()]
    for event in t.block(tag.address + EFFECT_EVENTS, EVENT_STRIDE):
        for part in t.block(event + EVENT_PARTS, PART_STRIDE):
            found = halo.reference(t, part + PART_TYPE)
            if not found:
                continue
            if found.group == 'ligh' and light is None:
                light = light_values(halo, found)
            elif found.group == 'jpt!':
                # The blast's own damage (the largest), not the shock wave that pushes objects.
                d = jpt_damage(halo, found)
                if strongest is None or (d.upper_max, d.outer) > strongest:
                    strongest, blast = (d.upper_max, d.outer), d.outer
            elif found.group == 'pctl':
                systems += 1
                names.append(found.name.lower())
        particles += len(t.block(event + EVENT_PARTICLES, PARTICLE_STRIDE))
    name = tag.name.lower()
    # Size for the engine's fireballs: the tags' particle systems are named large, med and small
    # (effects\particle systems\explosion large); a grenade's is small.
    words = ' '.join(names)
    scale = 1.6 if 'large' in words else .6 if 'small' in words or 'grenade' in name else 1.0
    if 'explosion' in name or blast > 0:
        kind = 'explosion'
    elif 'smoke' in name or 'smolder' in name or 'steam' in name or 'debris' in name:
        kind = 'smoke'
    elif 'burst' in name or 'spark' in name or 'splash' in name:
        kind = 'burst'
    else:
        kind = 'glow'
    return {'path': tag.name, 'kind': kind, 'scale': scale, 'light': light, 'blast': blast, 'systems': systems,
            'particles': particles, 'sound': effect_sound(halo, tag)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    effects, flashlight, classes = {}, None, []
    for cache in CAMPAIGN:
        source = args.maps / f'{cache}.map'
        if not source.is_file():
            raise SystemExit(f'missing {source}')
        with source.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            wanted = script_effects(halo)
            for tag in halo.directory:
                if tag.group == 'effe' and tag.name in wanted and tag.name not in effects:
                    effects[tag.name] = effect_record(halo, tag)
                elif tag.group == 'ligh' and tag.name == FLASHLIGHT and flashlight is None:
                    radius, r, g, b, _ = light_values(halo, tag)
                    falloff, cutoff = halo.tags.read('2f', tag.address + LIGHT_FALLOFF)
                    flashlight = (radius, falloff, cutoff, r, g, b)
            pairs = [f'{index}:{halo.tags.read("h", tag.address + SOUND_CLASS)[0]}'
                     for index, tag in enumerate(halo.directory) if tag.group == 'snd!']
            classes.append(f'classes {cache} ' + ' '.join(pairs))
    lines = [f'format {FORMAT}', f'# {len(effects)} script effects from the Xbox campaign caches', FIELDS.rstrip()]
    if flashlight:
        lines.append('flashlight ' + ' '.join(map(number, flashlight)))
    for index, name in enumerate(sorted(effects)):
        e = effects[name]
        lines += [f'effect {index}', f'path {name}', f'kind {e["kind"]} {number(e["scale"])}']
        if e['light']:
            lines.append('light ' + ' '.join(map(number, e['light'])))
        if e['blast'] > 0:
            lines.append(f'blast {number(e["blast"])}')
        lines.append(f'particles {e["systems"]} {e["particles"]}')
        if e['sound']:
            lines.append(f'sound play {e["sound"]}')
        lines.append('end')
    lines += classes
    target = args.output / 'packages/halo/effects.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('\n'.join(lines) + '\n', encoding='ascii')
    print(f'{len(effects)} script effects, flashlight {"found" if flashlight else "missing"}, '
          f'sound classes for {len(classes)} caches -> {target}')


if __name__ == '__main__':
    main()
