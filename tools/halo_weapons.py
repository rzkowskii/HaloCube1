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
from collections import namedtuple
from pathlib import Path

from import_halo_a30 import HaloMap

CAMPAIGN = ('a10', 'a30', 'a50', 'b30', 'b40', 'c10', 'c20', 'c40', 'd20', 'd40')
TICKS = 30
FORMAT = 3                        # 3: projectiles add the area inner radius, the air damage range and the
#                                   side effect; 2: damage is <lower> <upper min> <upper max> (1 had two)

# Byte offsets inside the tags. Objects start with a 380-byte object section;
# items add 396 more, so weapon fields begin at 776.
OBJECT, ITEM = 380, 396
WEAPON = OBJECT + ITEM
MAGAZINE_STRIDE, TRIGGER_STRIDE = 112, 276

# damage_effect (jpt!) layout in the Xbox Halo CE 1.0 caches, from the tag body. Read from the
# bytes of every campaign jpt! tag: weapons\frag grenade\explosion gives radius 1.5-2.5, category
# 3 (grenade), flags 0x20 (detonates explosives), AOE core radius 0.4, lower bound 80, upper bound
# 120-120; weapons\sniper rifle\sniper bullet a 0.5 vehicle passthrough penalty after its 101-101;
# the pistol and sniper bullets flag 0x2 (can cause headshots), the pulse generator 0x40 (only
# hurts shields), flames 0x80 (flaming death), the infection form 0x1000 (pop).
JPT_RADIUS = 0                    # 2f: full effect inside the first, none past the second
JPT_CUTOFF_SCALE = 8              # f
JPT_FLAGS = 12                    # u32
DAMAGE = 452                      # the damage section; its fields, from it:
JPT_SIDE_EFFECT = 0               # h: 0 none, 1 harmless, 2 lethal to the unsuspecting, 3 EMP
JPT_CATEGORY = 2                  # h: 0 none, 1 falling, 2 bullet, 3 grenade, 4 high explosive,
#                                   5 sniper, 6 melee, 7 flame, 8 mounted weapon, 9 vehicle,
#                                   10 plasma, 11 needle, 12 shotgun
JPT_DAMAGE_FLAGS = 4              # u32: 0x1 does not hurt owner, 0x2 headshots, 0x4 pings resistant
#                                   units, 0x20 detonates explosives, 0x40 only hurts shields, ...
JPT_CORE_RADIUS = 8               # f: AOE core radius
JPT_LOWER = 12                    # f: damage lower bound, what a hit scaled down to nothing deals
JPT_UPPER = 16                    # 2f: damage upper bound (min, max), a full-strength hit's range
JPT_PASSTHROUGH = 24              # f: vehicle passthrough penalty
JPT_CAMOUFLAGE = 28               # f: active camouflage damage
JPT_STUN = 32                     # 3f: stun, maximum stun, stun time
JPT_ACCELERATION = 48             # f: instantaneous acceleration
# (40 bytes on, the material damage modifiers.)
Damage = namedtuple('Damage', 'inner outer side_effect category flags core lower upper_min upper_max passthrough')
JPT_MATERIALS = 512               # 33f from the tag start: the damage modifier per retail material type
MATERIAL_COUNT = 33               # 0 dirt ... 12 hunter armor, 13 hunter skin, 25 sentinel, 26 monitor, 32 hunter shield
ACTOR_WEAPON = 100                # carried weapon reference inside an actv tag
# Sound references. A weapon's own: ready effect, overheated effect, pickup sound and its
# first-person animation graph (whose animations carry the reload, melee and ready sounds).
WEAPON_READY, WEAPON_OVERHEATED, WEAPON_PICKUP, WEAPON_FP_ANIMATIONS = 52, 108, 392, 356
# The zoom-in and zoom-out sounds follow the pickup sound (the pistol's and sniper rifle's
# sniper_2x_zoom and sniper_10x_zoom).
WEAPON_ZOOM_IN, WEAPON_ZOOM_OUT = 408, 424
MAGAZINE_EQUIPMENT, MAGAZINE_EQUIPMENT_STRIDE = 100, 28     # the ammo pickups a magazine takes
FIRING_EFFECTS, FIRING_EFFECT_STRIDE = 264, 132            # inside a trigger
FIRING_EFFECT, FIRING_MISFIRE, FIRING_EMPTY = 36, 52, 68
ANTR_SOUNDS, ANTR_ANIMATIONS, ANIMATION_STRIDE, ANIMATION_SOUND = 84, 116, 180, 60
EFFECT_EVENTS, EVENT_STRIDE, EVENT_PARTS, PART_STRIDE, PART_TYPE = 52, 68, 44, 104, 24
# Grenades: the globals' grenade block, the projectile's fuse and material responses, the eqip powerup.
GLOBALS_GRENADES, GRENADE_STRIDE = 0x128, 68
PROJECTILE_FLAGS, PROJECTILE_TIMER_STARTS, PROJECTILE_ARMING, PROJECTILE_EFFECT = 0, 4, 40, 48
PROJECTILE_TIMER, PROJECTILE_ATTACHED = 64, 152
# The super detonation (the needle's "has super combining explosion", flag 0x8): its effect at +16
# (weapons\needler\effects\explosion, whose jpt! weapons\needler\explosion is EMP, 0.5-1.0 world
# units, 60 down to 40). The tags hold no needle count; the engine decides it (halo_supercombine).
PROJECTILE_SUPER_DETONATION, PROJECTILE_SUPER_COMBINING = 16, 0x8
# The projectile's air damage range (2f, world units): an impact closer than the first deals
# full strength, falling to the jpt! lower bound at the second (shotgun pellet 1.5-3, plasma
# rifle and pistol bolts 20-50, charged plasma bolt 5-20, assault rifle bullet 5-15). The water
# damage range follows at +96; there is no water combat in the campaign sections.
PROJECTILE_AIR_DAMAGE_RANGE = 84
MATERIAL_RESPONSES, MATERIAL_STRIDE, POTENTIAL_RESPONSE, RESPONSE_ATTACH = 196, 160, 36, 4
OBJECT_ATTACHMENTS, ATTACHMENT_STRIDE = 320, 72
LOOPING_TRACKS, TRACK_STRIDE, TRACK_LOOP = 60, 160, 64
EQUIPMENT = WEAPON                # eqip fields follow the item section as the weapon's do
EQUIPMENT_FORMAT = 2              # 2: blast and attached damage carry the upper bound's min and max
# Interface sounds the engine plays for the player's shield and health.
UI_SOUNDS = {'shield-low': r'sound\sfx\ui\shield_low', 'health-low': r'sound\sfx\ui\health_low_heart'}

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
# melee <lower> <upper min> <upper max> <side effect>
#   jpt! damage: a full-strength hit deals a random amount in [upper min, upper max]; the lower
#   bound is what Halo scales a hit down to (range or area falloff, fall speed); side effect 2 is
#   "lethal to the unsuspecting" (every campaign weapon's melee), 3 EMP (the energy sword's); older
#   games read the first three fields only
# magazine <loaded> <initial total> <max total> <reload s> <rounds reloaded> <recharged/s>
# trigger <rate initial> <rate final> <rate accel s> <rate decel s> <rounds/shot> <projectiles/shot>
#         <distribution angle> <error initial> <error final> <error accel s> <error decel s>
#         <error angle min> <error angle max> <charge s> <heat/round> <age/round> <magazine>
# projectile <speed initial> <speed final> <range> <gravity> <guided rad/s> <damage lower>
#            <damage upper min> <damage upper max> <category> <area radius> <area inner radius>
#            <damage range near> <damage range far> <side effect> <path>
#   area damage is full inside the inner radius and falls to the lower bound at the outer (none
#   beyond); an impact falls from full strength at the near damage range to the lower bound at
#   the far one (far 0: no falloff); side effect 3 is EMP (the charged plasma bolt)
# core <radius>                after a projectile (and its materials line): the jpt! AOE core radius,
#                              world units (frag and plasma grenade 0.4, rocket 0.6); older games
#                              skip the line
# supercombine <timer s> <inner radius> <outer radius> <damage lower> <damage upper min>
#              <damage upper max> <side effect> <category>
#                              after a projectile whose stuck rounds super-detonate together (the
#                              needle): the timer the stuck rounds detonate after and the super
#                              detonation's area damage (its effect's jpt!); the sound role
#                              `supercombine` names its sound; older games skip the line
# sound <role> <snd! tag>      fire, charged, empty, cycle, reload, reload-empty, melee, ready, overheat, pickup,
#                              zoom-in, zoom-out, loop (held while the trigger is: the flamethrower),
#                              supercombine (the needles' super detonation)
# ammo <rounds> <eqip tag>     an ammo pickup this weapon's magazine takes, and the rounds it gives
# materials <33 modifiers>     after a melee or projectile line: its jpt! damage modifier per retail
#                              material type (0 dirt ... 12 hunter armor, 13 hunter skin, 25 sentinel,
#                              26 monitor, 32 hunter shield); older games ignore the line
'''

EQUIPMENT_FIELDS = '''# grenade <type> <maximum carried> <sticks to units 0|1> <timer starts> <arming s> <fuse s> <flags>
#   timer starts: 0 immediately, 1 after the first bounce, 2 at rest (or stuck), 3 after any bounce
# blast <inner radius> <outer radius> <damage lower> <damage upper min> <damage upper max>
#       <side effect> <category>
#   full damage inside the inner radius, falling to the lower bound at the outer (none beyond);
#   side effect 3 is EMP;
#   damage as in weapons.txt: a full-strength hit rolls [upper min, upper max]
# attached <damage lower> <damage upper min> <damage upper max>   to the unit a sticky grenade is stuck to
# materials <33 modifiers>      after a blast or attached line: the jpt! damage modifier per material type
# core <radius>                 after the blast (and its materials line): the jpt! AOE core radius
# path <proj tag> / equipment <eqip tag> / sound <role> <snd! tag>   (throw, explode, armed)
# equipment <index>
#   path <eqip tag>
#   kind <powerup> <grenade type> <powerup time s>
#     powerup: 0 none (ammo), 1 double speed, 2 over shield, 3 active camouflage,
#              4 full-spectrum vision, 5 health, 6 grenade
#   worldmodel <model>|-        under packages/models
#   ammo <weapons.txt index> <rounds>   for each weapon whose magazine takes it
#   sound pickup <snd! tag>
# sound <role> <snd! tag>       interface sounds: shield-low, health-low
'''


def jpt_damage(halo, tag):
    """The damage of a jpt! tag, read with the JPT_* layout (weapons, melee and grenades alike)."""
    t, a = halo.tags, tag.address
    inner, outer = t.read('2f', a + JPT_RADIUS)
    d = a + DAMAGE
    side_effect, = t.read('h', d + JPT_SIDE_EFFECT)
    category, = t.read('h', d + JPT_CATEGORY)
    flags, = t.read('I', d + JPT_DAMAGE_FLAGS)
    core, = t.read('f', d + JPT_CORE_RADIUS)
    lower, = t.read('f', d + JPT_LOWER)
    upper_min, upper_max = t.read('2f', d + JPT_UPPER)
    passthrough, = t.read('f', d + JPT_PASSTHROUGH)
    upper_max = max(upper_min, upper_max)
    return Damage(inner, outer, side_effect, category, flags, core, min(lower, upper_max), upper_min, upper_max,
                  passthrough)


def jpt_materials(halo, tag):
    """A jpt! tag's 33 damage modifiers, one per retail material type, or None without a tag."""
    if tag is None or tag.group != 'jpt!':
        return None
    return halo.tags.read(f'{MATERIAL_COUNT}f', tag.address + JPT_MATERIALS)


def materials_line(values):
    return 'materials ' + ' '.join(map(number, values)) if values else None


def damage_values(halo, tag):
    """Lower bound, upper bound min and max, category and area radius of a jpt! tag."""
    d = jpt_damage(halo, tag)
    return d.lower, d.upper_min, d.upper_max, d.category, d.outer


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


def effect_sound(halo, tag):
    """The sound a reference plays: a snd! itself, the first snd! part of an effe's events, or
    the loop of a looping sound (lsnd)."""
    if tag is None:
        return None
    t = halo.tags
    try:
        if tag.group == 'snd!':
            return tag.name
        if tag.group == 'lsnd':
            for track in t.block(tag.address + LOOPING_TRACKS, TRACK_STRIDE):
                loop = halo.reference(t, track + TRACK_LOOP)
                if loop and loop.group == 'snd!':
                    return loop.name
            return None
        if tag.group == 'effe':
            for event in t.block(tag.address + EFFECT_EVENTS, EVENT_STRIDE):
                for part in t.block(event + EVENT_PARTS, PART_STRIDE):
                    found = halo.reference(t, part + PART_TYPE)
                    if found and found.group == 'snd!':
                        return found.name
    except ValueError:
        return None
    return None


def animation_sounds(halo, antr):
    """First-person animation name (without 'first-person ') to the sound it plays."""
    if antr is None or antr.group != 'antr':
        return {}
    t = halo.tags
    sounds = [halo.reference(t, entry) for entry in t.block(antr.address + ANTR_SOUNDS, 20)]
    found = {}
    for animation in t.block(antr.address + ANTR_ANIMATIONS, ANIMATION_STRIDE):
        index, = t.read('h', animation + ANIMATION_SOUND)
        if 0 <= index < len(sounds) and sounds[index] and sounds[index].group == 'snd!':
            found.setdefault(t.name(animation, 32).removeprefix('first-person '), sounds[index].name)
    return found


def weapon_sounds(halo, tag):
    """Sound roles of a weapon from its tags: the first trigger's firing, empty and misfire
    effects, the second trigger's shot, the first-person animations' sounds and the pickup sound."""
    t, a = halo.tags, tag.address + WEAPON
    sounds = {}
    triggers = list(t.block(a + 500, TRIGGER_STRIDE))
    for index, trigger in enumerate(triggers[:2]):
        effects = list(t.block(trigger + FIRING_EFFECTS, FIRING_EFFECT_STRIDE))
        if not effects:
            continue
        fire = effect_sound(halo, halo.reference(t, effects[0] + FIRING_EFFECT))
        if index == 0:
            sounds['fire'] = fire
            sounds['empty'] = effect_sound(halo, halo.reference(t, effects[0] + FIRING_EMPTY))
            if t.read('f', a + 72)[0] > 0:      # only weapons that overheat misfire from heat
                sounds['overheat'] = effect_sound(halo, halo.reference(t, effects[0] + FIRING_MISFIRE))
        elif fire != sounds.get('fire'):
            sounds['charged'] = fire
    animations = animation_sounds(halo, halo.reference(t, a + WEAPON_FP_ANIMATIONS))
    full = animations.get('reload-full') or animations.get('enter') or animations.get('reload-empty')
    sounds.update({
        'cycle': animations.get('fire-1'),
        'reload': full,
        'reload-empty': animations.get('reload-empty') or full,
        'melee': animations.get('melee'),
        'ready': animations.get('ready') or effect_sound(halo, halo.reference(t, a + WEAPON_READY)),
        'pickup': effect_sound(halo, halo.reference(t, a + WEAPON_PICKUP)),
    })
    overheat = animations.get('overheating') or animations.get('o-h-s-enter') or \
        effect_sound(halo, halo.reference(t, a + WEAPON_OVERHEATED))
    if overheat:
        sounds['overheat'] = overheat
    sounds['zoom-in'] = effect_sound(halo, halo.reference(t, a + WEAPON_ZOOM_IN))
    sounds['zoom-out'] = effect_sound(halo, halo.reference(t, a + WEAPON_ZOOM_OUT))
    # A looping sound attached at a trigger marker plays while the trigger is held: the
    # flamethrower's fire_ft (its firing effect has no sound of its own).
    for attachment in t.block(tag.address + OBJECT_ATTACHMENTS, ATTACHMENT_STRIDE):
        found = halo.reference(t, attachment)
        if found and found.group == 'lsnd' and 'trigger' in t.name(attachment + 16, 32):
            sounds['loop'] = effect_sound(halo, found)
            break
    return {role: name for role, name in sounds.items() if name}


def weapon_ammo(halo, tag):
    """The ammo pickups (eqip tags) the weapon's first magazine takes, with their rounds."""
    t, a = halo.tags, tag.address + WEAPON
    found = []
    for magazine in list(t.block(a + 488, MAGAZINE_STRIDE))[:1]:
        for entry in t.block(magazine + MAGAZINE_EQUIPMENT, MAGAZINE_EQUIPMENT_STRIDE):
            rounds, = t.read('h', entry)
            equipment = halo.reference(t, entry + 12)
            if equipment and equipment.group == 'eqip':
                found.append((rounds, equipment.name))
    return found


def projectile_jpt(halo, proj):
    """The jpt! tag a projectile deals its damage with: the larger of its impact and attached
    detonation damage, else the one inside its detonation effect (None without either)."""
    t, a = halo.tags, proj.address + OBJECT
    best, damage = None, None
    for offset in (168, 152):     # impact damage, then attached detonation damage
        tag = halo.reference(t, a + offset)
        if tag and tag.group == 'jpt!':
            values = jpt_damage(halo, tag)
            # Needles carry an empty impact tag and deal their damage on detonation.
            if damage is None or values.upper_max > damage.upper_max:
                best, damage = tag, values
    if best is None:
        effect = halo.reference(t, a + 48)
        best = referenced(halo, effect, 'jpt!') if effect and effect.group == 'effe' else None
    return best


def projectile_record(halo, proj):
    t, a = halo.tags, proj.address + OBJECT
    initial, final, guided = t.read('3f', a + 104)
    maximum, gravity = t.read('2f', a + 76)
    near, far = t.read('2f', a + PROJECTILE_AIR_DAMAGE_RANGE)
    tag = projectile_jpt(halo, proj)
    d = jpt_damage(halo, tag) if tag else Damage(0.0, 0.0, 0, -1, 0, 0.0, 0.0, 0.0, 0.0, 0.0)
    return (initial * TICKS, final * TICKS, maximum, gravity, guided * TICKS,
            d.lower, d.upper_min, d.upper_max, d.category, d.outer, min(d.inner, d.outer),
            max(0.0, near), max(0.0, far), d.side_effect, proj.name)


def projectile_core(halo, proj):
    """The AOE core radius of the jpt! a projectile deals its damage with (world units)."""
    tag = projectile_jpt(halo, proj)
    return max(0.0, jpt_damage(halo, tag).core) if tag else 0.0


def super_detonation(halo, proj):
    """A projectile whose stuck rounds super-detonate together (the needle): (the supercombine
    line's values, its sound), or None."""
    t, a = halo.tags, proj.address + OBJECT
    flags, = t.read('I', a + PROJECTILE_FLAGS)
    if not flags & PROJECTILE_SUPER_COMBINING:
        return None
    effect = halo.reference(t, a + PROJECTILE_SUPER_DETONATION)
    tag = effect_jpt(halo, effect) if effect and effect.group == 'effe' else None
    if tag is None:
        return None
    _, timer = t.read('2f', a + PROJECTILE_TIMER)
    inner, outer, lower, upper_min, upper_max, side, category = jpt_values(halo, tag)
    return (max(0.0, timer), min(inner, outer), outer, lower, upper_min, upper_max, side, category), effect_sound(halo, effect)


def weapon_record(halo, tag, viewmodels, models):
    t, a = halo.tags, tag.address + WEAPON
    model = halo.reference(t, tag.address + 40)     # the object's third-person model
    melee_tag = halo.reference(t, a + 140)
    melee = ((*damage_values(halo, melee_tag)[:3], jpt_damage(halo, melee_tag).side_effect)
             if melee_tag and melee_tag.group == 'jpt!' else (0.0, 0.0, 0.0, 0))
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
        'melee_materials': jpt_materials(halo, melee_tag),
        'magazines': [], 'triggers': [],
        'sounds': weapon_sounds(halo, tag),
        'ammo': weapon_ammo(halo, tag),
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
            'materials': jpt_materials(halo, projectile_jpt(halo, proj)) if proj and proj.group == 'proj' else None,
            'core': projectile_core(halo, proj) if proj and proj.group == 'proj' else 0.0,
            'supercombine': super_detonation(halo, proj) if proj and proj.group == 'proj' else None,
        })
        if record['triggers'][-1]['supercombine'] and record['triggers'][-1]['supercombine'][1]:
            record['sounds'].setdefault('supercombine', record['triggers'][-1]['supercombine'][1])
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
        if materials_line(record.get('melee_materials')):
            lines.append(materials_line(record['melee_materials']))
        lines += ['magazine ' + ' '.join(map(number, m)) for m in record['magazines']]
        for trigger in record['triggers']:
            lines.append('trigger ' + ' '.join(map(number, trigger['values'])))
            p = trigger['projectile']
            lines.append('projectile ' + (' '.join(map(number, p[:-1])) + ' ' + p[-1] if p else '-'))
            if p and materials_line(trigger.get('materials')):
                lines.append(materials_line(trigger['materials']))
            # Optional lines older games skip; they come after the materials line, which belongs
            # to the projectile.
            if p and trigger.get('core', 0) > 0:
                lines.append(f'core {number(trigger["core"])}')
            if p and trigger.get('supercombine'):
                lines.append('supercombine ' + ' '.join(map(number, trigger['supercombine'][0])))
        lines += [f'sound {role} {name}' for role, name in sorted(record['sounds'].items())]
        lines += [f'ammo {rounds} {name}' for rounds, name in record['ammo']]
        lines.append('end')
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n')


def jpt_values(halo, tag):
    """Area damage of a jpt! tag: radius bounds (full damage inside the first, none past the
    second), damage lower bound, upper bound min and max, side effect and category."""
    d = jpt_damage(halo, tag)
    return d.inner, d.outer, d.lower, d.upper_min, d.upper_max, d.side_effect, d.category


def effect_jpt(halo, effect):
    """The jpt! part of an effe with the largest damage and radius (a detonation's blast, not its
    shock wave), or None."""
    t, best, tag = halo.tags, None, None
    if effect is None or effect.group != 'effe':
        return None
    for event in t.block(effect.address + EFFECT_EVENTS, EVENT_STRIDE):
        for part in t.block(event + EVENT_PARTS, PART_STRIDE):
            found = halo.reference(t, part + PART_TYPE)
            if found and found.group == 'jpt!':
                values = jpt_values(halo, found)
                if best is None or (values[4], values[1]) > (best[4], best[1]):
                    best, tag = values, found
    return tag


def effect_damage(halo, effect):
    """The jpt! part of an effe with the largest radius (a detonation's blast, not its shock wave)."""
    tag = effect_jpt(halo, effect)
    return jpt_values(halo, tag) if tag else None


def grenade_records(halo):
    """The globals' grenade types (0 fragmentation, 1 plasma): carried maximum, the thrown
    projectile's fuse, stickiness and blast, and their sounds."""
    t = halo.tags
    globals_tag = next(tag for tag in halo.directory if tag.group == 'matg')
    records = []
    for index, grenade in enumerate(t.block(globals_tag.address + GLOBALS_GRENADES, GRENADE_STRIDE)):
        maximum, = t.read('h', grenade)
        throw, equipment, proj = (halo.reference(t, grenade + offset) for offset in (4, 36, 52))
        if not proj or proj.group != 'proj':
            continue
        a = proj.address + OBJECT
        flags, = t.read('I', a + PROJECTILE_FLAGS)
        starts, = t.read('h', a + PROJECTILE_TIMER_STARTS)
        arming, = t.read('f', a + PROJECTILE_ARMING)
        _, fuse = t.read('2f', a + PROJECTILE_TIMER)
        sticks = any(t.read('h', response + POTENTIAL_RESPONSE)[0] == RESPONSE_ATTACH
                     for response in t.block(a + MATERIAL_RESPONSES, MATERIAL_STRIDE))
        detonation = halo.reference(t, a + PROJECTILE_EFFECT)
        attached = halo.reference(t, a + PROJECTILE_ATTACHED)
        armed = None
        for attachment in t.block(proj.address + OBJECT_ATTACHMENTS, ATTACHMENT_STRIDE):
            armed = armed or effect_sound(halo, halo.reference(t, attachment))
        sounds = {'throw': effect_sound(halo, throw), 'explode': effect_sound(halo, detonation), 'armed': armed}
        records.append({
            'type': index, 'maximum': maximum, 'sticks': int(sticks), 'starts': starts, 'arming': arming,
            'fuse': fuse, 'flags': flags, 'path': proj.name,
            'equipment': equipment.name if equipment and equipment.group == 'eqip' else None,
            'blast': effect_damage(halo, detonation) or (0.0, 0.0, 0.0, 0.0, 0.0, 0, 0),
            'blast_materials': jpt_materials(halo, effect_jpt(halo, detonation)),
            'core': max(0.0, jpt_damage(halo, effect_jpt(halo, detonation)).core) if effect_jpt(halo, detonation) else 0.0,
            'attached_materials': jpt_materials(halo, attached) if attached and attached.group == 'jpt!' else None,
            'attached': jpt_values(halo, attached)[2:5] if attached and attached.group == 'jpt!' else (0.0, 0.0, 0.0),
            'sounds': {role: name for role, name in sounds.items() if name},
        })
    return records


def equipment_record(halo, tag, models):
    t, a = halo.tags, tag.address + EQUIPMENT
    powerup, grenade = t.read('2h', a)
    time, = t.read('f', a + 4)
    model = halo.reference(t, tag.address + 40)
    pickup = effect_sound(halo, halo.reference(t, a + 8))
    return {'path': tag.name, 'kind': (powerup, grenade, time),
            'worldmodel': models.get(model.name, '-') if model else '-',
            'sounds': {'pickup': pickup} if pickup else {}}


def write_equipment(grenades, equipment, weapons, ui_sounds, path):
    """packages/halo/equipment.txt: grenade types, every campaign eqip tag (with the weapons
    whose ammo it is) and the interface sounds, for source/src/halo_weapons.cpp."""
    ammo = {}
    for index, weapon in enumerate(weapons):
        if weapon['class'] in PLAYER.values():
            for rounds, name in weapon['ammo']:
                ammo.setdefault(name, []).append((index, rounds))
    lines = [f'format {EQUIPMENT_FORMAT}', f'# {len(grenades)} grenade types and {len(equipment)} equipment tags '
             'from the Xbox campaign caches', EQUIPMENT_FIELDS.rstrip()]
    for g in grenades:
        lines += [f'grenade {g["type"]} {g["maximum"]} {g["sticks"]} {g["starts"]} {number(g["arming"])} '
                  f'{number(g["fuse"])} {g["flags"]}',
                  'blast ' + ' '.join(map(number, g['blast']))]
        if materials_line(g.get('blast_materials')):
            lines.append(materials_line(g['blast_materials']))
        if g.get('core', 0) > 0:
            lines.append(f'core {number(g["core"])}')
        lines.append('attached ' + ' '.join(map(number, g['attached'])))
        if materials_line(g.get('attached_materials')):
            lines.append(materials_line(g['attached_materials']))
        lines.append(f'path {g["path"]}')
        if g['equipment']:
            lines.append(f'equipment {g["equipment"]}')
        lines += [f'sound {role} {name}' for role, name in sorted(g['sounds'].items())]
        lines.append('end')
    for index, record in enumerate(equipment):
        lines += [f'equipment {index}', f'path {record["path"]}',
                  'kind ' + ' '.join(map(number, record['kind'])), f'worldmodel {record["worldmodel"]}']
        lines += [f'ammo {weapon} {rounds}' for weapon, rounds in ammo.get(record['path'], [])]
        lines += [f'sound {role} {name}' for role, name in sorted(record['sounds'].items())]
        lines.append('end')
    lines += [f'sound {role} {name}' for role, name in sorted(ui_sounds.items())]
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


def equipment_index(root):
    """eqip tag path to its packages/halo/equipment.txt index, read back from the table."""
    index, current = {}, None
    path = Path(root) / 'packages/halo/equipment.txt'
    if not path.exists():
        return index
    for line in path.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'equipment' and rest.isdigit():
            current = int(rest)
        elif key == 'end':
            current = None
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
    records, equipment, grenades, ui_sounds = {}, {}, [], {}
    for cache in CAMPAIGN:
        with (args.maps / f'{cache}.map').open('rb') as file, \
                mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            if not grenades:
                grenades = grenade_records(halo)
            names = {tag.name for tag in halo.directory if tag.group == 'snd!'}
            for role, name in UI_SOUNDS.items():
                if name in names:
                    ui_sounds.setdefault(role, name)
            for tag in halo.directory:
                if tag.group == 'weap' and tag.name not in records:
                    records[tag.name] = weapon_record(halo, tag, viewmodels, models)
                elif tag.group == 'eqip' and tag.name not in equipment:
                    equipment[tag.name] = equipment_record(halo, tag, models)
    ordered = [records[name] for name in sorted(records)]
    target = args.output / 'packages/halo/weapons.txt'
    write(ordered, target)
    playable = sum(r['class'] in PLAYER.values() for r in ordered)
    print(f'{len(ordered)} weapons ({playable} player weapons) -> {target}')
    target = args.output / 'packages/halo/equipment.txt'
    write_equipment(grenades, [equipment[name] for name in sorted(equipment)], ordered, ui_sounds, target)
    print(f'{len(grenades)} grenade types, {len(equipment)} equipment tags -> {target}')


if __name__ == '__main__':
    main()
