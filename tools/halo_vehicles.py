#!/usr/bin/env python3
"""Export the retail campaign vehicle definitions and models.

Every vehicle in VEHICLES becomes one record in packages/halo/vehicles.txt, read
from the first campaign cache that holds its `vehi` tag: the drive parameters
(halo_cast.motion: units per 30 Hz tick, turn in radians per tick), the render
model's bounds in the model frame (x forward, y left, z up, Halo world units),
the seats with their markers, and the vehicle weapon. The model is exported to
packages/models/halo/vehicle/<name> at EXPORT_SCALE AssaultCube units per Halo
unit; the engine scales it to the section (source/src/halo_vehicle.cpp).

tools/import_halo_campaign.py writes the scenario placements of these vehicles
into the section maps by their index here (vehicle_type), so the order of
VEHICLES must not change without re-importing the campaign.
"""
import argparse
import math
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap
from halo_cast import cast_metadata

CAMPAIGN = ('d40', 'b40', 'a30', 'b30', 'c40', 'a50', 'c10', 'd20', 'a10', 'c20')
FORMAT = 2
EXPORT_SCALE = 4
# vehi: the vehicle section follows the object (380) and unit (372) sections; its suspension and
# crash sounds are at 192 and 208 in it (warthog_suspension, jeep_hit_solid). The engine's loop is
# the first looping sound among the object's attachments (warthog_7_engine, ghost, banshee_engine).
VEHICLE_SECTION = 380 + 372
SUSPENSION_SOUND, CRASH_SOUND = VEHICLE_SECTION + 192, VEHICLE_SECTION + 208
OBJECT_ATTACHMENTS, ATTACHMENT_STRIDE = 320, 72
# The object's collision model (coll, object +112): flags u32 at 0 (bit 5 only damaged by
# explosives, bit 6 only damaged while occupied, bit 1 takes body damage for its children), the
# indirect (area) damage material index at 4, the maximum body vitality at 8 and the body depleted
# effect at 168; its materials block at 564 (72 bytes: the material type at 36 and the body damage
# multiplier at 60). A vitality of 0 is indestructible (the Warthog, the Scorpion, the turret).
OBJECT_COLLISION = 112
UNIT_SEATS, SEAT_STRIDE = 740, 284  # unit seats block: flags u32 at 0, label at 4, marker at 36
COLL_FLAGS, COLL_INDIRECT, COLL_VITALITY, COLL_DEPLETED_EFFECT = 0, 4, 8, 168
COLL_MATERIALS, MATERIAL_STRIDE, MATERIAL_TYPE, MATERIAL_BODY = 564, 72, 36, 60

# name, tag, engine class, vehicle weapon, whether the player may take the driver's seat.
# `car` drives on wheels (the Warthog), `turret` is a fixed gun with a gunner seat, `hover`
# floats over the ground and strafes (the Ghost, the Wraith), `tank` turns on its tracks (the
# Scorpion), `flyer` flies when piloted and lands when empty (the Banshee). The driver fires the
# vehicle weapon of a hover, tank or flyer (its first trigger; the second, the Banshee's fuel rod
# and the Scorpion's machine gun, on the secondary key). The Wraith is enemy-only in Halo's
# campaign. Indices are stored in the maps.
VEHICLES = (
    ('warthog', r'vehicles\warthog\warthog', 'car', r'vehicles\warthog\warthog gun', True),
    ('turret', r'vehicles\c gun turret\c gun turret', 'turret', r'vehicles\c gun turret\c gun turret gun', True),
    ('ghost', r'vehicles\ghost\ghost', 'hover', r'vehicles\ghost\ghost gun', True),
    ('scorpion', r'vehicles\scorpion\scorpion', 'tank', r'vehicles\scorpion\scorpion cannon', True),
    ('banshee', r'vehicles\banshee\banshee', 'flyer', r'vehicles\banshee\banshee gun', True),
    ('wraith', r'vehicles\wraith\wraith', 'hover', r'vehicles\wraith\wraith mortar', False),
)

FIELDS = '''# vehicle <index> <name>
# path <tag>                   retail vehi tag
# class car|turret|hover|tank|flyer|prop
# player 0|1                   whether the player may drive it (format 2)
# model <model>|- <scale>      under packages/models, AssaultCube units per Halo unit at scale 1
# motion <kind> <forward> <reverse> <accel> <decel> <turn> <slide>   per 30 Hz tick
# bounds <x0> <y0> <z0> <x1> <y1> <z1>   model frame, Halo units (x forward, y left, z up)
# radius <r>
# weapon <tag>|-
# seat <name> <marker> <x> <y> <z> <enter x> <enter y> <enter z> [<flags>]   marker positions in the
#                              model frame; the unit seat flags (1 invisible: the rider is inside the
#                              hull, the Wraith's driver; 4 driver, 8 gunner)
# camera <x> <y> <z>           first camera marker, or the bounds' top centre
# sound <role> <snd! tag>      engine (looped while driven), crash, suspension (a hard landing);
#                              older games skip these lines
# vitality <body> <coll flags> <material type> <body multiplier>   the collision model's body
#                              vitality (0: indestructible), its flags (32 only damaged by
#                              explosives, 64 only damaged while occupied, 2 takes the riders'
#                              damage), the retail material type of its indirect damage material
#                              (the jpt! modifier column) and that material's body multiplier
# destroyed <effe tag>         the body depleted effect, played where it is destroyed
# variant <actv tag>           the actor variant an actor seated at its gun fights as (by name:
#                              "<vehicle tag> pilot" or "gunner"; the retail tags reference none)
'''


def vehicle_type(path):
    """Index of a vehicle tag in VEHICLES, or None for vehicles the engine leaves out."""
    for index, (_, tag, _, _, _) in enumerate(VEHICLES):
        if tag == path:
            return index
    return None


def model_bounds(model):
    bones = [model.bind_pose[name].compose(inverse) for name, inverse in model.nodes]
    low, high = [math.inf] * 3, [-math.inf] * 3
    for mesh in model.meshes:
        for vertex, _, _, influences in mesh.vertices:
            point = [0.0, 0.0, 0.0]
            for index, weight in influences:
                p = bones[index].point(vertex)
                for i in range(3):
                    point[i] += weight * p[i]
            for i in range(3):
                low[i] = min(low[i], point[i])
                high[i] = max(high[i], point[i])
    if not all(math.isfinite(v) for v in low + high):
        return (-.5, -.5, 0.), (.5, .5, 1.)
    return tuple(low), tuple(high)


def record(halo, tag, index, name, kind, weapon, player, output):
    from import_halo_models import Model, export

    t = halo.tags
    reference = halo.reference(t, tag.address + 40)
    model = Model(halo, reference.name, 'vehicle') if reference and reference.group == 'mode' else None
    motion, rates, _, markers, seats = cast_metadata(halo, tag, model)
    low, high = model_bounds(model) if model else ((-.5, -.5, 0.), (.5, .5, 1.))
    radius, = t.read('f', tag.address + 4)
    path = '-'
    if model and model.meshes:
        extent = max(abs(v) for v in low + high)
        directory = output / 'packages/models/halo/vehicle'
        export(halo, directory, name, [model], None, (), EXPORT_SCALE, quiet=True,
               geometry_scale=max(100, math.ceil(extent * 18)))
        if (directory / name / 'tris.md3').is_file():
            path = f'halo/vehicle/{name}'
    marker = {m[0]: m[1:4] for m in markers}
    rows = []
    flags = [t.read('I', seat)[0] for seat in t.block(tag.address + UNIT_SEATS, SEAT_STRIDE)]
    for index, (seat, label) in enumerate(seats):
        at = marker.get(label, ((low[0] + high[0]) / 2, 0., high[2] * .5))
        enter = marker.get(f'{label} enter', at)
        rows.append((seat.replace(' ', '_'), label.replace(' ', '_'), *at, *enter, flags[index] if index < len(flags) else 0))
    camera = next((marker[n] for n in ('camera driver', 'camera', 'driver camera', 'camera gunner') if n in marker),
                  ((low[0] + high[0]) / 2, 0., high[2]))
    lines = [f'vehicle {index} {name}', f'path {tag.name}', f'class {kind}', f'player {int(player)}',
             f'model {path} {EXPORT_SCALE}',
             'motion %d %s' % (motion, ' '.join(f'{r:.6g}' for r in rates)),
             'bounds ' + ' '.join(f'{v:.4f}' for v in low + high),
             f'radius {radius:.4f}', f'weapon {weapon or "-"}']
    lines += ['seat %s %s %.4f %.4f %.4f %.4f %.4f %.4f %d' % row for row in rows]
    lines += ['camera %.4f %.4f %.4f' % tuple(camera)]
    lines += [f'sound {role} {sound}' for role, sound in vehicle_sounds(halo, tag)]
    vitality, flags, material, multiplier, effect = vehicle_health(halo, tag)
    lines.append(f'vitality {vitality:.6g} {flags} {material} {multiplier:.6g}')
    if effect:
        lines.append(f'destroyed {effect}')
    variant = vehicle_variant(halo, tag.name)
    if variant:
        lines.append(f'variant {variant}')
    lines.append('end')
    return lines


def vehicle_health(halo, tag):
    """(body vitality, coll flags, material type, body multiplier, depleted effect tag or None)."""
    t = halo.tags
    coll = halo.reference(t, tag.address + OBJECT_COLLISION, 'coll')
    if not coll:
        return 0.0, 0, -1, 1.0, None
    a = coll.address
    flags, = t.read('I', a + COLL_FLAGS)
    indirect, = t.read('h', a + COLL_INDIRECT)
    vitality, = t.read('f', a + COLL_VITALITY)
    materials = [(t.read('h', m + MATERIAL_TYPE)[0], t.read('f', m + MATERIAL_BODY)[0])
                 for m in t.block(a + COLL_MATERIALS, MATERIAL_STRIDE)]
    material, multiplier = (materials[indirect] if 0 <= indirect < len(materials) else
                            materials[0] if materials else (-1, 1.0))
    effect = halo.reference(t, a + COLL_DEPLETED_EFFECT)
    if not (vitality > 0 and math.isfinite(vitality)):
        vitality = 0.0
    return vitality, flags, material, multiplier, effect.name if effect and effect.group == 'effe' else None


def vehicle_variant(halo, path):
    """The actv tag named after the vehicle (`<path> pilot`, `<path> gunner`), or None."""
    names = sorted(tag.name for tag in halo.directory if tag.group == 'actv' and tag.name.lower().startswith(path.lower() + ' '))
    return names[0] if names else None


def vehicle_sounds(halo, tag):
    """(role, snd! tag) pairs: the engine loop, the crash and the suspension sounds."""
    from halo_weapons import effect_sound
    t, found = halo.tags, []
    for attachment in t.block(tag.address + OBJECT_ATTACHMENTS, ATTACHMENT_STRIDE):
        loop = halo.reference(t, attachment)
        if loop and loop.group == 'lsnd':
            name = effect_sound(halo, loop)
            if name:
                found.append(('engine', name))
                break
    for role, offset in (('crash', CRASH_SOUND), ('suspension', SUSPENSION_SOUND)):
        sound = effect_sound(halo, halo.reference(t, tag.address + offset))
        if sound:
            found.append((role, sound))
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='Directory with the expanded Xbox campaign caches')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()
    records = {}
    for cache in CAMPAIGN:
        wanted = [v for v in VEHICLES if v[1] not in records]
        if not wanted:
            break
        source = args.maps / f'{cache}.map'
        if not source.is_file():
            continue
        with source.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            tags = {tag.name: tag for tag in halo.directory if tag.group == 'vehi'}
            for name, path, kind, weapon, player in wanted:
                if path in tags:
                    records[path] = record(halo, tags[path], vehicle_type(path), name, kind, weapon, player, args.output)
    missing = [v[1] for v in VEHICLES if v[1] not in records]
    if missing:
        raise SystemExit(f'vehicle tags not found in the campaign caches: {", ".join(missing)}')
    target = args.output / 'packages/halo/vehicles.txt'
    target.parent.mkdir(parents=True, exist_ok=True)
    body = [f'format {FORMAT}', f'# {len(VEHICLES)} vehicles from the Xbox campaign caches', FIELDS.rstrip('\n')]
    for _, path, _, _, _ in VEHICLES:
        body += records[path]
    target.write_text('\n'.join(body) + '\n', encoding='ascii')
    print(f'{len(VEHICLES)} vehicles -> {target}')


if __name__ == '__main__':
    main()
