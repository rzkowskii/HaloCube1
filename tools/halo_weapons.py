#!/usr/bin/env python3
"""Export the retail campaign weapon, projectile and damage definitions.

Every `weap` tag in the ten Xbox campaign caches becomes one record in
packages/halo/weapons.txt. Each trigger follows its projectile to the impact
damage, or to the first damage effect its detonation effect carries. Speeds are
converted from world units per 30 Hz tick to world units per second; angles
stay in radians and times in seconds. source/src/halo_weapons.cpp reads it.
"""
import argparse
import json
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap

CAMPAIGN = ('a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40')
TICKS = 30
FORMAT = 1

# Byte offsets inside the tags. Objects start with a 380-byte object section;
# items add 396 more, so weapon fields begin at 776.
OBJECT, ITEM = 380, 396
WEAPON = OBJECT + ITEM
MAGAZINE_STRIDE, TRIGGER_STRIDE = 112, 276
DAMAGE = 452                      # damage section inside a jpt! tag
ACTOR_WEAPON = 100                # carried weapon reference inside an actv tag

# Weapons the player can hold, with their AssaultCube firing behaviour. The
# remaining weapon tags are vehicle, turret or actor-only guns.
PLAYER = {
    r'weapons\assault rifle\assault rifle': 'assault',
    r'weapons\pistol\pistol': 'pistol',
    r'weapons\shotgun\shotgun': 'shotgun',
    r'weapons\sniper rifle\sniper rifle': 'sniper',
    r'weapons\rocket launcher\rocket launcher': 'projectile',
    r'weapons\plasma pistol\plasma pistol': 'projectile',
    r'weapons\plasma rifle\plasma rifle': 'projectile',
    r'weapons\needler\needler': 'projectile',
    r'weapons\flamethrower\flamethrower': 'projectile',
}
# Models the game already packages; the library supplies the others.
PACKAGED_VIEWMODELS = {r'weapons\assault rifle\assault rifle': 'halo/ma5b', r'weapons\pistol\pistol': 'halo/pistol'}

WEAPON_FIELDS = '''# path <tag>                 retail weapon tag
# label <text>                weapon label, class assault|pistol|shotgun|sniper|projectile|vehicle|actor
# viewmodel <model>|-          first-person model under packages/models
# worldmodel <model>|-         third-person pickup model under packages/models
# ready <s>
# heat <recover> <overheat> <loss/s>
# zoom <levels> <min> <max>   magnification
# aim <autoaim angle> <autoaim range> <magnetism angle> <magnetism range>
# melee <lower> <upper>
# magazine <loaded> <initial total> <max total> <reload s> <rounds reloaded> <recharged/s>
# trigger <rate initial> <rate final> <rate accel s> <rate decel s> <rounds/shot> <projectiles/shot>
#         <distribution angle> <error initial> <error final> <error accel s> <error decel s>
#         <error angle min> <error angle max> <charge s> <heat/round> <age/round> <magazine>
# projectile <speed initial> <speed final> <range> <gravity> <guided rad/s> <damage lower> <damage upper>
#            <category> <area radius> <path>
'''


def damage_values(halo, tag):
    """Lower and upper damage, category and area radius of a jpt! tag."""
    t, a = halo.tags, tag.address
    category, = t.read('h', a + DAMAGE + 2)
    lower, = t.read('f', a + DAMAGE + 16)
    upper, = t.read('f', a + DAMAGE + 24)
    radius, = t.read('f', a + 4)
    return lower, max(lower, upper), category, radius


def referenced(halo, tag, group, limit=2048):
    """First tag reference of a group inside a tag body, for effect payloads."""
    t = halo.tags
    for offset in range(0, limit, 4):
        try:
            fourcc, = t.read('4s', tag.address + offset)
        except Exception:
            return None
        if fourcc[::-1] == group.encode():
            try:
                found = halo.reference(t, tag.address + offset, group)
            except ValueError:
                continue
            if found:
                return found
    return None


def projectile_record(halo, proj):
    t, a = halo.tags, proj.address + OBJECT
    initial, final, guided = t.read('3f', a + 104)
    maximum, gravity = t.read('2f', a + 76)
    damage = None
    for offset in (168, 152):     # impact damage, then attached detonation damage
        tag = halo.reference(t, a + offset)
        if tag and tag.group == 'jpt!':
            values = damage_values(halo, tag)
            # Needles carry an empty impact tag and deal their damage on detonation.
            if damage is None or values[1] > damage[1]:
                damage = values
    if damage is None:
        effect = halo.reference(t, a + 48)
        tag = referenced(halo, effect, 'jpt!') if effect and effect.group == 'effe' else None
        damage = damage_values(halo, tag) if tag else (0.0, 0.0, -1, 0.0)
    lower, upper, category, radius = damage
    return (initial * TICKS, final * TICKS, maximum, gravity, guided * TICKS,
            lower, upper, category, radius, proj.name)


def weapon_record(halo, tag, viewmodels, models):
    t, a = halo.tags, tag.address + WEAPON
    model = halo.reference(t, tag.address + 40)     # the object's third-person model
    melee = halo.reference(t, a + 140)
    melee = damage_values(halo, melee)[:2] if melee and melee.group == 'jpt!' else (0.0, 0.0)
    record = {
        'path': tag.name,
        'label': t.name(a + 4, 32) or '-',
        'class': PLAYER.get(tag.name, 'vehicle' if tag.name.startswith('vehicles') else 'actor'),
        'viewmodel': PACKAGED_VIEWMODELS.get(tag.name) or viewmodels.get(tag.name, '-'),
        'worldmodel': models.get(model.name, '-') if model else '-',
        'ready': t.read('f', a + 48),
        'heat': (t.read('f', a + 68)[0], t.read('f', a + 72)[0], t.read('f', a + 84)[0]),
        'zoom': (t.read('h', a + 210)[0], *t.read('2f', a + 212)),
        'aim': t.read('4f', a + 220),
        'melee': melee,
        'magazines': [], 'triggers': [],
    }
    for magazine in t.block(a + 488, MAGAZINE_STRIDE):
        recharge, initial, maximum, loaded = t.read('4h', magazine + 4)
        reload, = t.read('f', magazine + 20)
        reloaded, = t.read('h', magazine + 24)
        record['magazines'].append((loaded, initial, maximum, reload, reloaded, recharge))
    for trigger in t.block(a + 500, TRIGGER_STRIDE):
        rate = t.read('4f', trigger + 4)
        magazine, rounds = t.read('2h', trigger + 32)
        error = t.read('4f', trigger + 48)
        charge, = t.read('f', trigger + 72)
        projectiles, = t.read('h', trigger + 110)
        distribution, = t.read('f', trigger + 112)
        error_angle = t.read('2f', trigger + 124)
        heat, age = t.read('2f', trigger + 184)
        proj = halo.reference(t, trigger + 148)
        record['triggers'].append({
            'values': (*rate, rounds, max(1, projectiles), distribution, *error, *error_angle,
                       charge, heat, age, magazine),
            'projectile': projectile_record(halo, proj) if proj and proj.group == 'proj' else None,
        })
    return record


def number(value):
    if isinstance(value, int):
        return str(value)
    text = f'{value:.6g}'
    return '0' if text == '-0' else text


def write(records, path):
    lines = [f'format {FORMAT}', f'# {len(records)} weapons from the Xbox campaign caches', WEAPON_FIELDS.rstrip()]
    for index, record in enumerate(records):
        lines += [f'weapon {index}', f'path {record["path"]}', f'label {record["label"]} {record["class"]}',
                  f'viewmodel {record["viewmodel"]}', f'worldmodel {record["worldmodel"]}', f'ready {number(record["ready"][0])}',
                  'heat ' + ' '.join(map(number, record['heat'])),
                  'zoom ' + ' '.join(map(number, record['zoom'])),
                  'aim ' + ' '.join(map(number, record['aim'])),
                  'melee ' + ' '.join(map(number, record['melee']))]
        lines += ['magazine ' + ' '.join(map(number, m)) for m in record['magazines']]
        for trigger in record['triggers']:
            lines.append('trigger ' + ' '.join(map(number, trigger['values'])))
            p = trigger['projectile']
            lines.append('projectile ' + (' '.join(map(number, p[:-1])) + ' ' + p[-1] if p else '-'))
        lines.append('end')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n')


def weapon_index(root):
    """Tag path to record index, read back from an existing weapons.txt."""
    index, current = {}, None
    path = Path(root) / 'packages/halo/weapons.txt'
    if not path.exists():
        return index
    for line in path.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'weapon':
            current = int(rest)
        elif key == 'path' and current is not None:
            index[rest] = current
    return index


def actor_weapon(halo, actv):
    """The weapon tag an actor variant carries, or None."""
    tag = halo.reference(halo.tags, actv.address + ACTOR_WEAPON)
    return tag.name if tag and tag.group == 'weap' else None


def library_models(root, group):
    """Library model paths under packages/models for catalog 'viewmodels' or 'models'."""
    catalog = Path(root) / 'build/retail-assets/catalog.json'
    if not catalog.exists():
        return {}
    rows = json.loads(catalog.read_text()).get(group, {})
    return {name: row['path'].removeprefix('models/') for name, row in rows.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    viewmodels = library_models(args.output, 'viewmodels')
    models = library_models(args.output, 'models')
    records = {}
    for cache in CAMPAIGN:
        with (args.maps / f'{cache}.map').open('rb') as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group == 'weap' and tag.name not in records:
                    records[tag.name] = weapon_record(halo, tag, viewmodels, models)
    ordered = [records[name] for name in sorted(records)]
    target = args.output / 'packages/halo/weapons.txt'
    write(ordered, target)
    playable = sum(r['class'] in PLAYER.values() for r in ordered)
    print(f'{len(ordered)} weapons ({playable} player weapons) -> {target}')


if __name__ == '__main__':
    main()
