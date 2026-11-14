import sys
from pathlib import Path as _P
sys.path.insert(0, str(_P(__file__).resolve().parents[1]))
from qa_common import *  # noqa: E402,F403
from collections import deque
from pathlib import Path
import math
import mmap
import struct

from import_halo_a30 import HaloMap
from import_halo_campaign import MISSIONS, section_layout
from import_halo_scripts import Program, model_pack_scale, named_objects
from halo_devices import device_rows


def collision_points(data, bsp_region, collision, projection, spacing):
    nodes = [bsp_region.read('3i', p) for p in bsp_region.block(collision, 12)]
    planes = [bsp_region.read('4f', p) for p in bsp_region.block(collision + 12, 16)]
    def is_open(acx, acy, acz):
        x = (acx - 9) / projection.scale + projection.x0
        y = projection.y0 - (acy - 9) / projection.scale
        z = projection.z0 + acz / projection.scale
        node = 0
        while node >= 0:
            plane, back, front = nodes[node]
            a, b, c, d = planes[plane]
            node = front if a*x + b*y + c*z >= d else back
        return node < -1

    def can_stand(x, y, z):
        # Player has 1.1 AC radius and 5.2 AC standing height. Check center
        # and eight radial points at mid-height and head-height.
        if not is_open(x, y, z + .8):
            return False
        for level in (1.5, 5.1):
            for dx, dy in ((0,0),(1.1,0),(-1.1,0),(0,1.1),(0,-1.1),(.78,.78),(-.78,.78),(.78,-.78),(-.78,-.78)):
                if not is_open(x+dx, y+dy, z+level):
                    return False
        return True

    # Use the actual exported polygons for candidate floors and classify clearance
    # with the original BSP planes. Coordinates are AC units.
    _, count = struct.unpack_from('<4sI', data)
    cursor = 8
    samples = {}
    for _ in range(count):
        nx, ny, nz, flags, breakable, material, size = struct.unpack_from('<3fBBhI', data, cursor)
        cursor += 20
        points = [struct.unpack_from('<3f', data, cursor + 12 * i) for i in range(size)]
        cursor += 12 * size
        if nz < .65:
            continue
        xs, ys, _ = zip(*points)
        x0, y0, z0 = points[0]
        for x in range(math.ceil(min(xs)/spacing)*spacing, math.floor(max(xs)/spacing)*spacing+1, spacing):
            for y in range(math.ceil(min(ys)/spacing)*spacing, math.floor(max(ys)/spacing)*spacing+1, spacing):
                if any((b[0]-a[0])*(y-a[1])-(b[1]-a[1])*(x-a[0]) < -0.002
                       for a,b in zip(points,points[1:]+points[:1])):
                    continue
                z = round(4 * (z0 - ((x-x0)*nx + (y-y0)*ny)/nz)) / 4
                if can_stand(x,y,z):
                    samples.setdefault((x,y),set()).add(z)
    assert cursor == len(data)
    return samples, can_stand, is_open


def path_to(samples, can_stand, start, target, spacing):
    points = ((x,y,z) for (x,y), values in samples.items() for z in values)
    start_node = min(points, key=lambda point: math.dist(start,point))
    parents = {start_node:None}
    queue = deque([start_node])
    while queue:
        point = queue.popleft()
        x,y,z = point
        for dx,dy in ((spacing,0),(-spacing,0),(0,spacing),(0,-spacing)):
            for n in samples.get((x+dx,y+dy),()):
                next_node = x+dx,y+dy,n
                if next_node in parents or abs(n-z) > 2.5:
                    continue
                if not can_stand(x+dx/2,y+dy/2,(n+z)/2):
                    continue
                parents[next_node] = point
                queue.append(next_node)
    end = min(parents, key=lambda point: math.dist(target,point))
    result = []
    point = end
    while point is not None:
        result.append(point)
        point = parents[point]
    return result[::-1], len(parents)


def simplify_route(path, can_stand, is_open, can_pass_door):
    def can_walk(a,b):
        dx,dy,dz = (q-p for p,q in zip(a,b))
        horizontal = math.hypot(dx,dy)
        if abs(dz) > 1.25 * horizontal or horizontal > 60:
            return False
        steps = max(4, math.ceil(horizontal/.9))
        for i in range(1,steps):
            t = i / steps
            x,y,z = a[0]+t*dx,a[1]+t*dy,a[2]+t*dz
            # Ground can deviate slightly from the straight line. Confirm
            # standing clearance and support on the same connected tier.
            if not any(can_stand(x,y,z+offset) and can_pass_door(x,y,z+offset) and
                       not is_open(x,y,z+offset-.6) and is_open(x,y,z+offset+.4)
                       for offset in (0,-.5,.5,-1,1)):
                return False
        return True

    result = [path[0]]
    i = 0
    while i < len(path)-1:
        last = min(len(path)-1, i+50)
        while last > i+1 and not can_walk(path[i],path[last]):
            last -= 1
        i = last
        result.append(path[i])
    return result


def volume_intersections(path, halo, projection):
    result = {}
    for volume in halo.tags.block(halo.scenario + 864, 96):
        name = halo.tags.name(volume+4,32).lower()
        if not name.startswith('shafta'):
            continue
        kind, = halo.tags.read('H',volume)
        if kind != 1:
            continue
        fx,fy,fz,ux,uy,uz,ox,oy,oz,w,d,h = halo.tags.read('12f',volume+48)
        sx,sy,sz = uy*fz-uz*fy,uz*fx-ux*fz,ux*fy-uy*fx
        indices = []
        for i,(acx,acy,acz) in enumerate(path):
            x = (acx - 9) / projection.scale + projection.x0 - ox
            y = projection.y0 - (acy - 9) / projection.scale - oy
            z = projection.z0 + (acz + 4.5 * .55) / projection.scale - oz
            if 0 < x*fx+y*fy+z*fz < w and 0 < x*sx+y*sy+z*sz < d and 0 < x*ux+y*uy+z*uz < h:
                indices.append(i)
        if indices:
            result[name] = (indices[0],indices[-1])
    return result


def lock_door_clearance(halo, projection, object_index, opened):
    rows = device_rows(halo, Path.cwd(), False, model_pack_scale)
    row = next(d for d in rows if d[0] == object_index)
    pose, panels = row[-2:]
    ox,oy,oz,yaw,*_ = pose
    c,s = math.cos(yaw), math.sin(yaw)
    bounds = [frames[-1 if opened else 0] for _,frames in panels]
    radius = 1.1 / projection.scale
    height = 5.2 / projection.scale
    def clear(acx,acy,acz):
        x = (acx - 9) / projection.scale + projection.x0 - ox
        y = projection.y0 - (acy - 9) / projection.scale - oy
        z = projection.z0 + acz / projection.scale - oz
        x,y = x*c+y*s,-x*s+y*c
        return all(not (x1-radius < x < x2+radius and y1-radius < y < y2+radius and z < z2 and z+height > z1)
                   for x1,y1,z1,x2,y2,z2 in bounds)
    return clear


if __name__ == '__main__':
    source = HALO_SOURCE / 'b30.map'
    data = root / 'packages/maps/halo_b30_02.hcol'.read_bytes()
    with source.open('rb') as stream, mmap.mmap(stream.fileno(),0,access=mmap.ACCESS_READ) as cache:
        halo = HaloMap(cache)
        mission = next(m for m in MISSIONS if m.cache=='b30')
        bsp, indoor, bounds, entry, _, _, _, projection = section_layout(halo,mission,1)
        region, address = halo.bsp(bsp)
        collision, = region.block(address + 176, 96)
        spacing = 2
        samples, can_stand, is_open = collision_points(data, region, collision, projection, spacing)
        objects = list(named_objects(Program(halo)))
        by_name = {name:pose for name,_,pose in objects}
        door = next(i for i, (name,_,_) in enumerate(objects) if name == 'shafta_lock_door')
        open_door = lock_door_clearance(halo, projection, door, True)
        closed_door = lock_door_clearance(halo, projection, door, False)
        targets = {'control': projection.point(by_name['shafta_map_control'][:3]),
                   'upper': (243,74,0.5), 'lower':(243,74,-40),
                   'door': projection.point(by_name['shafta_lock_door'][:3])}
        print('total XY columns',len(samples),'sample points',sum(map(len,samples.values())))
        for label,start in [('native', (235,56,-6)), ('volume center',projection.point(entry))]:
            for name,target in targets.items():
                path,reached = path_to(samples,can_stand,start,target,spacing)
                print(label,name,'start',path[0],'end',path[-1],'offset',round(math.dist(target,path[-1]),2),
                      'nodes',reached,'path steps',len(path),'bounds',[(min(v),max(v)) for v in zip(*path)])
                print(path[::max(1,len(path)//14)],path[-1])
                if label=='native' and name=='control':
                    (DATA / 'b30-collision-route-ac.txt').write_text('\n'.join(','.join(map(str,point)) for point in path)+'\n')
                    for name,clear in [('open',open_door),('closed',closed_door)]:
                        print('full route blocked at',name,'door:', [(i, point) for i,point in enumerate(path) if not clear(*point)])
                    for name,clear in [('open',open_door),('closed',closed_door)]:
                        clear_samples = {xy:[z for z in heights if clear(*xy,z)] for xy,heights in samples.items()}
                        clear_path, reached = path_to(clear_samples,lambda x,y,z: can_stand(x,y,z) and clear(x,y,z),start,target,spacing)
                        print(name,'door route end',clear_path[-1],'offset',round(math.dist(target,clear_path[-1]),2),'steps',len(clear_path),'nodes',reached)
                        if name == 'open':
                            waypoints = simplify_route(clear_path,can_stand,is_open,clear)
                            print('simplified',len(waypoints), 'waypoints',waypoints)
                            (DATA / 'b30-collision-waypoints-ac.txt').write_text('\n'.join(','.join(map(str,point)) for point in waypoints)+'\n')
                            upper_path, _ = path_to(clear_samples, lambda x,y,z: can_stand(x,y,z) and clear(x,y,z),
                                                    start,(242,74,.5),spacing)
                            upper = simplify_route(upper_path,can_stand,is_open,clear)
                            tagged = [(point,'') for point in upper]
                            tagged[-1] = (tagged[-1][0], 'upper')
                            tagged.extend((point,'') for point in reversed(upper[:-1]))
                            for point in waypoints[1:]:
                                action = ('checkpoint' if point == (174,118,-39.75) else
                                          'suicide' if point == (216,88,-39.75) else '')
                                tagged.append((point,action))
                                if point == (240,74,-39.75):
                                    tagged.extend([((242,74,-39.75),'lower'),(point,'')])
                            (DATA / 'b30-collision-qa-route.txt').write_text('\n'.join(
                                ' '.join((*map(str,point),tag or '-')) for point,tag in tagged)+'\n')
                            print('trigger crossings', {label:(indices, clear_path[indices[0]],clear_path[indices[1]])
                                                        for label,indices in volume_intersections(clear_path,halo,projection).items()})
