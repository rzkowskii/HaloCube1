#!/usr/bin/env python3
"""Export the retail actor (actr) and actor variant (actv) behaviour tables.

Every `actv` tag in the ten Xbox campaign caches becomes one record in
packages/halo/actors.txt with the perception, firing, burst, grenade, defence,
morale and vitality values its actr and actv tags carry. Distances are in Halo
world units, times in seconds and angles in radians. source/src/halo_ai.cpp
reads it; tools/import_halo_scripts.py stores record indices per squad start.
"""
import argparse
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap
from halo_weapons import CAMPAIGN, weapon_index

FORMAT = 2                        # 2: team, unitmelee and material lines
# Actor families the engine has bodies for, matching ActorKind in halo.cpp (monitor and engineer
# fly and never fight).
KINDS = {'grunt': 0, 'elite': 1, 'jackal': 2, 'hunter': 3, 'floodcombat elite': 4,
         'floodcombat_human': 5, 'flood_infection': 6, 'floodcarrier': 7, 'sentinel': 8,
         'marine': 9, 'marine_armored': 9, 'crewman': 9, 'marine_suicidal': 9, 'captain': 9,
         'johnson': 9, 'bisenti': 9, 'mendoza': 9, 'fitzgerald': 9, 'jenkins': 9,
         'marine_wounded': 9, 'captain_ingame': 9, 'monitor': 10, 'engineer': 11}
# Inside the actv tag's unit (bipd): its default team (h: 0 default, 1 player, 2 human,
# 3 covenant, 4 flood, 5 sentinel, 6-9 unused), the collision model and the melee jpt!.
UNIT_REFERENCE = 20               # actv: the unit tag reference
UNIT_TEAM = 384                   # unit: default team
UNIT_COLLISION = 112              # object: coll reference
UNIT_MELEE = 648                  # unit: melee damage jpt! reference
# coll materials: name[32], flags u32 (1 head), material type h, pad, shield leak f,
# shield damage multiplier f at 44, body damage multiplier f at 60.
COLL_MATERIALS, MATERIAL_STRIDE = 564, 72

FIELDS = '''# actor <index>
# path <actv tag>
# family <folder> <kind> <actr type> <actr flags> <actv flags>
# weapon <weapons.txt index>|-1
# vision <distance> <central angle> <max angle> <peripheral angle> <peripheral distance> <hearing>
# fire <max range> <rate> <error> <first delay min> <max> <combat range min> <max> <lead> <damage scale>
# burst <duration min> <max> <separation min> <max>
# grenade <type> <chance> <range min> <max> <count min> <max>
# vitality <body> <shield>
# defense <hide shield fraction> <emerge to attack> <emerge to pursue>
# morale <casualty trigger> <friend killed chance> <leader killed chance>
# berserk <damage amount> <threshold> <melee range>
# positions <guard min> <max> <combat min> <max>
# team <unit default team>          retail numbering: 0 default, 1 player, 2 human, 3 covenant, 4 flood, 5 sentinel
# unitmelee <lower> <upper min> <upper max>   the unit's own melee jpt! (a Hunter's smash)
# material <type> <flags> <body multiplier> <shield multiplier>   each collision material (flags 1: head)
'''


def actor_record(halo, tag, weapons):
    t, a = halo.tags, tag.address
    actr = halo.reference(t, a + 4)
    weapon = halo.reference(t, a + 100)
    family = tag.name.split('\\')[1] if '\\' in tag.name else tag.name
    record = {'path': tag.name, 'family': family.replace(' ', '_'), 'kind': KINDS.get(family, -1),
              'weapon': weapons.get(weapon.name, -1) if weapon and weapon.group == 'weap' else -1,
              'actv_flags': t.read('I', a)[0]}
    record['fire'] = (*t.read('3f', a + 116), *t.read('2f', a + 128), *t.read('2f', a + 156),
                      *t.read('2f', a + 192))
    record['burst'] = (*t.read('2f', a + 224), *t.read('2f', a + 232))
    record['grenade'] = (t.read('h', a + 384)[0], t.read('f', a + 416)[0], *t.read('2f', a + 404),
                         *t.read('2h', a + 464))
    record['vitality'] = t.read('2f', a + 512)
    record['melee'] = t.read('f', a + 352)[0]
    record.update(unit_record(halo, halo.reference(t, a + UNIT_REFERENCE)))
    if actr and actr.group == 'actr':
        r = actr.address
        record['type'] = t.read('h', r + 20)[0]
        record['actr_flags'] = t.read('I', r)[0]
        record['vision'] = (*t.read('3f', r + 24), *t.read('2f', r + 40), t.read('f', r + 76)[0])
        record['defense'] = t.read('3f', r + 732)
        record['morale'] = (t.read('h', r + 632)[0], t.read('f', r + 672)[0], t.read('f', r + 680)[0])
        record['berserk'] = (*t.read('2f', r + 920), record['melee'])
        record['positions'] = t.read('4f', r + 952)
    else:
        record.update(type=-1, actr_flags=0, vision=(0,) * 6, defense=(0,) * 3, morale=(0, 0.0, 0.0),
                      berserk=(0.0, 0.0, record['melee']), positions=(0,) * 4)
    return record


def unit_record(halo, unit):
    """The unit's default team, melee jpt! damage and collision materials."""
    from halo_weapons import jpt_damage
    t = halo.tags
    record = {'team': -1, 'unitmelee': (0.0, 0.0, 0.0), 'materials': []}
    if not unit or unit.group not in ('bipd', 'vehi'):
        return record
    record['team'], = t.read('h', unit.address + UNIT_TEAM)
    melee = halo.reference(t, unit.address + UNIT_MELEE)
    if melee and melee.group == 'jpt!':
        d = jpt_damage(halo, melee)
        record['unitmelee'] = (d.lower, d.upper_min, d.upper_max)
    coll = halo.reference(t, unit.address + UNIT_COLLISION)
    if coll and coll.group == 'coll':
        for m in t.block(coll.address + COLL_MATERIALS, MATERIAL_STRIDE):
            flags, = t.read('I', m + 32)
            kind, = t.read('h', m + 36)
            shield, = t.read('f', m + 44)
            body, = t.read('f', m + 60)
            record['materials'].append((kind, flags, body, shield))
    return record


def number(value):
    if isinstance(value, int):
        return str(value)
    text = f'{value:.6g}'
    return '0' if text == '-0' else text


def write(records, path):
    lines = [f'format {FORMAT}', f'# {len(records)} actor variants from the Xbox campaign caches', FIELDS.rstrip()]
    for index, r in enumerate(records):
        lines += [f'actor {index}', f'path {r["path"]}',
                  f'family {r["family"]} {r["kind"]} {r["type"]} {r["actr_flags"]} {r["actv_flags"]}',
                  f'weapon {r["weapon"]}']
        for key in ('vision', 'fire', 'burst', 'grenade', 'vitality', 'defense', 'morale', 'berserk', 'positions'):
            lines.append(key + ' ' + ' '.join(map(number, r[key])))
        lines.append(f'team {r.get("team", -1)}')
        lines.append('unitmelee ' + ' '.join(map(number, r.get('unitmelee', (0.0, 0.0, 0.0)))))
        lines += ['material ' + ' '.join(map(number, m)) for m in r.get('materials', [])]
        lines.append('end')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n')


def actor_index(root):
    """actv tag path to record index, read back from an existing actors.txt."""
    index, current = {}, None
    path = Path(root) / 'packages/halo/actors.txt'
    if not path.exists():
        return index
    for line in path.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'actor':
            current = int(rest)
        elif key == 'path' and current is not None:
            index[rest] = current
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    weapons = weapon_index(args.output)
    if not weapons:
        raise SystemExit('run tools/halo_weapons.py first to write packages/halo/weapons.txt')
    records = {}
    for cache in CAMPAIGN:
        with (args.maps / f'{cache}.map').open('rb') as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group == 'actv' and tag.name not in records:
                    records[tag.name] = actor_record(halo, tag, weapons)
    ordered = [records[name] for name in sorted(records)]
    target = args.output / 'packages/halo/actors.txt'
    write(ordered, target)
    bodies = sum(r['kind'] >= 0 for r in ordered)
    print(f'{len(ordered)} actor variants ({bodies} with engine bodies) -> {target}')


if __name__ == '__main__':
    main()
