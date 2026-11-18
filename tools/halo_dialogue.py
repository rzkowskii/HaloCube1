#!/usr/bin/env python3
"""Export the retail unit dialogue (udlg) the engine's actors and the player speak with.

Every unit an actor variant (actv) of the ten Xbox campaign caches uses, and the globals' player
unit (characters\\cyborg\\cyborg), carries a dialogue_variants block (../Halo3DS
units/unit_definitions.h:64-73 and :169: unit +692, 24-byte elements, the variant number a short at
+0 and the udlg tag reference at +8, its index at +0x14). Each udlg holds 209 vocalization sound
references (units/unit_dialogue.c dialogue_definition: a short version, pad and 12 unused bytes,
then 16-byte tag references from +16; the type names are dialogue_definitions.c's
dialogue_vocalization_type_name). This tool writes packages/halo/dialogue.txt (FIELDS below) with
the vocalizations the engine speaks (VOCALIZATIONS and what Halo's lookup falls back to) and, per
sound, its permutation count, longest permutation (ms, sound_definition +132, which unit_speak
times a line by, unit_dialogue.c:353-356) and the permutations' skip fractions (the permutation
+32; sound_definitions.c:227-262 skips a permutation at that chance). tools/import_halo_audio.py
then decodes every permutation of those sounds to packages/audio/halo/dialogue/<sound>_<n>.wav and
source/src/halo_dialogue.cpp plays them. The file is optional: without it the engine keeps
AssaultCube's stock pain and death sounds. All of it is generated from the caches and git-ignored.
"""
import argparse
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap
from halo_weapons import CAMPAIGN
from halo_actors import GLOBALS_PLAYER_INFORMATION, PLAYER_INFORMATION_STRIDE, UNIT_REFERENCE

FORMAT = 1
UNIT_DIALOGUE_VARIANTS, VARIANT_STRIDE, VARIANT_DIALOGUE = 692, 24, 8
UDLG_VOCALIZATIONS, VOCALIZATION_TYPES = 16, 209
SOUND_LONGEST, SOUND_PITCH_RANGES, PITCH_RANGE_STRIDE = 132, 152, 72
PITCH_RANGE_ACTUAL_COUNT, PITCH_RANGE_PERMUTATIONS, PERMUTATION_STRIDE, PERMUTATION_SKIP = 44, 60, 124, 32

# dialogue_vocalization_lookup (../Halo3DS units/dialogue_definitions.c:830-850): the type
# unit_test_speech falls back to when a udlg has no sound for one (unit_dialogue.c:140-160).
LOOKUP = (
    1, -1, -1, -1, -1, -1, -1, 6, 6, 6, -1, 10, -1, 12, 15, -1, 14, 15, 14, 15, -1, -1, 21, -1,
    -1, -1, -1, -1, -1, -1, -1, 29, -1, -1, -1, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, -1, -1,
    -1, -1, -1, 49, -1, -1, -1, 53, 54, 53, 54, 53, 54, 53, 54, 53, 54, 53, 53, 53, 53, 53, 53, 53,
    53, 53, 53, 53, -1, -1, -1, -1, -1, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 93, 94, 95,
    -1, 96, 96, 98, 96, 100, 100, 100, 100, -1, -1, -1, 108, 109, 108, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 53, 112, 154, 191,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0)
# The port's table points 93, 94, 95, 108, 109 and 191 at themselves, which would never end
# unit_test_speech's loop for a udlg without them; the engine (and the closure below) take a
# self-reference as no fallback, the conservative reading (docs/macos-build.md, Unit dialogue).
assert len(LOOKUP) == VOCALIZATION_TYPES

# The vocalizations the engine speaks (source/src/halo_dialogue.cpp), by Halo's type index
# (dialogue_definitions.c:617-828): unit_make_damage_sound's pain and deaths (unit_dialogue.c:
# 725-886: 6 pain body minor, 7 major, 8 pain shield, 9 pain falling, 14-19 death quiet, violent,
# falling, agonizing, instant, flying), unit_scream's (887-966: 10 scream fear, 11 scream pain, 39
# hurt enemy grenade) and the ai_communication events' (ai/ai_communication.c global_dialogue_table,
# 1215-1484): the death event's killer and friend lines (49, 51, 53, 55, 57, 59, 61, 63, 65-75,
# 96-105), damage (21, 22, 29, 31), hurt (23, 32, 35-38, 40-45), sighted enemy (108, 109), grenade
# throwing (115), grenade sighted (117), grenade danger (118, 119, 120), shooting, shooting vehicle,
# berserk, group and traitor (149-153), flee (156), flee leader died (158), berserk (178), melee
# (179), dive (180) and leap (182). Their lookups are added (fallbacks).
SPOKEN = (6, 7, 8, 9, 10, 11, 14, 15, 16, 17, 18, 19, 21, 22, 23, 29, 31, 32, 35, 36, 37, 38, *range(39, 46),
          49, 51, 53, 55, 57, 59, 61, 63, *range(65, 76), *range(96, 106), 108, 109, 115, 117, 118, 119, 120,
          *range(149, 154), 156, 158, 178, 179, 180, 182)


def vocalizations():
    wanted = set()
    for v in SPOKEN:
        while v >= 0 and v not in wanted:
            wanted.add(v)
            v = LOOKUP[v] if LOOKUP[v] != v else -1
    return sorted(wanted)


FIELDS = '''# sound <id> <permutations> <longest ms> <snd! tag>   a dialogue sound; its permutations decode to
#                                                    packages/audio/halo/dialogue/<sample>_<n>.wav
# skip <id> <skip fraction per permutation>          (sound_definitions.c:227-262)
# udlg <id> <udlg tag>                               a dialogue, then its lines up to `end`:
# voc <vocalization type> <sound id>                 (dialogue_definitions.c:617-828 names the types)
# unit <id> <unit tag>                               a unit, then up to `end`:
# variant <variant number> <udlg id>                 its dialogue_variants (unit_definitions.h:64-73)
# actor <unit id> <actv tag>                         an actor variant's unit
# player <unit id>                                   the globals' player unit'''


def sound_record(halo, tag):
    """(permutation count, longest ms, skip fractions) of a snd! tag, or None."""
    t = halo.tags
    try:
        longest, = t.read('i', tag.address + SOUND_LONGEST)
        ranges = t.block(tag.address + SOUND_PITCH_RANGES, PITCH_RANGE_STRIDE)
        if not ranges:
            return None
        actual, = t.read('h', ranges[0] + PITCH_RANGE_ACTUAL_COUNT)
        permutations = t.block(ranges[0] + PITCH_RANGE_PERMUTATIONS, PERMUTATION_STRIDE)
        count = max(1, min(actual, len(permutations))) if permutations else 0
        if not count:
            return None
        skips = [t.read('f', permutations[i] + PERMUTATION_SKIP)[0] for i in range(count)]
    except ValueError:
        return None
    return count, max(0, longest), skips


def dialogue_variants(halo, unit):
    t = halo.tags
    found = []
    try:
        for element in t.block(unit.address + UNIT_DIALOGUE_VARIANTS, VARIANT_STRIDE):
            number, = t.read('h', element)
            udlg = halo.reference(t, element + VARIANT_DIALOGUE)
            if udlg and udlg.group == 'udlg':
                found.append((number, udlg))
    except ValueError:
        pass
    return found


def number(value):
    text = f'{value:.6g}'
    return '0' if text == '-0' else text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    wanted = vocalizations()
    units, actors, udlgs, sounds = {}, {}, {}, {}
    player = None
    for cache in CAMPAIGN:
        path = args.maps / f'{cache}.map'
        if not path.exists():
            continue
        with path.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            t = halo.tags
            bodies = []
            for tag in halo.directory:
                if tag.group == 'actv':
                    unit = halo.reference(t, tag.address + UNIT_REFERENCE)
                    if unit and unit.group in ('bipd', 'vehi'):
                        actors.setdefault(tag.name, unit.name)
                        bodies.append(unit)
            matg = next((tag for tag in halo.directory if tag.group == 'matg'), None)
            if matg:
                elements = t.block(matg.address + GLOBALS_PLAYER_INFORMATION, PLAYER_INFORMATION_STRIDE)
                unit = halo.reference(t, elements[0]) if elements else None
                if unit and unit.group == 'bipd':
                    player = player or unit.name
                    bodies.append(unit)
            for unit in bodies:
                if unit.name not in units:
                    units[unit.name] = [(n, u.name) for n, u in dialogue_variants(halo, unit)]
                for _, udlg in dialogue_variants(halo, unit):
                    lines = udlgs.setdefault(udlg.name, {})
                    for v in wanted:
                        if v in lines:
                            continue
                        sound = halo.reference(t, udlg.address + UDLG_VOCALIZATIONS + 16 * v)
                        if not sound or sound.group != 'snd!':
                            continue
                        record = sounds.get(sound.name) or sound_record(halo, sound)
                        if record:
                            sounds[sound.name] = record
                            lines[v] = sound.name
    sound_ids = {name: i for i, name in enumerate(sorted(sounds))}
    udlg_ids = {name: i for i, name in enumerate(sorted(udlgs))}
    unit_ids = {name: i for i, name in enumerate(sorted(n for n, v in units.items() if v))}
    out = [f'format {FORMAT}', f'# {len(udlgs)} dialogues, {len(sounds)} sounds of {len(wanted)} vocalization types '
           'from the Xbox campaign caches (tools/halo_dialogue.py)', FIELDS]
    for name, i in sound_ids.items():
        count, longest, skips = sounds[name]
        out += [f'sound {i} {count} {longest} {name}', f'skip {i} ' + ' '.join(map(number, skips))]
    for name, i in udlg_ids.items():
        out.append(f'udlg {i} {name}')
        out += [f'voc {v} {sound_ids[s]}' for v, s in sorted(udlgs[name].items())]
        out.append('end')
    for name, i in unit_ids.items():
        out.append(f'unit {i} {name}')
        out += [f'variant {n} {udlg_ids[u]}' for n, u in units[name]]
        out.append('end')
    out += [f'actor {unit_ids[u]} {a}' for a, u in sorted(actors.items()) if u in unit_ids]
    if player in unit_ids:
        out.append(f'player {unit_ids[player]}')
    target = args.output / 'packages/halo/dialogue.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('\n'.join(out) + '\n')
    permutations = sum(s[0] for s in sounds.values())
    print(f'{len(unit_ids)} units with dialogue, {len(udlgs)} udlg, {len(sounds)} sounds '
          f'({permutations} permutations), player {player or "none"} -> {target}')


if __name__ == '__main__':
    main()
