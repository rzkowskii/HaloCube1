#!/usr/bin/env python3
"""Convert the opening Halo CE Xbox A30 encounter into an AssaultCube map."""

import argparse
from collections import Counter, deque
from dataclasses import dataclass
import gzip
import hashlib
import math
import mmap
from pathlib import Path
import struct

from halo_map_art import SCENERY_MODELS, write_materials, write_scenery, write_sky, write_world_mesh


TAG_BASE = 0x803A6000
# A30 BSP 0 opens in a valley. Crop from the lifeboat to the first structure.
FACTOR = 9
SIZE = 1 << FACTOR
ORIGIN_X = -37
ORIGIN_Y = 56
MARGIN = 14
HORIZONTAL_SCALE = 2.75
VERTICAL_SCALE = 2.0
SOURCE_FLOOR = 48.0
RETAIL_SHA256 = "287959f34a7379a1dbec5fec15b4d94d17fe97ccd861f5c0c8fc80148b7ed293"
CROP_X = 74
CROP_Y = -123
FLOOR_BASE = 66
CEILING = 112
SKY, _WATER_SLOT, ROCK, GRASS, DIRT, METAL, MOSS = range(7)
LIGHT, PLAYERSTART, AMMO, GRENADE, HEALTH, MAPMODEL = 1, 2, 4, 5, 6, 10


def ac_xy(x, y):
    return (x - ORIGIN_X) * HORIZONTAL_SCALE + MARGIN, (ORIGIN_Y - y) * HORIZONTAL_SCALE + MARGIN


def ac_z(z):
    return (z - SOURCE_FLOOR) * VERTICAL_SCALE


def ac_yaw(radians):
    return (round(900 - math.degrees(radians) * 10)) % 3600


class Region:
    def __init__(self, data, offset, size, base):
        if not 0 <= offset <= len(data) - size:
            raise ValueError("Cache region exceeds file bounds")
        self.data = data
        self.offset = offset
        self.size = size
        self.base = base

    def read(self, fmt, address):
        position = address - self.base
        if not 0 <= position <= self.size - struct.calcsize('<' + fmt):
            raise ValueError(f"Cache pointer outside its region: {address:#x}")
        return struct.unpack_from('<' + fmt, self.data, self.offset + position)

    def block(self, address, stride):
        count, start = self.read('II', address)
        if count and not self.base <= start <= self.base + self.size - count * stride:
            raise ValueError(f"Cache block outside its region: {address:#x}")
        return range(start, start + count * stride, stride)

    def name(self, address, size=256):
        if not self.base <= address < self.base + self.size:
            raise ValueError(f"Cache name outside its region: {address:#x}")
        position = self.offset + address - self.base
        return self.data[position:min(position + size, self.offset + self.size)].split(b'\0', 1)[0].decode('ascii')


@dataclass(frozen=True)
class Tag:
    group: str
    name: str
    address: int


class HaloMap:
    def __init__(self, data):
        signature, version, length, _, offset, size = struct.unpack_from('<6I', data)
        if (signature, version, length) != (0x68656164, 5, len(data)):
            raise ValueError('Expected an expanded Xbox Halo CE map')
        self.name = bytes(data[32:64]).split(b'\0', 1)[0].decode('ascii')
        if data[2044:2048] != b'toof':
            raise ValueError('Invalid Halo cache footer')
        self.data = data
        self.tags = Region(data, offset, size, TAG_BASE)
        directory, scenario, _, count, _, _, _, _, signature = self.tags.read('9I', TAG_BASE)
        if signature != 0x74616773 or not 0 < count <= 65535:
            raise ValueError('Invalid Halo tag directory')
        self.directory = []
        for index in range(count):
            group, _, _, handle, name, body, _, _ = self.tags.read('8I', directory + 32 * index)
            if handle & 0xffff != index:
                raise ValueError('Invalid Halo tag handle')
            self.directory.append(Tag(group.to_bytes(4, 'big').decode('ascii'), self.tags.name(name), body))
        self.scenario = self.tag(scenario, 'scnr').address

    def tag(self, handle, group=None):
        index = handle & 0xffff
        if index >= len(self.directory):
            raise ValueError(f'Invalid tag handle: {handle:#x}')
        tag = self.directory[index]
        if group and tag.group != group:
            raise ValueError(f'Expected a {group} tag, found {tag.group}')
        return tag

    def reference(self, region, address, group=None):
        handle, = region.read('I', address + 12)
        return self.tag(handle, group) if handle != 0xffffffff else None

    def bsp(self, index=0):
        refs = self.tags.block(self.scenario + 1444, 32)
        offset, size, base = self.tags.read('3I', refs[index])
        region = Region(self.data, offset, size, base)
        address, _, _, _, _, signature = region.read('6I', base)
        if signature != 0x73627370:
            raise ValueError('Invalid Halo BSP signature')
        return region, address

    def player_starts(self):
        for start in self.tags.block(self.scenario + 852, 52):
            yield self.tags.read('4fhh', start)

    def encounters(self):
        for encounter in self.tags.block(self.scenario + 1068, 176):
            name = self.tags.name(encounter, 32)
            team, = self.tags.read('h', encounter + 36)
            for squad in self.tags.block(encounter + 128, 232):
                squad_name = self.tags.name(squad, 32)
                for position in self.tags.block(squad + 208, 28):
                    yield name, team, squad_name, self.tags.read('4f', position)

    def scenery(self):
        palettes = self.tags.block(self.scenario + 540, 48)
        for scenery in self.tags.block(self.scenario + 528, 72):
            palette, = self.tags.read('h', scenery)
            if not 0 <= palette < len(palettes):
                continue
            tag = self.reference(self.tags, palettes[palette], 'scen')
            yield tag.name, self.tags.read('6f', scenery + 8)

    def surfaces(self, index=0):
        region, bsp = self.bsp(index)
        surfaces = region.block(bsp + 248, 6)
        lightmaps = self.reference(region, bsp, 'bitm')
        for lightmap in region.block(bsp + 260, 32):
            bitmap_index, = region.read('h', lightmap)
            for material in region.block(lightmap + 20, 256):
                shader = self.reference(region, material)
                if not shader:
                    continue
                first, count = region.read('II', material + 20)
                encoding, = region.read('h', material + 176)
                vertex_count, _, _, descriptor = region.read('4I', material + 180)
                if not count:
                    continue
                if encoding not in (0, 1) or not 0 < vertex_count <= 65535 or first + count > len(surfaces):
                    raise ValueError('Invalid Halo BSP material')
                stride = 32 if encoding == 1 else 56
                address, = region.read('I', descriptor + 4)
                positions = [region.read('3f', address + i * stride) for i in range(vertex_count)]
                uv = [region.read('2f', address + i * stride + stride - 8) for i in range(vertex_count)]
                light_count, _, _, light_descriptor = region.read('4I', material + 200)
                lightmap_index = bitmap_index
                light_uv = [(0.0, 0.0)] * vertex_count
                if lightmap_index >= 0 and light_count == vertex_count:
                    light_address, = region.read('I', light_descriptor + 4)
                    light_uv = [tuple((2 * p + 1) / 65535 for p in region.read('2h', light_address + i * 8 + 4))
                                for i in range(light_count)]
                else:
                    lightmap_index = -1
                vertices = list(zip(positions, uv, light_uv))
                for surface in surfaces[first:first + count]:
                    indexes = region.read('3H', surface)
                    yield shader, lightmaps, lightmap_index, tuple(vertices[i] for i in indexes)

    def triangles(self, index=0):
        for shader, _, _, vertices in self.surfaces(index):
            yield shader, tuple(vertex[0] for vertex in vertices)



def floor_texture(shader, slope):
    name = shader.name.rsplit('\\', 1)[-1]
    if 'ground' in name:
        return DIRT if slope > .38 else MOSS if 'highland' in name else GRASS
    if 'riverbed' in name:
        return DIRT
    if 'cliff' in name:
        return ROCK
    return METAL


def sample_bsp(halo):
    right, bottom = map(math.ceil, ac_xy(CROP_X, CROP_Y))
    samples = {}
    for shader, points in halo.triangles():
        if shader.group != 'senv' or 'filler' in shader.name or max(p[2] for p in points) < SOURCE_FLOOR - 1:
            continue
        a, b, c = tuple((*ac_xy(x, y), ac_z(z)) for x, y, z in points)
        bx, by, bz = (b[i] - a[i] for i in range(3))
        cx, cy, cz = (c[i] - a[i] for i in range(3))
        area = bx * cy - by * cx
        if area <= 1e-6:
            continue
        slope = math.hypot(bz * cy - cz * by, bx * cz - cx * bz) / area
        tex = floor_texture(shader, slope)
        # Sample corners on AssaultCube's native grid. Keep overlapping BSP
        # surfaces until the walkable layer has been chosen from the player start.
        for y in range(max(MARGIN, math.ceil(min(a[1], b[1], c[1]))), min(bottom, math.floor(max(a[1], b[1], c[1]))) + 1):
            for x in range(max(MARGIN, math.ceil(min(a[0], b[0], c[0]))), min(right, math.floor(max(a[0], b[0], c[0]))) + 1):
                ax, ay = x - a[0], y - a[1]
                u = (ax * cy - ay * cx) / area
                v = (bx * ay - by * ax) / area
                if min(u, v) < -1e-5 or u + v > 1 + 1e-5:
                    continue
                z = a[2] + u * bz + v * cz
                if FLOOR_BASE - 63.75 <= z <= FLOOR_BASE:
                    samples.setdefault((x, y), []).append((z, tex))
    return samples


def nearest(sample, height):
    return min(sample, key=lambda item: (abs(item[0] - height), item[1]))


def walkable_floor(samples, start):
    x, y, z, *_ = start
    origin = tuple(round(v) for v in ac_xy(x, y))
    floor = {origin: nearest(samples[origin], ac_z(z))}
    remaining = deque([origin])
    while remaining:
        x, y = remaining.popleft()
        height, _ = floor[x, y]
        for pos in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
            if pos in floor or pos not in samples:
                continue
            next_height, tex = nearest(samples[pos], height)
            if abs(next_height - height) <= 1.35:
                floor[pos] = (next_height, tex)
                remaining.append(pos)
    return floor


def make_cubes(floor):
    # A floor-heightfield cell uses its four corner vertex deltas. Adjacent
    # solid cells still provide a corner delta to visible neighbors.
    cubes = []
    for y in range(SIZE):
        for x in range(SIZE):
            sample = floor.get((x, y))
            delta = round(4 * (FLOOR_BASE - sample[0])) if sample else 0
            corners = [(x + 1, y), (x, y + 1), (x + 1, y + 1)]
            if sample and all(corner in floor for corner in corners):
                cubes.append(bytes((2, FLOOR_BASE, CEILING, ROCK, sample[1], SKY, delta, ROCK, 0)))
            else:
                cubes.append(bytes((0, ROCK, delta)))
    return cubes


def floor_position(floor, x, y, z, search=12):
    ax, ay = ac_xy(x, y)
    x0, y0 = round(ax), round(ay)
    az = ac_z(z)
    candidates = []
    for y in range(y0 - search, y0 + search + 1):
        for x in range(x0 - search, x0 + search + 1):
            neighbors = [(x + dx, y + dy) for dy in range(-2, 3) for dx in range(-2, 3)]
            if not all(p in floor for p in neighbors):
                continue
            altitude = floor[x, y][0]
            if max(abs(floor[p][0] - altitude) for p in neighbors) > 2.8:
                continue
            candidates.append(((x - ax)**2 + (y - ay)**2 + 2.0 * (altitude - az)**2, x, y, altitude))
    if not candidates:
        raise ValueError(f'No clear floor near Halo landmark ({x0}, {y0}, {az:.2f})')
    _, x, y, altitude = min(candidates)
    return x, y, altitude


def entity(pos, kind, a1=0, a2=0, a3=0, a4=0, a5=0, a6=0, a7=0):
    x, y, z = pos
    return (x, y, round(z), a1, kind, a2, a3, a4, a5, a6, a7)


def campaign_entities(halo, floor):
    start = next(halo.player_starts())
    actors = list(halo.encounters())
    first_wave = [p for encounter, _, squad, p in actors if encounter == 'the_last_stand' and squad == 'wolf'][:4]
    second_wave = [p for encounter, _, squad, p in actors if encounter == 'lz_search' and squad == 'pass_elite']
    second_wave.extend(p for encounter, _, squad, p in actors if encounter == 'lz_search' and squad == 'pass_grunt')
    landmarks = {halo.tags.name(ref + 4, 32): halo.tags.read('6f', ref + 72)
                 for ref in halo.tags.block(halo.scenario + 864, 96)}
    checkpoint = (27, -28, 68.0, math.pi / 2)
    landing = landmarks['first_marine_extraction']
    extraction = (landing[0] + landing[3] / 2, landing[1] + landing[4] / 2, landing[2])
    def place(source, search=12):
        return floor_position(floor, *source[:3], search)
    insertion = floor_position(floor, start[0] + 1.8, start[1] + 4.0, start[2])
    recovery = place(checkpoint, 34)
    exit_point = place(extraction, 36)
    entries = [entity(insertion, PLAYERSTART, 0, 1),
               entity(recovery, PLAYERSTART, ac_yaw(checkpoint[3]), 1),
               entity(exit_point, PLAYERSTART, 0, 100)]
    # World-space guides follow the mapped valley path and the bend after
    # first contact. Extraction has the same marker type with stage zero.
    routes = (((210, 413), (210, 382)), ((200, 339), (183, 333), (183, 302), (186, 268)))
    for stage, route in enumerate(routes, 1):
        for x, y in route:
            entries.append(entity((x, y, floor[x, y][0]), PLAYERSTART, 0, 100, stage))
    for wave, group in enumerate((first_wave, second_wave[:4])):
        for index, actor in enumerate(group):
            position = place(actor, 24)
            entries.append(entity(position, PLAYERSTART, ac_yaw(actor[3]), 0, wave, int(index == 0)))
    for pos, kind in (((31, -96, 58), AMMO), ((34, -77, 58), GRENADE),
                      ((29, -66, 59), HEALTH), ((28, -29, 68), AMMO),
                      ((31, -27, 68), HEALTH), ((29, -20, 70), GRENADE)):
        position = place(pos, 24)
        entries.append(entity(position, kind, round((position[2] - FLOOR_BASE) * 10)))
    for point, color in ((insertion, (220, 170, 113)), (recovery, (120, 157, 211)), (exit_point, (112, 224, 223))):
        x, y, z = point
        entries.append(entity((x, y, z + 3.5), LIGHT, 22, *color))
    for name, (x, y, z, yaw, pitch, roll) in halo.scenery():
        model = name.rsplit('\\', 1)[-1]
        if model not in SCENERY_MODELS:
            continue
        ax, ay = map(round, ac_xy(x, y))
        az = ac_z(z)
        sample = floor.get((ax, ay))
        if not sample or abs(az - sample[0]) > 5:
            continue
        slot = SCENERY_MODELS.index(model)
        # Renderer/collision anchor map models to the nominal cube floor.
        # The map config gives each model a shared -64 cube base offset.
        elevation = round((az - FLOOR_BASE + 64) * 5)
        if 0 <= elevation <= 255:
            entries.append(entity((ax, ay, az), MAPMODEL, round(math.degrees(yaw) * 10) + 1800, slot, elevation,
                                  0, round(math.degrees(pitch) * 10), round(math.degrees(roll))))
    return entries


def encode_map(cubes, entries, factor=FACTOR, title=b'Halo | A30 - The Crash Site'):
    texture_order = bytes(range(256)) * 3
    flags = (1 << 8) | (1 << 10)
    header = struct.pack('<4s4i128s768si4s4i10i', b'ACMP', 10, 980, factor, len(entries),
                         title, texture_order, -1000000, b'\0'*4, 1, 0x81939F, flags, 0, *([0]*10))
    assert len(header) == 980
    raw = bytearray(header)
    for entry in entries:
        raw.extend(struct.pack('<4h4BhbB', *entry))
    previous = None
    repeat = 0
    for cube in cubes:
        if cube == previous and repeat < 255:
            repeat += 1
            continue
        if repeat:
            raw.extend((255, repeat))
            repeat = 0
        if cube == previous:
            repeat = 1
        else:
            raw.extend(cube)
            previous = cube
    if repeat:
        raw.extend((255, repeat))
    return gzip.compress(raw, mtime=0)


def build(halo, output):
    samples = sample_bsp(halo)
    floor = walkable_floor(samples, next(halo.player_starts()))
    print(f'Walkable floor: {len(floor):,} of {len(samples):,} sampled terrain vertices')
    entries = campaign_entities(halo, floor)
    for entry in entries:
        if entry[4] == PLAYERSTART:
            print('Map start marker', entry[:8])
    output = output / 'packages'
    write_materials(halo, output / 'textures' / 'halo_a30')
    write_sky(halo, output / 'textures' / 'skymaps' / 'halo_a30')
    write_scenery(halo, output / 'models' / 'mapmodels' / 'halo_a30', HORIZONTAL_SCALE, VERTICAL_SCALE)
    write_world_mesh(halo, output, ac_xy, ac_z, (ORIGIN_X - 45, CROP_X + 45, CROP_Y - 30, ORIGIN_Y + 32))
    output = output / 'maps'
    output.mkdir(parents=True, exist_ok=True)
    (output / 'halo_a30.cgz').write_bytes(encode_map(make_cubes(floor), entries))


def describe(halo):
    print('Player starts', list(halo.player_starts())[:40])
    for address in halo.tags.block(halo.scenario + 48, 16):
        sky = halo.reference(halo.tags, address, 'sky ')
        print('Sky', sky)
        model = halo.reference(halo.tags, sky.address)
        print('Sky model', model)
    region, bsp = halo.bsp()
    print('BSP bounds', region.read('6f', bsp + 200))
    shaders = Counter(shader for shader, _ in halo.triangles())
    for shader, count in shaders.most_common():
        if shader.group == 'senv':
            maps = [halo.reference(halo.tags, shader.address + offset) for offset in (136, 184, 204)]
            print(count, shader.name, [tag.name for tag in maps if tag])
        else:
            print(count, shader.name)
    groups = {}
    for name, team, squad, position in halo.encounters():
        groups.setdefault((name, team, squad), []).append(tuple(round(v, 2) for v in position))
    for group, positions in groups.items():
        print(group, len(positions), positions[:10])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('map', type=Path, help='Expanded Halo CE Xbox a30.map')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent, help='AssaultCube overlay root')
    parser.add_argument('--describe', action='store_true', help='Inspect source BSP and encounter records')
    args = parser.parse_args()
    with args.map.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        if hashlib.sha256(data).hexdigest() != RETAIL_SHA256:
            parser.error('Expected the exact expanded North American Xbox Halo CE 1.0 a30.map')
        halo = HaloMap(data)
        if args.describe:
            describe(halo)
        else:
            build(halo, args.output)


if __name__ == '__main__':
    main()
