#!/usr/bin/env python3
"""Convert retail Xbox campaign BSP sections and authored scenario placements."""
import argparse
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
import math
import mmap
from pathlib import Path
import re

from import_halo_a30 import HaloMap, ac_yaw, encode_map, entity, LIGHT, PLAYERSTART, AMMO, GRENADE, HEALTH
from halo_collision import write_structure_collision
from halo_map_art import write_world_mesh


@dataclass(frozen=True)
class Mission:
    cache: str
    title: str
    sections: tuple
    end: str


# The two Control Room missions travel in opposite directions. Cinematic-only
# BSPs and optional branches stay outside the campaign route.
MISSIONS = (
    Mission('a10', 'The Pillar of Autumn', (0, 1, 2, 3, 4, 5, 6), 'end_trigger'),
    Mission('a30', 'Halo', (0, 1), 'river_boat_lz'),
    Mission('a50', 'The Truth and Reconciliation', (0, 1, 2, 3, 2), 'shuttle_door_switch_trigger'),
    Mission('b30', 'The Silent Cartographer', (0, 1, 0), 'beach_lz'),
    Mission('b40', 'Assault on the Control Room', (3, 0, 1, 10, 2, 11, 2), 'control_trigger'),
    Mission('c10', '343 Guilty Spark', (0, 1, 2, 1, 3, 4, 5), 'enc_swamp2'),
    Mission('c20', 'The Library', (0, 1, 2, 3), 'finale'),
    Mission('c40', 'Two Betrayals', (2, 10, 1, 0, 5), 'pulse_3_trigger'),
    Mission('d20', 'Keyes', (0, 1, 2, 3, 4, 3), 'hangar_exit_trigger'),
    Mission('d40', 'The Maw', (0, 1, 2, 3, 4, 5, 6, 7), 'grand_finale'),
)
INTERIOR = {
    'a10': set(range(7)), 'a50': {1, 2, 3}, 'b30': {1}, 'b40': set(range(3, 13)),
    'c10': {1, 2, 3, 4}, 'c20': set(range(4)), 'c40': set(range(3, 13)),
    'd20': {0, 2, 3, 4}, 'd40': set(range(5)),
}
ACTORS = {'grunt': 0, 'elite': 1, 'jackal': 2, 'hunter': 3, 'floodcombat elite': 4,
          'floodcombat_human': 5, 'flood_infection': 6, 'floodcarrier': 7, 'sentinel': 8}
TRANSITIONS = {
    ('a50', 0, 1): 'gravity_pad_teleport_trigger', ('a50', 1, 0): 'entr_safe',
    ('d20', 0, 1): '0_to_1_transition_trigger', ('d20', 1, 0): 'grav_lift',
    ('d20', 1, 2): '1_to_2_transition_trigger', ('d20', 2, 1): (-11.6, 11.0, -71.8),
}
# Anchors near the navigable side of scripted lifts, dropship arrivals and
# mission-specific entrances. Coordinates are in the retail BSP's space.
SECTION_ANCHORS = {
    ('a30', 0): ('first_structure_front', 'cave_floor_entrance'),
    # The return starts outside the shaft. The extraction marker is on the
    # ledge beside the hovering Pelican's rear ramp.
    ('b30', 2): ((6.1, -13.4, 4.0), (37.8, -18.6, 9.1)),
    ('b40', 1): (None, (169.5, -310.0, -10.0)),
    ('b40', 2): ((174.0, -299.0, -10.8), (264.0, -558.0, 21.8)),
    ('b40', 4): ((283.0, -622.0, 70.0), (283.0, -685.0, 70.0)),
    ('c10', 6): ((-34.0, 84.5, -27.0), 'enc_swamp2'),
    ('c40', 0): ((362.686, -712.737, 73.818), (286.5, -627.9, 70.3)),
    ('c40', 2): ((273.9, -551.6, 22.4), 'waypoint5_trigger'),
    ('c40', 3): ((204.1, -246.0, -11.0), (173.3, -103.6, 1.0)),
    ('c40', 4): ((171.3, -44.6, 33.5), 'pulse_3_trigger'),
    ('d40', 7): ((744.0, -8.0, 66.0), 'grand_finale'),
}


def volume_centers(halo):
    centers = {}
    for offset in halo.tags.block(halo.scenario + 864, 96):
        name = halo.tags.name(offset + 4, 32)
        x, y, z, w, d, h = halo.tags.read('6f', offset + 72)
        centers[re.sub(r'^bsp\s+', 'bsp', name)] = (x + w/2, y + d/2, z + h/2)
    return centers


def actor_starts(halo, index):
    palette = halo.tags.block(halo.scenario + 1056, 16)
    for encounter in halo.tags.block(halo.scenario + 1068, 176):
        bsp, = halo.tags.read('h', encounter + 126)
        if bsp != index:
            continue
        for squad in halo.tags.block(encounter + 128, 232):
            default, = halo.tags.read('h', squad + 32)
            for start in halo.tags.block(squad + 208, 28):
                variant, = halo.tags.read('h', start + 24)
                variant = default if variant < 0 else variant
                if not 0 <= variant < len(palette):
                    continue
                tag = halo.reference(halo.tags, palette[variant])
                if not tag:
                    continue
                actor = tag.name.split('\\')[1]
                if actor in ACTORS:
                    yield halo.tags.read('4f', start), ACTORS[actor]


@dataclass
class Projection:
    x0: float
    y0: float
    z0: float
    scale: float

    def xy(self, x, y):
        return (x - self.x0) * self.scale + 9, (self.y0 - y) * self.scale + 9

    def z(self, z):
        return (z - self.z0) * self.scale

    def point(self, p):
        return (*self.xy(*p[:2]), self.z(p[2]))


def rasterize(halo, bsp, projection, size):
    floors, ceilings = {}, {}
    for shader, triangle in halo.triangles(bsp):
        if shader.group != 'senv' or 'filler' in shader.name:
            continue
        a, b, c = map(projection.point, triangle)
        bx, by, bz = (b[i] - a[i] for i in range(3))
        cx, cy, cz = (c[i] - a[i] for i in range(3))
        area = bx*cy - by*cx
        if abs(area) < 1e-5:
            continue
        if math.hypot(bz*cy-cz*by, bx*cz-cx*bz) > abs(area) * 2.2:
            continue
        layer = floors if area > 0 else ceilings
        for y in range(max(6, math.ceil(min(a[1], b[1], c[1]))), min(size-7, math.floor(max(a[1], b[1], c[1])))+1):
            for x in range(max(6, math.ceil(min(a[0], b[0], c[0]))), min(size-7, math.floor(max(a[0], b[0], c[0])))+1):
                u = ((x-a[0])*cy-(y-a[1])*cx)/area
                v = (bx*(y-a[1])-by*(x-a[0]))/area
                if min(u, v) >= -1e-5 and u + v <= 1.00001:
                    z = round((a[2] + u*bz + v*cz)*4)
                    if -460 <= z <= 468:
                        layer.setdefault((x, y), set()).add(z)
    return {p: sorted(z for z in levels if not any(1 < c-z < 22 for c in ceilings.get(p, ())))
            for p, levels in floors.items()}, ceilings


def component(samples, point, elevation):
    # Represent a single height at each XY, as native AC cells require.
    floor = {point: elevation}
    queue = deque([point])
    while queue:
        x, y = queue.popleft()
        current = floor[x,y]
        for p in ((x+1,y), (x-1,y), (x,y+1), (x,y-1)):
            if p in floor:
                continue
            elevations = samples.get(p, ())
            if elevations:
                target = min(elevations, key=lambda z: abs(z-current))
                if abs(target-current) <= 6:
                    floor[p] = target
                    queue.append(p)
    return floor


def clear_square(floor, p, z):
    x, y = p
    return all(abs(floor.get((x+dx,y+dy), 10000)-z) <= 13
               for dx in range(-1, 3) for dy in range(-1, 3))


def projected_route(samples, start, end, actor_positions):
    def seed_score(p, z, target):
        return (p[0]-target[0])**2 + (p[1]-target[1])**2 + ((z/4-target[2])*2)**2

    def seed_for(target, limit=35):
        candidates = []
        for x in range(round(target[0])-limit, round(target[0])+limit+1):
            for y in range(round(target[1])-limit, round(target[1])+limit+1):
                for z in samples.get((x,y), ()):
                    if all(any(abs(other-z)<=9 for other in samples.get((x+dx,y+dy),()))
                           for dx in range(-1,3) for dy in range(-1,3)):
                        candidates.append((seed_score((x,y),z,target),(x,y),z))
        return min(candidates, default=None)

    tried, best = set(), None
    for target in (start,end,*actor_positions[:36]):
        seed = seed_for(target)
        if not seed or (seed[1],seed[2]) in tried:
            continue
        _, point, z = seed
        floor = component(samples,point,z)
        # Record multiple trial seeds for the same connected floor.
        for candidate in (start,end,*actor_positions[:36]):
            near = (round(candidate[0]),round(candidate[1]))
            if near in floor:
                tried.add((near,floor[near]))
        tried.add((point,z))
        points = {p for p, level in floor.items() if clear_square(floor,p,level)}
        if len(points)<20:
            continue
        first = closest(points,floor,start)
        parents = routes(points,first,floor)
        last = closest(parents,floor,end)
        path = trace_path(parents,last)
        if len(path)<16:
            distances = {first: 0}
            for p, parent in parents.items():
                if parent is not None:
                    distances[p] = distances[parent]+1
            last = max(distances, key=lambda p: (distances[p],p))
            path = trace_path(parents,last)
        if len(path) < 16:
            continue
        score = seed_score(path[0],floor[path[0]],start) + seed_score(path[-1],floor[path[-1]],end)
        if best is None or score < best[0]:
            best = (score,floor,parents,path)
        if score < 42:
            break
    return best


def trace_path(parents,end):
    path = [end]
    while parents[path[-1]] is not None:
        path.append(parents[path[-1]])
    return path[::-1]


def closest(points, floor, target):
    x, y, z = target
    return min(points, key=lambda p: ((p[0]-x)**2 + (p[1]-y)**2 + ((floor[p]/4-z)*2)**2, p))


def routes(points, start, floor):
    parents = {start: None}
    queue = deque([start])
    while queue:
        x, y = queue.popleft()
        for p in ((x+1,y), (x-1,y), (x,y+1), (x,y-1)):
            if p in points and p not in parents and abs(floor[p]-floor[x,y]) <= 5:
                parents[p] = (x,y)
                queue.append(p)
    return parents


def native_cubes(floor, ceilings, size):
    for y in range(size):
        for x in range(size):
            z = floor.get((x,y))
            if z is None:
                yield b'\x00\x02\x00'
                continue
            corners = [floor.get((x+dx,y+dy)) for dx,dy in ((1,0),(0,1),(1,1))]
            if any(v is None for v in corners):
                yield b'\x00\x02\x00'
                continue
            base = z // 4
            # Keep overhangs above the selected layer as native ceilings.
            roof = min((c//4 for c in ceilings.get((x,y), ()) if c >= z+22), default=125)
            roof = min(125, max(base+6, roof))
            # Native SPACE cells keep consistent elevations across steep or
            # overlapping rooms. Exact render surfaces are drawn separately.
            yield bytes((4, (z//4)&255, roof&255, 2, 2, 0, 0, 2, 0))


def section_layout(halo, mission, part):
    bsp = mission.sections[part]
    region, address = halo.bsp(bsp)
    xmin, xmax, ymin, ymax, _, _ = region.read('6f', address+200)
    volumes = volume_centers(halo)
    actors = list(actor_starts(halo, bsp))
    previous = mission.sections[part-1] if part else None
    following = mission.sections[part+1] if part+1 < len(mission.sections) else None
    def resolve(anchor):
        return volumes.get(anchor) if isinstance(anchor,str) else anchor
    start = next(halo.player_starts())[:3]
    if previous is not None:
        start = resolve(TRANSITIONS.get((mission.cache,bsp,previous), f'bsp{bsp},{previous}'))
        if start is None and actors:
            start = actors[0][0][:3]
        if start is None:
            start = ((xmin+xmax)/2,(ymin+ymax)/2,region.read('f', address+216)[0]+2)
    end = volumes.get(TRANSITIONS.get((mission.cache,bsp,following), f'bsp{bsp},{following}')) if following is not None else volumes.get(mission.end)
    if end is None and actors:
        end = max((p[:3] for p,_ in actors), key=lambda p: math.dist(start,p))
    if end is None:
        end = start
    start_override, end_override = SECTION_ANCHORS.get((mission.cache,part), (None,None))
    start = resolve(start_override) or start
    end = resolve(end_override) or end
    indoor = bsp in INTERIOR.get(mission.cache, ())
    margin = 28 if indoor else 65
    xmin, xmax = max(xmin,min(start[0],end[0])-margin), min(xmax,max(start[0],end[0])+margin)
    ymin, ymax = max(ymin,min(start[1],end[1])-margin), min(ymax,max(start[1],end[1])+margin)
    span = max(xmax-xmin,ymax-ymin,1)
    scale = min(8.0 if indoor else 4.1, 980/span)
    factor = max(7, math.ceil(math.log2(span*scale+24)))
    projection = Projection(xmin, ymax, start[2], scale)
    if mission.cache == 'b30' and part == 2:
        # Both playable views of BSP 0 share one coordinate space.
        *_, factor, projection = section_layout(halo, mission, 0)
    return bsp, indoor, (xmin, xmax, ymin, ymax), start, end, actors, factor, projection


def make_section(halo, mission, part, output):
    bsp, indoor, (xmin, xmax, ymin, ymax), start, end, actors, factor, projection = section_layout(halo, mission, part)
    size, scale = 1 << factor, projection.scale
    projected_start, projected_end = projection.point(start), projection.point(end)
    samples, ceilings = rasterize(halo, bsp, projection, size)
    projected = projected_route(samples, projected_start, projected_end,
                                [projection.point(p) for p,_ in actors])
    if projected is None:
        raise ValueError(f'no continuous walkable layer at {start}')
    _, floor, parents, path = projected
    def xyz(p):
        return (*p,floor[p]/4)
    def facing(a,b):
        return ac_yaw(math.atan2(a[1]-b[1],b[0]-a[0]))
    halfway = len(path)//2
    retail_surface = mission.cache == 'b30' and part == 2
    entries = [entity(tuple(map(round, projected_start)) if retail_surface else xyz(path[0]), PLAYERSTART,
                      900 if retail_surface else facing(path[0],path[min(5,len(path)-1)]), 1),
               entity(xyz(path[halfway]), PLAYERSTART, facing(path[halfway],path[min(halfway+5,len(path)-1)]),1),
               entity(tuple(map(round, projected_end)) if retail_surface else xyz(path[-1]), PLAYERSTART, 0,100)]
    for i in range(10, len(path)-4, 11):
        phase = 1 if i < len(path)//4 else 2 if i < halfway-3 else 3
        entries.append(entity(xyz(path[i]), PLAYERSTART,0,100,phase))
    for i in (0,halfway):
        for kind in (AMMO, HEALTH, GRENADE):
            spot = path[min(len(path)-1,i+kind*2)]
            entries.append(entity(xyz(spot),kind,0))
    used = []
    for wave, anchor in enumerate((path[len(path)//4],path[3*len(path)//4])):
        eligible = []
        for p, actor in actors:
            point = projection.point(p)
            if math.dist(point[:2],anchor) > 55:
                continue
            # Projection from an authored actor onto a nearby reachable cell.
            close = [(x,y) for x in range(round(point[0])-6,round(point[0])+7)
                           for y in range(round(point[1])-6,round(point[1])+7) if (x,y) in parents]
            if not close:
                continue
            place = closest(close,floor,point)
            if abs(floor[place]/4-point[2]) > 8 or math.dist(place,anchor)>35:
                continue
            eligible.append((math.dist(place,anchor),place,actor,p[3]))
        for _, place, actor, angle in sorted(eligible):
            if any(math.dist(place,p) < 5 for p in used):
                continue
            entries.append(entity(xyz(place),PLAYERSTART,ac_yaw(angle),0,wave,actor))
            used.append(place)
            if sum(e[4]==PLAYERSTART and e[5]==0 and e[6]==wave for e in entries) >= 4:
                break
    for p in (path[0],path[halfway],path[-1]):
        x,y,z = xyz(p)
        entries.append(entity((x,y,z+5),LIGHT,19,165,170,182))
    name = f'halo_{mission.cache}_{part+1:02}'
    output = output / 'packages'
    maps = output / 'maps'
    maps.mkdir(parents=True, exist_ok=True)
    (maps / f'{name}.cgz').write_bytes(encode_map(native_cubes(floor,ceilings,size),entries,factor,
                                                  f'{mission.title} | Section {part+1}'.encode()))
    (maps / f'{name}.cfg').write_text('fog 360\nfogcolour 0x617181\n')
    write_world_mesh(halo,output,projection.xy,projection.z,(xmin-8,xmax+8,ymin-8,ymax+8),
                     map_name=name,bsp=bsp,include_sky=not indoor)
    if mission.cache == 'b30' and part < 2:
        count = write_structure_collision(halo, bsp, projection, maps / f'{name}.hcol')
        print(f'{name} BSP {bsp}: {count} retail collision surfaces', flush=True)
    print(f'{name} BSP {bsp}: {len(path)} steps, {len(used)} actors, {len(parents)} cells; '
          f'entry offset {math.dist(xyz(path[0]),projected_start)/scale:.1f}, '
          f'exit offset {math.dist(xyz(path[-1]),projected_end)/scale:.1f} source units',flush=True)


def convert(args):
    source, output, mission, part = args
    with (source / f'{mission.cache}.map').open('rb') as file, mmap.mmap(file.fileno(),0,access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        if halo.name != mission.cache:
            raise ValueError(f'Wrong cache in {mission.cache}.map')
        try:
            make_section(halo,mission,part,output)
        except (ValueError, IndexError) as error:
            return f'{mission.cache} section {part+1}: {error}'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps',type=Path,help='Directory with the ten expanded Xbox campaign map caches')
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--mission',choices=[m.cache for m in MISSIONS])
    parser.add_argument('--part',type=int,help='One-based section number, used with --mission')
    args = parser.parse_args()
    jobs = [(args.maps,args.output,m,i) for m in MISSIONS if not args.mission or m.cache==args.mission
            for i in range(len(m.sections)) if args.part is None or i+1==args.part]
    if not jobs or (args.part is not None and not args.mission):
        parser.error('Choose a valid mission and section')
    with ProcessPoolExecutor(max_workers=min(len(jobs),4)) as pool:
        failures = [error for error in pool.map(convert,jobs) if error]
    for failure in failures:
        print(failure,flush=True)
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
