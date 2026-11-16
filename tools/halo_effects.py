#!/usr/bin/env python3
"""Export the retail effects the mission scripts create, the flashlight and the sound classes.

packages/halo/effects.txt (git-ignored like the rest of packages/) is read by
source/src/halo_fx.cpp:

- every `effe` tag a mission script names (the effect constants of effect_new,
  effect_new_on_object_marker and the d40 effect globals), and the vehicles' body depleted
  effects (halo_vehicles.vehicle_health: the Wraith's, Ghost's and Banshee's death explosion) and the
  actor units' body destroyed effects (halo_actors.destroyed_effect: the carrier's blast, the
  Sentinels' and the Engineer's death explosions, set off where such a unit is destroyed, halo.cpp's
  halokilled): its first sound, its first light
  (radius in world units, colour, duration), the largest area radius of its `jpt!` parts and
  how many particle systems and particles it carries. The engine draws an effect with
  AssaultCube's own particles (fireballs, sparks, smoke) and a flash sprite sized from these;
  the tags' particle systems, decals and debris are not converted. The strongest damaging `jpt!`
  part is written as the optional `damage` and `materials` lines (batch 5's critic, gap 11): the
  engine deals it for a vehicle's death explosion and, unless halo_effectdamage 0, for the
  scripts' effect_new (haloeffectblast in source/src/weapon.cpp; tools/halo_effect_reach.py lists
  where the scripts set them off against the planned routes). The format stays 1: older
  games skip the new lines.
- every `jpt!` a mission script names (the damage constants of damage_object and damage_new: a10's
  shield-tutorial zapper, a30's shock explosion, c40's pulse generator, d40's oven heat and the
  guaranteed kills), as optional `jpt` records after the effects: radii, damage, side effect,
  category, AOE core, damage flags (0x40 only hurts shields), the effect's own flags and the 33
  material modifiers (and, after them, an optional `jptkind <category> <acceleration>` line, as in
  weapons.txt, for the unit dialogue's death lines). The engine deals them as Halo's hs_damage_object and hs_damage_new do
  (source/src/halo_script_ai.inc); older games skip the records and keep the flat 40 and the
  name-based kills.
- the player's flashlight (`characters\\cyborg\\flashlight_cyborg`): radius and cone angles.
- the sound class of every `snd!` tag in each cache (the `classes` lines), which
  sound_class_set_gain fades.

tools/import_halo_audio.py decodes the effects' sounds (the `sound play` lines).
"""
import argparse
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap
from halo_weapons import CAMPAIGN, EFFECT_EVENTS, EVENT_STRIDE, EVENT_PARTS, JPT_FLAGS, PART_STRIDE, PART_TYPE, \
    effect_sound, jpt_damage, jpt_kind, jpt_materials, kind_line, materials_line, number

FORMAT = 1
EVENT_PARTICLES, PARTICLE_STRIDE = 56, 232     # an effect event's particles block
# ligh: radius at 4, the cone's falloff and cutoff angles at 16 and 20 (radians), the colour's
# upper bound (ARGB floats) at 72 and the duration at 244 (ticks when above a few, else seconds).
LIGHT_RADIUS, LIGHT_FALLOFF, LIGHT_CUTOFF, LIGHT_COLOR, LIGHT_DURATION = 4, 16, 20, 72, 244
SOUND_CLASS = 4                                # snd!: the class (16-bit) before the sample rate
HS_EFFECT = 25                                 # HaloScript value type of an effect constant
HS_DAMAGE = 26                                 # ... and of a damage (jpt!) constant (../Halo3DS hs/hs.h:40-41)
FLASHLIGHT = r'characters\cyborg\flashlight_cyborg'

FIELDS = '''# flashlight <radius> <falloff angle> <cutoff angle> <r> <g> <b>   world units, radians
# effect <index>
#   path <effe tag>
#   kind explosion|burst|smoke|glow <scale>   how the engine draws it with its own particles, and how
#                                         big (1.6 the tags' large particle systems, 0.6 small ones)
#   light <radius> <r> <g> <b> <seconds>  the first light part
#   blast <radius>                        the largest jpt! area radius (the drawn blast's size)
#   damage <inner> <outer> <lower> <upper min> <upper max> <side effect> <category> <core>
#                                         the strongest damaging jpt! part (world units, points)
#   materials <33 modifiers>              that part's damage modifier per retail material type
#   particles <systems> <particles>       particle system and particle parts it carries
#   sound play <snd! tag>                 the first sound part
#   end
# jpt <index>                            a jpt! a mission script names (damage_object, damage_new)
#   path <jpt! tag>
#   jptdamage <inner> <outer> <lower> <upper min> <upper max> <side effect> <category> <core> <damage flags> <effect flags>
#                                         world units, points; damage flags 0x40 only hurts shields,
#                                         0x200 skips shields; effect flag 0x1 don't scale by distance
#   materials <33 modifiers>              its damage modifier per retail material type
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


def script_damage(halo):
    """jpt! tag names named by the scenario's script constants (op 26 nodes: the damage argument of
    damage_object and damage_new)."""
    from import_halo_scripts import Program
    try:
        program = Program(halo)
    except ValueError:
        return set()
    found = set()
    for salt, op, ty, flags, _, _, data in program.nodes:
        if salt and flags & 1 and op == HS_DAMAGE and data != 0xffffffff:
            index = data & 0xffff
            if index < len(halo.directory) and halo.directory[index].group == 'jpt!':
                found.add(halo.directory[index].name)
    return found


def jpt_record(halo, tag):
    d = jpt_damage(halo, tag)
    effect_flags, = halo.tags.read('I', tag.address + JPT_FLAGS)
    return {'damage': d, 'effect_flags': effect_flags, 'materials': jpt_materials(halo, tag), 'kind': jpt_kind(halo, tag)}


def light_values(halo, tag):
    t, a = halo.tags, tag.address
    radius, = t.read('f', a + LIGHT_RADIUS)
    alpha, r, g, b = t.read('4f', a + LIGHT_COLOR)
    duration, = t.read('f', a + LIGHT_DURATION)
    seconds = duration / 30 if duration > 5 else duration
    return max(0.0, radius), min(max(r, 0.0), 1.0), min(max(g, 0.0), 1.0), min(max(b, 0.0), 1.0), max(0.0, seconds)


def vehicle_effects(halo):
    """The body depleted effects of the vehicles the engine drives (tools/halo_vehicles.py
    `destroyed` lines): halo_vehicle.cpp plays one where a vehicle is destroyed."""
    from halo_vehicles import VEHICLES, vehicle_health
    paths = {tag for _, tag, _, _, _ in VEHICLES}
    found = set()
    for tag in halo.directory:
        if tag.group == 'vehi' and tag.name in paths:
            effect = vehicle_health(halo, tag)[4]
            if effect:
                found.add(effect)
    return found


def unit_effects(halo):
    """The body destroyed effects of the bipeds destroyed after dying (tools/halo_actors.py's
    `destroyed` lines): halo.cpp sets one off where such an actor dies and deals its damage."""
    from halo_actors import destroyed_effect
    found = set()
    for tag in halo.directory:
        if tag.group == 'bipd':
            effect = destroyed_effect(halo, tag)
            if effect:
                found.add(effect)
    return found


def effect_record(halo, tag):
    t = halo.tags
    light, blast, strongest, systems, particles, names = None, 0.0, None, 0, 0, [tag.name.lower()]
    damage, materials, damage_kind = None, None, None
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
                    # Its damage, when it deals any (a shock wave only pushes).
                    if d.upper_max > 0 and d.outer > 0:
                        damage, materials, damage_kind = d, jpt_materials(halo, found), jpt_kind(halo, found)
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
            'particles': particles, 'sound': effect_sound(halo, tag), 'damage': damage, 'materials': materials,
            'jptkind': damage_kind}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    effects, jpts, flashlight, classes = {}, {}, None, []
    for cache in CAMPAIGN:
        source = args.maps / f'{cache}.map'
        if not source.is_file():
            raise SystemExit(f'missing {source}')
        with source.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            wanted = script_effects(halo) | vehicle_effects(halo) | unit_effects(halo)
            damage = script_damage(halo)
            for tag in halo.directory:
                if tag.group == 'effe' and tag.name in wanted and tag.name not in effects:
                    effects[tag.name] = effect_record(halo, tag)
                elif tag.group == 'jpt!' and tag.name in damage and tag.name not in jpts:
                    jpts[tag.name] = jpt_record(halo, tag)
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
        d = e['damage']
        if d:
            lines.append('damage ' + ' '.join(map(number, (d.inner, d.outer, d.lower, d.upper_min, d.upper_max))) +
                         f' {d.side_effect} {d.category} {number(d.core)}')
            if e['materials']:
                lines.append(materials_line(e['materials']))
            if e.get('jptkind'):
                lines.append(kind_line(e['jptkind']))
        lines.append(f'particles {e["systems"]} {e["particles"]}')
        if e['sound']:
            lines.append(f'sound play {e["sound"]}')
        lines.append('end')
    # After every effect's `end`, so an older game, with no effect open, skips each line.
    for index, name in enumerate(sorted(jpts)):
        j = jpts[name]
        d = j['damage']
        lines += [f'jpt {index}', f'path {name}',
                  'jptdamage ' + ' '.join(map(number, (d.inner, d.outer, d.lower, d.upper_min, d.upper_max))) +
                  f' {d.side_effect} {d.category} {number(d.core)} {d.flags} {j["effect_flags"]}']
        if j['materials']:
            lines.append(materials_line(j['materials']))
        if j.get('kind'):
            lines.append(kind_line(j['kind']))
        lines.append('end')
    lines += classes
    target = args.output / 'packages/halo/effects.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('\n'.join(lines) + '\n', encoding='ascii')
    print(f'{len(effects)} script effects, {len(jpts)} script jpt!s, flashlight {"found" if flashlight else "missing"}, '
          f'sound classes for {len(classes)} caches -> {target}')


if __name__ == '__main__':
    main()
