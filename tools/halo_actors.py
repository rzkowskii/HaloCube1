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

FORMAT = 3                        # 2: team, unitmelee and material lines; 3: major and difficulty lines
# Format 3 also carries the optional unit, spawn, pop, destroyed, resistance and shieldtime lines and the
# material lines' fifth field, the shield leak, the berserk line's fourth (the actr melee delay) and the
# grenade line's seventh (the encounter grenade timeout; below). The format number stays 3:
# an engine that predates them skips keys it does not know and fields past a line's last (halo_ai.cpp
# parse), so a newer table still loads there, and an older table (without them) loads in a newer
# engine, which keeps its built-in carrier burst, has no infection pop or speed stagger, puts
# every shield over the whole body and recharges it on the Elite's 12 s stun and 10 s recharge, and
# scales its own melee and grenade waits.
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
# actv: the major upgrade actv reference (../Halo3DS source/ai/actor_definitions.h:408, after the
# flags, actor and unit references): the variant Halo places instead when a squad's major upgrade
# roll succeeds (encounters.c:3040, actors.c:2687).
MAJOR_REFERENCE = 36
# matg: difficulty_information (game_globals.h:246), one element of 35 values by 4 difficulties
# (Easy, Normal, Hard, Impossible) then 21 pad floats (game_globals.h:189-193); the value names
# follow the enum at game_globals.h:63-99.
GLOBALS_DIFFICULTY, DIFFICULTY_STRIDE = 284, 35 * 16 + 21 * 4
DIFFICULTY_VALUES = (
    'enemy_damage', 'enemy_vitality', 'enemy_shield', 'enemy_recharge', 'friend_damage',
    'friend_vitality', 'friend_shield', 'friend_recharge', 'infection_forms', 'unused9',
    'rate_of_fire', 'projectile_error', 'burst_error', 'new_target_delay', 'burst_separation',
    'target_tracking', 'target_leading', 'overcharge_chance', 'special_fire_delay',
    'guidance_vs_player', 'melee_delay_base', 'melee_delay_scale', 'unused22',
    'grenade_chance_scale', 'grenade_timer_scale', 'unused25', 'unused26', 'unused27',
    'major_upgrade', 'major_upgrade_1', 'major_upgrade_2', 'unused31', 'unused32', 'unused33',
    'unused34')
UNIT_FLAGS = 380                  # unit: flags (../Halo3DS units/unit_definitions.h:21-48; bit 19 is_inconsequential)
UNIT_TEAM = 384                   # unit: default team
UNIT_COLLISION = 112              # object: coll reference
UNIT_MELEE = 648                  # unit: melee damage jpt! reference
# unit: the actors a unit releases when it is destroyed (unit_definitions.h:152-155): the
# spawned_actor_variant actv reference, spawn_actor_lower_bound and _upper_bound (h, h) and
# spawn_throw_velocity (f, world units a second). Halo spawns random_range(lower, upper + 1) of them
# and throws them at velocity / 30 a tick (units.c:1666-1693, actors.c:2844).
UNIT_SPAWN_VARIANT, UNIT_SPAWN_BOUNDS, UNIT_SPAWN_VELOCITY = 588, 604, 608
# bipd: the biped section starts at +752 (its jump_velocity, +196 in _biped_definition,
# biped_definitions.h:53-87, is the +948 tools/import_halo_campaign.py reads); its flags follow the
# moving turning speed (bit 11 random_speed_increase, biped_definitions.h:25-37).
BIPED_FLAGS = 756
# coll: the body destroyed effect reference (after the localized, area, body damaged and body
# depleted effects); the infection forms' is the effe whose jpt! pops its neighbours.
COLL_BODY_DESTROYED_EFFECT = 188
JPT_INFECTION_FORM_POP = 1 << 12  # jpt! damage flag _damage_infection_form_pop_bit (objects/damage.c:207-209)
UNIT_DESTROYED_AFTER_DYING = 1 << 1  # unit flag _unit_is_destroyed_after_dying_bit (units/unit_definitions.h:24)
# coll materials: name[32], flags u32 (1 head), material type h, pad, shield leak f at 40,
# shield damage multiplier f at 44, body damage multiplier f at 60 (../Halo3DS
# objects/damage_resistances.h:21-41, which asserts the 0x48 size and the 0x28 and 0x3C offsets).
COLL_MATERIALS, MATERIAL_STRIDE = 564, 72
# coll damage_resistance (damage_resistances.h:43-87): the indirect damage material index (h) at 4,
# maximum body vitality at 8, maximum shield vitality at 204, the shield material type (h) at 210,
# the shield failure function (h) at 236, threshold and maximum failure at 240 and 244, the minimum
# shield stun damage, stun time and recharge time (seconds) at 264, 268 and 272 and the runtime
# shield recharge velocity (a fraction of the shield per tick) at 448.
COLL_INDIRECT_MATERIAL, COLL_BODY_VITALITY, COLL_SHIELD_VITALITY, COLL_SHIELD_MATERIAL = 4, 8, 204, 210
COLL_SHIELD_FAILURE, COLL_SHIELD_STUN, COLL_RECHARGE_VELOCITY = 236, 264, 448
# actr: berserk.melee_attack_delay_timer (../Halo3DS ai/actor_definitions.h:222), the first field of
# actor_berserk_properties at +888 (defensive at +720, 108 bytes; pursuit at +828, 60), whose ninth,
# damage_berserk_amount, is the +920 read below. actv: grenade_combat.encounter_grenade_timeout
# (:375) at +424 (grenade_combat at +384: four shorts, then six floats to the throw chance at +416,
# throw_grenade_delay at +420; items follow at +448). Both checked against the cache bytes (the
# padding around them is zero in every actv and actr of the ten caches). The optional last field of
# the `berserk` (only for a variant with an actr) and `grenade` lines; older engines read the rest.
ACTR_MELEE_DELAY, ACTV_GRENADE_TIMEOUT = 888, 424

FIELDS = '''# difficulty <value index> <name> <easy> <normal> <hard> <impossible>   the globals (matg) difficulty table, before the actors
# actor <index>
# path <actv tag>
# family <folder> <kind> <actr type> <actr flags> <actv flags>
# weapon <weapons.txt index>|-1
# vision <distance> <central angle> <max angle> <peripheral angle> <peripheral distance> <hearing>
# fire <max range> <rate> <error> <first delay min> <max> <combat range min> <max> <lead> <damage scale>
# burst <duration min> <max> <separation min> <max>
# grenade <type> <chance> <range min> <max> <count min> <max> <encounter timeout s>   older engines read the first six
# vitality <body> <shield>
# defense <hide shield fraction> <emerge to attack> <emerge to pursue>
# morale <casualty trigger> <friend killed chance> <leader killed chance>
# berserk <damage amount> <threshold> <melee range> <melee attack delay s>   the last only with an actr; older engines read three
# positions <guard min> <max> <combat min> <max>
# team <unit default team>          retail numbering: 0 default, 1 player, 2 human, 3 covenant, 4 flood, 5 sentinel
# unitmelee <lower> <upper min> <upper max>   the unit's own melee jpt! (a Hunter's smash)
# material <type> <flags> <body multiplier> <shield multiplier> <shield leak>   each collision material (flags 1: head); older engines read the first four
# resistance <indirect material> <coll body> <coll shield> <shield material type> <failure function> <failure threshold> <maximum failure>   the coll's (absent: none)
# shieldtime <minimum stun damage> <stun s> <recharge s> <runtime recharge velocity>   the coll's shield stun and recharge, velocity per tick (absent: none)
# major <actv tag>                  the variant a major upgrade places instead (absent: none)
# unit <unit flags> <biped flags>   the unit tag's flags (bit 19: inconsequential) and a biped's (bit 11: random speed increase; 0 for a vehicle)
# spawn <actor index> <lower> <upper> <throw velocity>   the actors the unit releases when destroyed (absent: none), velocity in world units a second
# pop <inner> <outer> <lower> <upper min> <upper max> <jpt flags>   the body destroyed effect's infection-form pop damage (absent: none)
# destroyed <effe tag>              the body destroyed effect a unit destroyed after dying sets off, when it is not the pop (absent: none; effects.txt holds its damage)
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
                         *t.read('2h', a + 464), t.read('f', a + ACTV_GRENADE_TIMEOUT)[0])
    record['vitality'] = t.read('2f', a + 512)
    major = halo.reference(t, a + MAJOR_REFERENCE)
    record['major'] = major.name if major and major.group == 'actv' else ''
    record['melee'] = t.read('f', a + 352)[0]
    record.update(unit_record(halo, halo.reference(t, a + UNIT_REFERENCE)))
    if actr and actr.group == 'actr':
        r = actr.address
        record['type'] = t.read('h', r + 20)[0]
        record['actr_flags'] = t.read('I', r)[0]
        record['vision'] = (*t.read('3f', r + 24), *t.read('2f', r + 40), t.read('f', r + 76)[0])
        record['defense'] = t.read('3f', r + 732)
        record['morale'] = (t.read('h', r + 632)[0], t.read('f', r + 672)[0], t.read('f', r + 680)[0])
        record['berserk'] = (*t.read('2f', r + 920), record['melee'], t.read('f', r + ACTR_MELEE_DELAY)[0])
        record['positions'] = t.read('4f', r + 952)
    else:
        record.update(type=-1, actr_flags=0, vision=(0,) * 6, defense=(0,) * 3, morale=(0, 0.0, 0.0),
                      berserk=(0.0, 0.0, record['melee']), positions=(0,) * 4)
    return record


def unit_record(halo, unit):
    """The unit's default team, flags, melee jpt! damage, collision materials, the actors it
    releases when destroyed and its infection-form pop."""
    from halo_weapons import jpt_damage, referenced
    t = halo.tags
    record = {'team': -1, 'unitmelee': (0.0, 0.0, 0.0), 'materials': []}
    if not unit or unit.group not in ('bipd', 'vehi'):
        return record
    record['team'], = t.read('h', unit.address + UNIT_TEAM)
    unit_flags, = t.read('I', unit.address + UNIT_FLAGS)
    biped_flags = t.read('I', unit.address + BIPED_FLAGS)[0] if unit.group == 'bipd' else 0
    record['unit'] = (unit_flags, biped_flags)
    spawned = halo.reference(t, unit.address + UNIT_SPAWN_VARIANT)
    if spawned and spawned.group == 'actv':
        lower, upper = t.read('2h', unit.address + UNIT_SPAWN_BOUNDS)
        velocity, = t.read('f', unit.address + UNIT_SPAWN_VELOCITY)
        record['spawn'] = (spawned.name, lower, upper, velocity)
    melee = halo.reference(t, unit.address + UNIT_MELEE)
    if melee and melee.group == 'jpt!':
        d = jpt_damage(halo, melee)
        record['unitmelee'] = (d.lower, d.upper_min, d.upper_max)
    coll = halo.reference(t, unit.address + UNIT_COLLISION)
    if coll and coll.group == 'coll':
        c = coll.address
        for m in t.block(c + COLL_MATERIALS, MATERIAL_STRIDE):
            flags, = t.read('I', m + 32)
            kind, = t.read('h', m + 36)
            leak, shield = t.read('2f', m + 40)
            body, = t.read('f', m + 60)
            record['materials'].append((kind, flags, body, shield, leak))
        # Halo's shield step (objects/damage.c:800-988): the hit's material leaks that fraction past
        # the shield (842-843), the failure curve lets more through below the threshold (844-857),
        # the coll's shield material type picks the jpt! modifier (898-899), and a blast lands on the
        # indirect material (1623-1630); the stun and recharge (978-982, 1925-1932) follow.
        record['resistance'] = (t.read('h', c + COLL_INDIRECT_MATERIAL)[0], t.read('f', c + COLL_BODY_VITALITY)[0],
                                t.read('f', c + COLL_SHIELD_VITALITY)[0], t.read('h', c + COLL_SHIELD_MATERIAL)[0],
                                t.read('h', c + COLL_SHIELD_FAILURE)[0], *t.read('2f', c + COLL_SHIELD_FAILURE + 4))
        record['shieldtime'] = (*t.read('3f', c + COLL_SHIELD_STUN), t.read('f', c + COLL_RECHARGE_VELOCITY)[0])
        # The infection forms' body destroyed effect (characters\flood_infection\body destroyed):
        # its jpt! carries the pop flag, which only hurts inconsequential units (damage.c:2343-2375).
        effect = halo.reference(t, coll.address + COLL_BODY_DESTROYED_EFFECT)
        pop = referenced(halo, effect, 'jpt!') if effect and effect.group == 'effe' else None
        if pop:
            d = jpt_damage(halo, pop)
            if d.flags & JPT_INFECTION_FORM_POP:
                record['pop'] = (d.inner, d.outer, d.lower, d.upper_min, d.upper_max, d.flags)
        destroyed = destroyed_effect(halo, unit)
        if destroyed:
            record['destroyed'] = destroyed
    return record


def destroyed_effect(halo, unit):
    """The effe tag a unit sets off when it is destroyed, or None: its coll's body destroyed effect
    (coll +188) when the unit is destroyed after dying (unit +380 bit 1) and the effect is not the
    infection-form pop (written as the `pop` line instead). Halo's unit_destroy (../Halo3DS
    units/units.c:2711-2716), reached at the end of dying for such a unit (:6679, :10043), calls
    object_destroy, which creates the coll's body destroyed effect on the object (objects/damage.c:
    606-632), then releases the unit's spawned actors. In the ten caches: the carrier's
    (characters\\floodcarrier\\effects\\body destroyed), the Sentinel's and the Engineer's."""
    from halo_weapons import jpt_damage, referenced
    t = halo.tags
    if not unit or unit.group not in ('bipd', 'vehi'):
        return None
    flags, = t.read('I', unit.address + UNIT_FLAGS)
    coll = halo.reference(t, unit.address + UNIT_COLLISION)
    if not flags & UNIT_DESTROYED_AFTER_DYING or not coll or coll.group != 'coll':
        return None
    effect = halo.reference(t, coll.address + COLL_BODY_DESTROYED_EFFECT)
    if not effect or effect.group != 'effe':
        return None
    damage = referenced(halo, effect, 'jpt!')
    if damage and jpt_damage(halo, damage).flags & JPT_INFECTION_FORM_POP:
        return None
    return effect.name


def number(value):
    if isinstance(value, int):
        return str(value)
    text = f'{value:.6g}'
    return '0' if text == '-0' else text


def difficulty_rows(halo):
    """The globals tag's difficulty table: 35 rows of (Easy, Normal, Hard, Impossible) values,
    or None when the cache has no matg tag or no difficulty_information element. Halo reads it in
    game_difficulty_get_value (../Halo3DS source/game/game_globals.c:167-202)."""
    t = halo.tags
    matg = next((tag for tag in halo.directory if tag.group == 'matg'), None)
    if not matg:
        return None
    elements = t.block(matg.address + GLOBALS_DIFFICULTY, DIFFICULTY_STRIDE)
    if not elements:
        return None
    values = t.read(f'{len(DIFFICULTY_VALUES) * 4}f', elements[0])
    return [tuple(values[i * 4:i * 4 + 4]) for i in range(len(DIFFICULTY_VALUES))]


def write(records, path, difficulty=None):
    lines = [f'format {FORMAT}', f'# {len(records)} actor variants from the Xbox campaign caches', FIELDS.rstrip()]
    for index, row in enumerate(difficulty or ()):
        lines.append(f'difficulty {index} {DIFFICULTY_VALUES[index]} ' + ' '.join(map(number, row)))
    # A spawned actor variant names its record by index (every actv of the caches has one).
    index_of = {r['path']: i for i, r in enumerate(records)}
    for index, r in enumerate(records):
        lines += [f'actor {index}', f'path {r["path"]}',
                  f'family {r["family"]} {r["kind"]} {r["type"]} {r["actr_flags"]} {r["actv_flags"]}',
                  f'weapon {r["weapon"]}']
        for key in ('vision', 'fire', 'burst', 'grenade', 'vitality', 'defense', 'morale', 'berserk', 'positions'):
            lines.append(key + ' ' + ' '.join(map(number, r[key])))
        lines.append(f'team {r.get("team", -1)}')
        lines.append('unitmelee ' + ' '.join(map(number, r.get('unitmelee', (0.0, 0.0, 0.0)))))
        lines += ['material ' + ' '.join(map(number, m)) for m in r.get('materials', [])]
        if r.get('major'):
            lines.append(f'major {r["major"]}')
        if 'unit' in r:
            lines.append('unit ' + ' '.join(map(number, r['unit'])))
        spawn = r.get('spawn')
        if spawn and spawn[0] in index_of:
            lines.append('spawn ' + ' '.join(map(number, (index_of[spawn[0]], *spawn[1:]))))
        if r.get('pop'):
            lines.append('pop ' + ' '.join(map(number, r['pop'])))
        if r.get('destroyed'):
            lines.append(f'destroyed {r["destroyed"]}')
        for key in ('resistance', 'shieldtime'):
            if r.get(key):
                lines.append(key + ' ' + ' '.join(map(number, r[key])))
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
    records, tables = {}, {}
    for cache in CAMPAIGN:
        with (args.maps / f'{cache}.map').open('rb') as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            tables[cache] = difficulty_rows(halo)
            for tag in halo.directory:
                if tag.group == 'actv' and tag.name not in records:
                    records[tag.name] = actor_record(halo, tag, weapons)
    ordered = [records[name] for name in sorted(records)]
    # The ten caches carry the same globals tag; a disagreement would make the table depend on the
    # mission, which the engine does not model: keep the first and say so.
    present = [table for table in tables.values() if table]
    difficulty = present[0] if present else None
    if any(table != difficulty for table in present):
        print('warning: the caches disagree on the globals difficulty table; using the first cache\'s')
    target = args.output / 'packages/halo/actors.txt'
    write(ordered, target, difficulty)
    bodies = sum(r['kind'] >= 0 for r in ordered)
    majors = sum(bool(r.get('major')) for r in ordered)
    spawns = sum(bool(r.get('spawn')) for r in ordered)
    pops = sum(bool(r.get('pop')) for r in ordered)
    blasts = sum(bool(r.get('destroyed')) for r in ordered)
    print(f'{len(ordered)} actor variants ({bodies} with engine bodies, {majors} with a major upgrade, '
          f'{spawns} releasing actors, {pops} popping, {blasts} with a body destroyed effect), '
          f'{len(difficulty or ())} difficulty values -> {target}')


if __name__ == '__main__':
    main()
