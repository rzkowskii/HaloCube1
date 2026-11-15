"""Export the retail structure collision polygons in campaign coordinates."""

import math
import re
import struct
import tempfile
from pathlib import Path


def surface_vertices(surface, first_edge, edges, vertices):
    points = []
    edge = first_edge
    while True:
        if not 0 <= edge < len(edges) or len(points) == 8:
            raise ValueError(f'Invalid collision edge loop for surface {surface}')
        start, end, forward, reverse, left, right = edges[edge]
        if surface == left:
            vertex, edge = start, forward
        elif surface == right:
            vertex, edge = end, reverse
        else:
            raise ValueError(f'Collision edge does not bound surface {surface}')
        if not 0 <= vertex < len(vertices):
            raise ValueError(f'Invalid collision vertex for surface {surface}')
        points.append(vertices[vertex])
        if edge == first_edge:
            break
    if len(points) < 3:
        raise ValueError(f'Collision surface {surface} has fewer than three vertices')
    return points


def structure_surfaces(halo, bsp, projection):
    """The retail structure collision surfaces projected into campaign coordinates.

    Returns (surfaces, tree). Each surface is (normal, flags, breakable, material, points),
    with the points wound to agree with the normal. `tree` answers open-space queries
    against the original collision BSP planes.
    """
    region, address = halo.bsp(bsp)
    collision = region.block(address + 176, 96)
    if len(collision) != 1:
        raise ValueError(f'Expected one structure collision BSP, found {len(collision)}')
    base = collision[0]
    planes = [region.read('3f', p) for p in region.block(base + 12, 16)]
    surfaces = region.block(base + 60, 12)
    edges = [region.read('6i', p) for p in region.block(base + 72, 24)]
    vertices = [projection.point(region.read('3f', p)) for p in region.block(base + 84, 16)]
    if not surfaces or not all(math.isfinite(value) for point in vertices for value in point):
        raise ValueError('Missing collision surfaces or invalid collision coordinates')

    result = []
    for index, surface in enumerate(surfaces):
        designator, edge, flags, breakable, material = region.read('IiBBh', surface)
        plane = designator & 0x7fffffff
        if plane >= len(planes):
            raise ValueError(f'Invalid collision plane for surface {index}')
        x, y, z = planes[plane]
        length = math.hypot(x, y, z)
        if not math.isfinite(length) or length == 0:
            raise ValueError(f'Invalid collision normal for surface {index}')
        sign = -1 if designator & 0x80000000 else 1
        points = surface_vertices(index, edge, edges, vertices)
        # Campaign projection reflects Y. Reverse the edge loop so its winding
        # still agrees with the outward collision normal.
        result.append(((sign*x/length, -sign*y/length, sign*z/length), flags, breakable, material, points[::-1]))
    return result, CollisionTree(region, base, projection)


class CollisionTree:
    """Open-space and standing-clearance queries on the original collision BSP.

    Coordinates are campaign (AssaultCube) units, converted back to retail space.
    """

    # The player is 1.1 units wide in each direction and 5.2 tall. Routes keep 0.3 units of
    # clearance beyond that, since a walker steering along them drifts off the centre line.
    RING = ((0, 0), (1.4, 0), (-1.4, 0), (0, 1.4), (0, -1.4),
            (1.0, 1.0), (-1.0, 1.0), (1.0, -1.0), (-1.0, -1.0))

    def __init__(self, region, base, projection):
        self.projection = projection
        self.nodes = [region.read('3i', p) for p in region.block(base, 12)]
        self.planes = [region.read('4f', p) for p in region.block(base + 12, 16)]

    def is_open(self, acx, acy, acz):
        p = self.projection
        x = (acx - 9) / p.scale + p.x0
        y = p.y0 - (acy - 9) / p.scale
        z = p.z0 + acz / p.scale
        nodes, planes = self.nodes, self.planes
        node = 0
        while node >= 0:
            plane, back, front = nodes[node]
            a, b, c, d = planes[plane]
            node = front if a*x + b*y + c*z >= d else back
        return node < -1

    def can_stand(self, x, y, z, top=5.1):
        if not self.is_open(x, y, z + .8):
            return False
        for level in (1.5, top):
            for dx, dy in self.RING:
                if not self.is_open(x + dx, y + dy, z + level):
                    return False
        return True

    # A crouching player is 3/4 of the standing eye height (4.5) plus 0.7 above the eye.
    CROUCH_TOP = 4.5 * .75 + .7 - .15

    def can_crouch(self, x, y, z):
        return self.can_stand(x, y, z, self.CROUCH_TOP)


def floor_samples(surfaces, tree, size, minimum_normal=.65, crouch=None):
    """Standing floor heights at each campaign grid point, from the collision polygons.

    Returns {(x, y): sorted heights in quarter units} for every grid point whose floor
    polygon has room for the player above it. When `crouch` is a set, floors with room only
    for a crouching player are included too and added to it as (x, y, z).
    """
    samples = {}
    for normal, _, _, _, points in surfaces:
        if normal[2] < minimum_normal:
            continue
        nx, ny, nz = normal
        x0, y0, z0 = points[0]
        low_x = max(6, math.ceil(min(p[0] for p in points)))
        high_x = min(size - 7, math.floor(max(p[0] for p in points)))
        low_y = max(6, math.ceil(min(p[1] for p in points)))
        high_y = min(size - 7, math.floor(max(p[1] for p in points)))
        if low_x > high_x or low_y > high_y:
            continue
        # The polygon is convex: a grid point is inside when it is on the same side of every edge.
        edges = []
        for a, b in zip(points, points[1:] + points[:1]):
            edges.append((b[1] - a[1], a[0] - b[0], b[0]*a[1] - a[0]*b[1]))
        centroid_x = sum(p[0] for p in points) / len(points)
        centroid_y = sum(p[1] for p in points) / len(points)
        flip = -1 if sum(e[0]*centroid_x + e[1]*centroid_y + e[2] for e in edges) < 0 else 1
        for x in range(low_x, high_x + 1):
            for y in range(low_y, high_y + 1):
                if any(flip*(e[0]*x + e[1]*y + e[2]) < -1e-6 for e in edges):
                    continue
                z = round(4 * (z0 - ((x - x0)*nx + (y - y0)*ny) / nz))
                if tree.can_stand(x, y, z / 4):
                    samples.setdefault((x, y), set()).add(z)
                elif crouch is not None and tree.can_crouch(x, y, z / 4):
                    samples.setdefault((x, y), set()).add(z)
                    crouch.add((x, y, z))
    return {point: sorted(levels) for point, levels in samples.items()}


def _model_polygons(halo, tag):
    """Object-space collision polygons of a machine tag in its first animation pose."""
    from halo_devices import animation
    from import_halo_models import Model
    tags = halo.tags
    model_ref = halo.reference(tags, tag.address + 40, 'mode')
    coll = halo.reference(tags, tag.address + 112, 'coll')
    if not model_ref or not coll:
        return []
    model = Model(halo, model_ref.name, 'part')
    pose = animation(halo, tag, model)[0]
    result = []
    for index, node in enumerate(tags.block(coll.address + 652, 64)):
        if index >= len(model.nodes):
            break
        transform = pose[model.nodes[index][0]]
        for bsp in tags.block(node + 52, 96):
            planes = [tags.read('3f', q) for q in tags.block(bsp + 12, 16)]
            edges = [tags.read('6i', q) for q in tags.block(bsp + 72, 24)]
            vertices = [transform.point(tags.read('3f', q)) for q in tags.block(bsp + 84, 16)]
            for number, surface in enumerate(tags.block(bsp + 60, 12)):
                designator, edge, flags, breakable, material = tags.read('IiBBh', surface)
                plane = designator & 0x7fffffff
                if plane >= len(planes):
                    continue
                sign = -1 if designator & 0x80000000 else 1
                normal = transform.vector(tuple(sign * v for v in planes[plane]))
                try:
                    points = surface_vertices(number, edge, edges, vertices)
                except ValueError:
                    continue
                result.append((normal, flags, breakable, material, points))
    return result


def static_device_surfaces(halo, bsp, projection, output, still=()):
    """Collision polygons of the scenario machines that never move, in campaign coordinates.

    A machine whose collision boxes are the same in every animation frame (a bulkhead frame,
    a console, a floor hatch) is part of the room: its real polygons go into the section's
    .hcol and the engine stops colliding with its coarse box. Returns (surfaces, device indices)
    with device indices in the engine's (packages/halo/<map>.hscb) order. A machine that a
    script creates is baked from the start; one outside this BSP keeps the engine's box.
    `still` names machines that move only during a cinematic (c20's index_platform, lowered
    in the extraction scene): they are baked in their first pose.
    """
    from halo_devices import device_rows
    from import_halo_scripts import model_pack_scale
    tags, scenario = halo.tags, halo.scenario
    # The same placement walk as device_rows, so the tags line up with its rows.
    placements = []
    for placement_block, palette_block, group in ((660, 672, 'mach'), (684, 696, 'ctrl')):
        palette = tags.block(scenario + palette_block, 48)
        for placement in tags.block(scenario + placement_block, 64):
            variant, _ = tags.read('2h', placement)
            if not 0 <= variant < len(palette):
                continue
            tag = halo.reference(tags, palette[variant], group)
            if tag:
                placements.append(tag if group == 'mach' else None)
    polygons, surfaces, baked = {}, [], []
    from import_halo_scripts import named_objects, Program
    program = Program(halo)
    names = {index: name for index, (name, _, _) in enumerate(named_objects(program))}
    removed = removed_devices(program)
    for device, (tag, row) in enumerate(zip(placements, device_rows(halo, output, False, model_pack_scale))):
        not_placed, bsp_mask, control, kind = row[6], row[8], row[9], row[10]
        pose, panels = row[-2], row[-1]
        name = names.get(row[0], '')
        # A machine that is not placed automatically counts once a script names it: a10's
        # jammed bulkheads are created by x10_post when the mission starts.
        scripted = name and re.search(rb'\b' + re.escape(name.encode()) + rb'\b', program.strings)
        if tag is None or control or not panels or (not_placed & 1 and not scripted) or not bsp_mask >> bsp & 1:
            continue
        # A machine a script takes away (a50's cell doors, switched to their door_off permutation
        # for the rescue) keeps the engine's removable box instead of polygons in the .hcol.
        if name and removed(name):
            continue
        if name not in still and \
                any(max(abs(a - b) for a, b in zip(frame, frames[0])) > 1e-3 for _, frames in panels for frame in frames):
            continue
        if tag.address not in polygons:
            polygons[tag.address] = _model_polygons(halo, tag)
        if not polygons[tag.address]:
            continue
        ox, oy, oz, yaw = pose[:4]
        cosine, sine = math.cos(yaw), math.sin(yaw)
        def place(v):
            return projection.point((v[0]*cosine - v[1]*sine + ox, v[0]*sine + v[1]*cosine + oy, v[2] + oz))
        for normal, flags, breakable, material, points in polygons[tag.address]:
            nx, ny, nz = normal[0]*cosine - normal[1]*sine, normal[0]*sine + normal[1]*cosine, normal[2]
            length = math.hypot(nx, ny, nz)
            if not length:
                continue
            # Campaign projection reflects Y: flip the normal's Y and reverse the winding.
            surfaces.append(((nx/length, -ny/length, nz/length), flags & 15, breakable, material,
                             [place(v) for v in points[::-1]]))
        baked.append(device)
    return surfaces, baked


def write_structure_collision(halo, bsp, projection, destination, machines=((), ())):
    """Write the section's .hcol; `machines` is static_device_surfaces' (surfaces, devices)."""
    surfaces, _ = structure_surfaces(halo, bsp, projection)
    extra, baked = machines
    surfaces = surfaces + list(extra)
    # HCL1 stores little-endian surface records followed by their boundary vertices. An
    # optional DEV1 trailer lists the scenario devices whose polygons are included.
    data = bytearray(struct.pack('<4sI', b'HCL1', len(surfaces)))
    for normal, flags, breakable, material, points in surfaces:
        data.extend(struct.pack('<3fBBhI', *normal, flags, breakable, material, len(points)))
        for point in points:
            data.extend(struct.pack('<3f', *point))
    if baked:
        data.extend(struct.pack(f'<4sI{len(baked)}I', b'DEV1', len(baked), *baked))

    with tempfile.TemporaryDirectory(dir=destination.parent) as directory:
        temporary = Path(directory) / destination.name
        temporary.write_bytes(data)
        temporary.replace(destination)
    return len(surfaces)


def opened_devices(program):
    """Names of the devices some script powers on or moves to a non-zero position.

    A door a script only ever closes or switches off (a10's tunnel_door_1, closed behind the
    player) never opens, whatever the scripts mention it for.
    """
    import struct as packing
    opened = set()
    setters = {'device_set_position', 'device_set_position_immediate', 'device_set_power'}
    for salt, _, _, flags, _, _, data in program.nodes:
        if not salt or flags & 1:
            continue
        try:
            head = program.node(data)
            if program.text(head[5]) not in setters:
                continue
            target = program.node(head[4])
            value = program.node(target[4])
        except ValueError:
            continue
        # A literal real (opcode 6) of zero closes or powers off; anything else may open.
        if value[3] & 1 and value[1] == 6 and packing.unpack('<f', packing.pack('<I', value[6]))[0] == 0:
            continue
        opened.add(program.text(target[5]))
    return opened


def moved_devices(program):
    """Names of the devices some script moves to any position (c10's mission_swamp_a brings
    lift_a up to the landing with (device_set_position lift_a 0.0))."""
    moved = set()
    setters = {'device_set_position', 'device_set_position_immediate'}
    for salt, _, _, flags, _, _, data in program.nodes:
        if not salt or flags & 1:
            continue
        try:
            head = program.node(data)
            if program.text(head[5]) in setters:
                moved.add(program.text(program.node(head[4])[5]))
        except ValueError:
            continue
    return moved


def opened_groups(program):
    """Names of the device groups some script sets to a non-zero position.

    c20's floor doors are driven only through their position groups
    ((device_group_set floor1_door1 door_open)), never by device name.
    """
    import struct as packing
    opened = set()
    setters = {'device_group_set', 'device_group_set_immediate'}
    for salt, _, _, flags, _, _, data in program.nodes:
        if not salt or flags & 1:
            continue
        try:
            head = program.node(data)
            if program.text(head[5]) not in setters:
                continue
            target = program.node(head[4])
            value = program.node(target[4])
        except ValueError:
            continue
        # A literal real zero closes; a global (door_open) or any other value may open.
        if value[3] & 1 and not value[3] & 4 and value[1] == 6 and \
                packing.unpack('<f', packing.pack('<I', value[6]))[0] == 0:
            continue
        opened.add(program.text(target[5]))
    return opened


def cleared_one_sided(program):
    """Names of the devices some script makes two-sided: (device_one_sided_set <name> false)."""
    names = set()
    for salt, _, _, flags, _, _, data in program.nodes:
        if not salt or flags & 1:
            continue
        try:
            head = program.node(data)
            if program.text(head[5]) != 'device_one_sided_set':
                continue
            target = program.node(head[4])
            value = program.node(target[4])
        except ValueError:
            continue
        # A literal boolean (opcode 5) false.
        if value[3] & 1 and value[1] == 5 and not value[6] & 0xff:
            names.add(program.text(target[5]))
    return names


def destroyed_devices(program):
    """Names of the objects some script destroys with (object_destroy <name>)."""
    names = set()
    for salt, _, _, flags, _, _, data in program.nodes:
        if not salt or flags & 1:
            continue
        try:
            head = program.node(data)
            if program.text(head[5]) != 'object_destroy':
                continue
            target = program.node(head[4])
        except ValueError:
            continue
        if target[3] & 1:
            names.add(program.text(target[5]))
    return names


def removed_devices(program):
    """Predicate for the device names a script switches to a "door_off" permutation.

    The engine treats that as removing the door and its collision (a50's cell doors). Doors a
    script destroys are not included: a10's baked lifepod machines are, and its validated
    collision is kept as it is.
    """
    names = set()
    for salt, _, _, flags, _, _, data in program.nodes:
        if not salt or flags & 1:
            continue
        try:
            head = program.node(data)
            if program.text(head[5]) != 'object_set_permutation':
                continue
            # (object_set_permutation <object> <region> <permutation>)
            target = program.node(head[4])
            permutation = program.node(program.node(target[4])[4])
        except ValueError:
            continue
        if target[3] & 1 and program.text(permutation[5]).startswith('door_off'):
            names.add(program.text(target[5]))
    return names.__contains__


def floor_leaf(frame):
    """Whether a door panel box (x1, y1, z1, x2, y2, z2, relative to the door's origin) lies flat
    on the floor: c10's interior doors fold their leaves down into the doorway when open, 0.05 to
    0.23 units proud of the floor, and players walk over them in Halo. The engine
    (Scenario::collideDevices, kDoorLeafTop and kDoorLeafThickness) lets bodies walk over such a
    panel of a door (device kind 0); keep the two limits in step."""
    return frame[5] <= .25 and frame[5] - frame[2] <= .3


class DoorBlocks:
    """Scenario doors and lifts that stand in the player's way, in campaign coordinates.

    A door that is open, or that the player's own approach opens (automatic and powered at the
    start), blocks only with its panels in the open pose; its closed pose is "closable". A closed device that no script mentions never opens, so it is solid ("sealed"). A
    closed door that a script operates, or an unnamed door, is "scripted": a route may pass it
    by waiting for it to open. With `ungrouped_sealed`, an unnamed door with neither a power nor a
    position group is sealed instead: no script names it and no control or group drives it.
    """

    def __init__(self, halo, projection, output, skip=(), never_open=(), ungrouped_sealed=False, floor_leaves=False):
        from halo_devices import device_rows
        from import_halo_scripts import model_pack_scale, named_objects, Program
        self.projection = projection
        program = Program(halo)
        strings = program.strings
        names = {index: name for index, (name, _, _) in enumerate(named_objects(program))}
        opened_by_script = opened_devices(program)
        two_sided_by_script = cleared_one_sided(program)
        destroyed = destroyed_devices(program)
        group_rows = halo.tags.block(halo.scenario + 648, 52)
        groups = [halo.tags.read('f', p + 32)[0] for p in group_rows]
        group_names = [halo.tags.name(p, 32) for p in group_rows]
        opened_by_group = opened_groups(program)
        self.sealed, self.scripted, self.closable, self.opened = {}, {}, {}, {}
        # Closed automatic doors that open only for a player on their back side (local x <= 0,
        # as the engine's updateDevices tests): a50's gravity room, muster bay and hangar exits,
        # which the scripts make two-sided once a fight is over.
        self.one_sided, self.reopened = {}, set()
        # Lift platforms (b40's lift_a to lift_d): a body may stand on one, as the engine's
        # device collision lets it, but not walk through it.
        self.platforms = {}
        self.names = {'sealed': [], 'scripted': [], 'open': [], 'lift': []}
        scale = projection.scale
        skip = set(skip)
        for device, row in enumerate(device_rows(halo, output, False, model_pack_scale)):
            if device in skip:
                # Baked into the section's .hcol: the route sees its real polygons instead.
                continue
            (index, power, position, _, flags, attributes, not_placed, _, _, control, kind, *_), pose, panels = row[:11], row[-2], row[-1]
            name = names.get(index, '')
            mentioned = name and re.search(rb'\b' + re.escape(name.encode()) + rb'\b', strings)
            # A device that is not placed automatically exists once a script creates it.
            if control or kind == 2 or not panels or (not_placed & 1 and not mentioned):
                continue
            initial_power = groups[power] if 0 <= power < len(groups) else (0 if flags & 2 else 1)
            opened = (groups[position] if 0 <= position < len(groups) else (1 if flags & 1 else 0)) > 0
            # Only kind 0 doors open by themselves when approached.
            automatic = kind == 0 and not attributes & 1
            label = name or f'#kind{kind}/attr{attributes}/power{power}/flags{flags}/pos{position}'
            # A lift is a thin slab that travels a storey or more; doors that rise are tall panels
            # moving a few units. c10's lifts are slabs exactly half a unit thick; c20's platforms
            # are a 0.4-unit deck over a 1.13-unit turntable, which rise together.
            lift = all(frames[0][5] - frames[0][2] < 1.2 and abs(frames[-1][2] - frames[0][2]) > 4
                       for _, frames in panels)
            if lift:
                poses = ((self.platforms, -1 if opened else 0),)
                self.names['lift'].append(label)
            elif opened or (automatic and initial_power > 0):
                # An open door still collides with its panels in their open pose (a door that
                # slides into its frame, a hatch folded to one side): keep the route clear of them.
                # Its closed pose is kept too: a script may shut it behind the player.
                poses = ((self.opened, -1), (self.closable, 0))
                self.names['open'].append(label)
                if automatic and not opened and attributes & 2:
                    poses += ((self.one_sided, 0),)
                    if name in two_sided_by_script or name in opened_by_script:
                        self.reopened.add(label)
            else:
                # A named device opens only if a script opens it; unnamed props never do. An
                # unnamed door may still be driven through its group, so it counts as scripted.
                # A door a script destroys is gone once it has run (c10's post_helmet_door_3,
                # replaced by a bashed-in door after the x50 scene): wait for it like an opening.
                # c20's floor doors open through their position group, never by name.
                by_group = 0 <= position < len(group_names) and group_names[position] in opened_by_group
                grouped = 0 <= power < len(groups) or 0 <= position < len(groups)
                opens = (name in opened_by_script or name in destroyed or by_group) if name else \
                    kind == 0 and (grouped or not ungrouped_sealed) or by_group
                bucket = self.scripted if opens and name not in never_open else self.sealed
                poses = ((bucket, 0),)
                self.names['scripted' if bucket is self.scripted else 'sealed'].append(label)
            ox, oy, oz, yaw = pose[:4]
            cosine, sine = math.cos(yaw), math.sin(yaw)
            for (bucket, frame), (_, frames) in ((pose_, panel) for pose_ in poses for panel in panels):
                x1, y1, z1, x2, y2, z2 = frames[frame]
                if floor_leaves and kind == 0 and floor_leaf(frames[frame]):
                    # A door leaf folded flat on the floor in this pose: bodies walk over it
                    # (Scenario::collideDevices skips it), as Halo's biped does.
                    continue
                # Axis-aligned extent of the rotated box in campaign cells.
                corners = []
                for lx in (x1, x2):
                    for ly in (y1, y2):
                        rx, ry = lx*cosine - ly*sine + ox, lx*sine + ly*cosine + oy
                        corners.append(((rx - projection.x0)*scale + 9, (projection.y0 - ry)*scale + 9))
                low_x, high_x = min(c[0] for c in corners) - 3, max(c[0] for c in corners) + 3
                low_y, high_y = min(c[1] for c in corners) - 3, max(c[1] for c in corners) + 3
                entry = ((ox, oy, oz, cosine, sine), (x1, y1, z1, x2, y2, z2), label)
                for gx in range(int(low_x) >> 4, (int(high_x) >> 4) + 1):
                    for gy in range(int(low_y) >> 4, (int(high_y) >> 4) + 1):
                        bucket.setdefault((gx, gy), []).append(entry)

    def _hits(self, bucket, x, y, z, step=0.0):
        panels = bucket.get((int(x) >> 4, int(y) >> 4))
        if not panels:
            return None
        p = self.projection
        # The body's half-width plus the walker's drift off the cell centre, as for the BSP RING.
        radius, height = (1.1 + .3) / p.scale, 5.2 / p.scale
        rx = (x - 9) / p.scale + p.x0
        ry = p.y0 - (y - 9) / p.scale
        rz = p.z0 + z / p.scale
        for (ox, oy, oz, c, s), (x1, y1, z1, x2, y2, z2), name in panels:
            dx, dy, dz = rx - ox, ry - oy, rz - oz
            lx, ly = dx*c + dy*s, -dx*s + dy*c
            # `step` (retail units) lets a body stand on a panel whose top is that close to its
            # feet, as the engine's device collision does.
            if x1 - radius < lx < x2 + radius and y1 - radius < ly < y2 + radius and dz + step < z2 and dz + height > z1:
                return name
        return None

    def one_sided_crossing(self, here, node):
        """A closed one-sided door that a step from `here` into `node` (x, y, quarter-unit z)
        walks into from its front, where it does not open: (label, a script makes it two-sided
        or opens it). None for any other step.
        """
        entering = self._hits(self.one_sided, node[0], node[1], node[2] / 4)
        if entering is None or self._hits(self.one_sided, here[0], here[1], here[2] / 4) == entering:
            return None
        p = self.projection
        rx = (here[0] - 9) / p.scale + p.x0
        ry = p.y0 - (here[1] - 9) / p.scale
        for (ox, oy, _, c, s), _, label in self.one_sided[int(node[0]) >> 4, int(node[1]) >> 4]:
            if label == entering:
                front = (rx - ox)*c + (ry - oy)*s > 0
                return (label, label in self.reopened) if front else None
        return None

    def sealed_except(self, x, y, z4, name):
        """Like sealed_at, but a device named `name` (one a step has moved out of the way: d40's
        opened piston) does not count."""
        panels = self.sealed.get((int(x) >> 4, int(y) >> 4))
        if not panels or all(label == name for *_, label in panels):
            return self._hits(self.platforms, x, y, z4 / 4, .23)
        kept = {key: [entry for entry in entries if entry[2] != name] for key, entries in self.sealed.items()
                if key == (int(x) >> 4, int(y) >> 4)}
        return self._hits(kept, x, y, z4 / 4) or self._hits(self.platforms, x, y, z4 / 4, .23)

    def sealed_at(self, x, y, z4):
        # The engine stands a body on a device top within 0.23 units of its feet.
        return self._hits(self.sealed, x, y, z4 / 4) or self._hits(self.platforms, x, y, z4 / 4, .23)

    def scripted_at(self, x, y, z4):
        return self._hits(self.scripted, x, y, z4 / 4)

    def opened_at(self, x, y, z4):
        """An open door's panels in their open pose."""
        return self._hits(self.opened, x, y, z4 / 4, .2)

    def closable_at(self, x, y, z4):
        """An open door whose closed panels would stand here."""
        return self._hits(self.closable, x, y, z4 / 4)


def _device_table(halo, output):
    """(name, row) for every scenario machine and control, in the engine's device order, the
    scripts and the device group names and initial values."""
    from halo_devices import device_rows
    from import_halo_scripts import model_pack_scale, named_objects, Program
    program = Program(halo)
    names = {index: name for index, (name, _, _) in enumerate(named_objects(program))}
    group_rows = halo.tags.block(halo.scenario + 648, 52)
    groups = [halo.tags.read('f', p + 32)[0] for p in group_rows]
    group_names = [halo.tags.name(p, 32) for p in group_rows]
    rows = [(names.get(row[0], ''), row) for row in device_rows(halo, output, False, model_pack_scale)]
    return rows, program, groups, group_names


def _frame_position(frame, frames, flags):
    """The device position (0 to 1) whose animation frame is `frame`, as the engine's
    deviceFrame maps it (flag 8 reverses the position)."""
    value = frame / max(frames - 1, 1)
    return 1 - value if flags & 8 else value


class LightBridges:
    """c10's light bridges: flat machine panels that slide out over a pit when their position is
    set (bridge_a to bridge_e, extended from their control panels, or by a script: flicker_bridge
    moves bridge_c). The engine collides with each panel's box in its current pose, so an
    extended bridge is floor; a retracted one leaves the pit open.

    The route planner walks a bridge's extended deck as floor (`add_floors`; `cells` maps each
    deck cell, x, y and quarter-unit z, to its bridge), and
    `entry(here, node)` prices stepping onto it from `here`: 0 for a bridge that starts extended,
    SCRIPTED_COST (a wait) for one a script extends or whose control is within reach of `here`,
    and None (no way on) for one whose controls are all elsewhere, such as on the far side.
    """

    def __init__(self, halo, projection, output, wait_cost, bsp):
        rows, program, groups, group_names = _device_table(halo, output)
        opened_by_script, opened_by_group = opened_devices(program), opened_groups(program)
        self.projection, self.wait_cost = projection, wait_cost
        self.bridges, self.cells = [], {}
        controls = {}
        for name, row in rows:
            # A control, by its position group; the engine's use skips controls with flag 16.
            if row[9] and not row[4] & 16:
                controls.setdefault(row[2], []).append((name, row[-2][:3]))
        for device, (name, row) in enumerate(rows):
            power, position, flags, control, kind = row[1], row[2], row[4], row[9], row[10]
            pose, panels = row[-2], row[-1]
            if control or kind == 2 or not panels or not row[8] >> bsp & 1:
                continue
            # A bridge: every panel a flat slab under half a unit thick that keeps its height,
            # and some panel slides more than a unit across.
            if not all(frames[0][5] - frames[0][2] < .5 and abs(frames[-1][2] - frames[0][2]) < .05 for _, frames in panels):
                continue
            if max(max(abs(frames[-1][i] - frames[0][i]) for i in (0, 1, 3, 4)) for _, frames in panels) <= 1:
                continue
            # Extended: the end pose whose panels join into the fewest separate strips.
            def strips(index):
                boxes = [frames[index] for _, frames in panels]
                parent = list(range(len(boxes)))
                def find(i):
                    while parent[i] != i:
                        i = parent[i]
                    return i
                for i, a in enumerate(boxes):
                    for j, b in enumerate(boxes[:i]):
                        if a[0] <= b[3] + .05 and b[0] <= a[3] + .05 and a[1] <= b[4] + .05 and b[1] <= a[4] + .05:
                            parent[find(i)] = find(j)
                return len({find(i) for i in range(len(boxes))})
            last = len(panels[0][1]) - 1
            on = last if strips(last) <= strips(0) else 0
            on_value = _frame_position(on, last + 1, flags)
            initial = groups[position] if 0 <= position < len(groups) else (1 if flags & 1 else 0)
            powered = groups[power] > 0 if 0 <= power < len(groups) else not flags & 2
            label = name or f'#{device}'
            bridge = {
                'name': label, 'device': device, 'value': on_value,
                'always': abs(initial - on_value) < .01 and powered,
                'scripted': name in opened_by_script or
                            (0 <= position < len(group_names) and group_names[position] in opened_by_group),
                'controls': [point for _, point in controls.get(position, [])] if position >= 0 else [],
            }
            self.bridges.append(bridge)
            ox, oy, oz, yaw = pose[:4]
            cosine, sine = math.cos(yaw), math.sin(yaw)
            quads = []
            for _, frames in panels:
                x1, y1, _, x2, y2, z2 = frames[on]
                corners = [(lx*cosine - ly*sine + ox, lx*sine + ly*cosine + oy, z2 + oz)
                           for lx, ly in ((x1, y1), (x2, y1), (x2, y2), (x1, y2))]
                # Campaign projection reflects Y: reverse the winding, as for the structure.
                quads.append(((0.0, 0.0, 1.0), 0, 0, -1, [projection.point(c) for c in corners[::-1]]))
            bridge['quads'] = quads

    def add_floors(self, samples, tree, size, walls=None):
        """Adds the extended decks to `samples` ({(x, y): levels}) and remembers their cells."""
        for index, bridge in enumerate(self.bridges):
            for xy, levels in floor_samples(bridge['quads'], tree, size).items():
                for z in levels:
                    if walls is not None and walls.blocked(xy[0], xy[1], z):
                        continue
                    self.cells[(xy[0], xy[1], z)] = index
                    if z not in samples.get(xy, ()):
                        samples[xy] = sorted(samples.get(xy, []) + [z])

    def control_near(self, bridge, cell):
        """Whether a control of the bridge is within the use reach of a body standing at `cell`."""
        p = self.projection
        for point in bridge['controls']:
            cx, cy, cz = p.point(point)
            if math.hypot(cx - cell[0], cy - cell[1]) <= 1.45 * p.scale and -1.5 * p.scale < cz - cell[2] / 4 < 3 * p.scale:
                return True
        return False

    def entry(self, here, node):
        index = self.cells.get(tuple(node))
        if index is None or tuple(here) in self.cells:
            return 0
        bridge = self.bridges[index]
        if bridge['always']:
            return 0
        if bridge['scripted'] or self.control_near(bridge, here):
            return self.wait_cost
        return None

    def at(self, cell):
        """The bridge whose extended deck holds a route cell (a Cell, or x, y, quarter-unit z), or None."""
        index = self.cells.get((cell[0], cell[1], cell.z if hasattr(cell, 'z') else cell[2]))
        return None if index is None else self.bridges[index]


def lift_ride(halo, projection, output, name, near):
    """Where a route boards the lift `name` at the landing nearest `near` (retail point).

    Returns (board, value, send, call): `board` the retail point on the deck where the body
    stands (beside the lift's control when one is within reach of the deck, else the deck's
    centre), `value` the lift position with the deck at that landing, `send` whether the ride
    starts from that control (use it once standing on the deck), and `call` whether the lift
    has to be called from the landing (no script moves it). None when there is no such lift.
    """
    rows, program, groups, _ = _device_table(halo, output)
    found = next((row for row_name, row in rows if row_name == name and not row[9]), None)
    if found is None or found[3] < 0:
        return None
    elevator, flags, position = found[3], found[4], found[2]
    pose, panels = found[-2], found[-1]
    deck = next((frames for node, frames in panels if node == elevator), None)
    if not deck:
        return None
    ox, oy, oz, yaw = pose[:4]
    ends = (0, len(deck) - 1)
    frame = min(ends, key=lambda f: abs(oz + deck[f][5] - near[2]))
    x1, y1, _, x2, y2, z2 = deck[frame]
    cosine, sine = math.cos(yaw), math.sin(yaw)
    margin = (1.1 + .3) / projection.scale
    best = None
    for control_name, row in rows:
        if not row[9] or row[2] != position or position < 0:
            continue
        cx, cy = row[-2][0] - ox, row[-2][1] - oy
        lx, ly = cx*cosine + cy*sine, -cx*sine + cy*cosine
        bx = min(max(lx, x1 + margin), x2 - margin)
        by = min(max(ly, y1 + margin), y2 - margin)
        reach = math.hypot(bx - lx, by - ly)
        if reach <= 1.2 and (best is None or reach < best[0]):
            best = (reach, bx, by)
    bx, by = (best[1], best[2]) if best else ((x1 + x2) / 2, (y1 + y2) / 2)
    board = (bx*cosine - by*sine + ox, bx*sine + by*cosine + oy, oz + z2)
    return board, _frame_position(frame, len(deck), flags), best is not None, name not in moved_devices(program)


class PolygonClearance:
    """Whether a standing player at a route cell would touch any of a set of wall polygons.

    Used for the static machines baked into a section's .hcol, which the structure BSP's
    open-space test does not know about.
    """

    RADIUS, HEIGHT = 1.1 + .25, 5.2

    def __init__(self, surfaces, minimum_normal=.65, radius=None, bottom=None):
        # `bottom` is the lowest test point above the feet; below it the player steps up.
        self.RADIUS = radius or self.RADIUS
        self.bottom = bottom if bottom is not None else .2 + self.RADIUS
        self.buckets = {}
        for normal, _, _, _, points in surfaces:
            if normal[2] >= minimum_normal:
                continue
            low = [min(p[i] for p in points) for i in range(3)]
            high = [max(p[i] for p in points) for i in range(3)]
            entry = (normal, points, low, high)
            for gx in range(int(low[0] - self.RADIUS) >> 3, (int(high[0] + self.RADIUS) >> 3) + 1):
                for gy in range(int(low[1] - self.RADIUS) >> 3, (int(high[1] + self.RADIUS) >> 3) + 1):
                    self.buckets.setdefault((gx, gy), []).append(entry)

    @staticmethod
    def _distance(point, normal, points):
        px, py, pz = point
        ax, ay, az = points[0]
        offset = (px-ax)*normal[0] + (py-ay)*normal[1] + (pz-az)*normal[2]
        projected = (px - offset*normal[0], py - offset*normal[1], pz - offset*normal[2])
        inside = True
        best = float('inf')
        for a, b in zip(points, points[1:] + points[:1]):
            edge = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
            to = (projected[0]-a[0], projected[1]-a[1], projected[2]-a[2])
            cross = (edge[1]*to[2]-edge[2]*to[1], edge[2]*to[0]-edge[0]*to[2], edge[0]*to[1]-edge[1]*to[0])
            if cross[0]*normal[0] + cross[1]*normal[1] + cross[2]*normal[2] < 0:
                inside = False
            length = edge[0]**2 + edge[1]**2 + edge[2]**2
            t = 0 if not length else max(0, min(1, ((px-a[0])*edge[0] + (py-a[1])*edge[1] + (pz-a[2])*edge[2]) / length))
            best = min(best, math.dist(point, (a[0]+edge[0]*t, a[1]+edge[1]*t, a[2]+edge[2]*t)))
        return abs(offset) if inside else best

    CROUCH_HEIGHT = 4.5 * .75 + .7

    def blocked(self, x, y, z4, crouching=False):
        entries = self.buckets.get((int(x) >> 3, int(y) >> 3))
        if not entries:
            return False
        z = z4 / 4
        height = self.CROUCH_HEIGHT if crouching else self.HEIGHT
        for normal, points, low, high in entries:
            if x < low[0] - self.RADIUS or x > high[0] + self.RADIUS or y < low[1] - self.RADIUS or \
               y > high[1] + self.RADIUS or z + height < low[2] or z + .4 > high[2]:
                continue
            for level in sorted({min(self.bottom, height - self.RADIUS), (self.bottom + height - self.RADIUS) / 2, height - self.RADIUS}):
                if self._distance((x, y, z + level), normal, points) < self.RADIUS:
                    return True
        return False


def machine_boxes(halo, output, bsp, poses=None, skip=()):
    """The panel boxes of the scenario machines and doors in BSP `bsp` (not controls, not the
    baked machines in `skip`), each in the pose of its initial position or of the position `poses`
    gives its name: [(name, (ox, oy, oz, cosine, sine), (x1, y1, z1, x2, y2, z2))] in retail units,
    the box relative to the device, rotated by its yaw. Machines a script creates are left out."""
    rows, _, groups, _ = _device_table(halo, output)
    poses = poses or {}
    boxes = []
    for device, (name, row) in enumerate(rows):
        position, flags, not_placed, mask, control, kind = row[2], row[4], row[6], row[8], row[9], row[10]
        pose, panels = row[-2], row[-1]
        if control or kind == 2 or not panels or device in skip or not mask >> bsp & 1 or not_placed & 1:
            continue
        value = poses.get(name, groups[position] if 0 <= position < len(groups) else (1 if flags & 1 else 0))
        ox, oy, oz, yaw = pose[:4]
        for _, frames in panels:
            reversed_value = 1 - value if flags & 8 else value
            frame = frames[min(len(frames) - 1, max(0, round(reversed_value * (len(frames) - 1))))]
            boxes.append((name, (ox, oy, oz, math.cos(yaw), math.sin(yaw)), frame))
    return boxes


def _segment_box(a, b, place, box):
    """Whether the retail segment a-b passes through a device box (machine_boxes entry parts)."""
    ox, oy, oz, cosine, sine = place
    def local(p):
        dx, dy = p[0] - ox, p[1] - oy
        return (dx*cosine + dy*sine, -dx*sine + dy*cosine, p[2] - oz)
    la, lb = local(a), local(b)
    t0, t1 = 0.0, 1.0
    for i in range(3):
        d = lb[i] - la[i]
        low, high = box[i], box[i + 3]
        if abs(d) < 1e-9:
            if not low < la[i] < high:
                return False
            continue
        u, v = (low - la[i]) / d, (high - la[i]) / d
        if u > v:
            u, v = v, u
        t0, t1 = max(t0, u), min(t1, v)
        if t0 > t1:
            return False
    return True


class SightLines:
    """Lines of sight through the structure collision polygons (and the static machines baked into
    the .hcol) and machine boxes, as the engine's shots see them (haloscenariotrace: every surface,
    two-sided ones included, and the devices' boxes in their current pose).

    Points are in the route planner's campaign coordinates (Projection.point); `boxes` are
    machine_boxes entries in retail units."""

    def __init__(self, surfaces, projection, boxes=()):
        self.projection, self.boxes = projection, list(boxes)
        self.polygons, self.buckets = [], {}
        for normal, _, _, _, points in surfaces:
            low = [min(p[i] for p in points) for i in range(3)]
            high = [max(p[i] for p in points) for i in range(3)]
            index = len(self.polygons)
            self.polygons.append((normal, points, low, high))
            for gx in range(int(low[0]) >> 3, (int(high[0]) >> 3) + 1):
                for gy in range(int(low[1]) >> 3, (int(high[1]) >> 3) + 1):
                    self.buckets.setdefault((gx, gy), []).append(index)

    def retail(self, point):
        p = self.projection
        return ((point[0] - 9) / p.scale + p.x0, p.y0 - (point[1] - 9) / p.scale, p.z0 + point[2] / p.scale)

    @staticmethod
    def _crosses(a, b, normal, points):
        d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        denominator = normal[0]*d[0] + normal[1]*d[1] + normal[2]*d[2]
        if abs(denominator) < 1e-9:
            return False
        p0 = points[0]
        t = (normal[0]*(p0[0] - a[0]) + normal[1]*(p0[1] - a[1]) + normal[2]*(p0[2] - a[2])) / denominator
        if not 1e-4 < t < 1 - 1e-4:
            return False
        hit = (a[0] + d[0]*t, a[1] + d[1]*t, a[2] + d[2]*t)
        sign = None
        for u, v in zip(points, points[1:] + points[:1]):
            e = (v[0] - u[0], v[1] - u[1], v[2] - u[2])
            w = (hit[0] - u[0], hit[1] - u[1], hit[2] - u[2])
            c = (e[1]*w[2] - e[2]*w[1], e[2]*w[0] - e[0]*w[2], e[0]*w[1] - e[1]*w[0])
            s = c[0]*normal[0] + c[1]*normal[1] + c[2]*normal[2]
            if abs(s) < 1e-9:
                continue
            if sign is None:
                sign = s > 0
            elif sign != (s > 0):
                return False
        return True

    def clear(self, a, b):
        """Whether nothing stands between campaign points a and b."""
        steps = int(max(abs(b[0] - a[0]), abs(b[1] - a[1])) / 4) + 1
        seen = set()
        low = [min(a[i], b[i]) for i in range(3)]
        high = [max(a[i], b[i]) for i in range(3)]
        for k in range(steps + 1):
            x, y = a[0] + (b[0] - a[0]) * k / steps, a[1] + (b[1] - a[1]) * k / steps
            for gx in range((int(x) >> 3) - 1, (int(x) >> 3) + 2):
                for gy in range((int(y) >> 3) - 1, (int(y) >> 3) + 2):
                    for index in self.buckets.get((gx, gy), ()):
                        if index in seen:
                            continue
                        seen.add(index)
                        normal, points, plow, phigh = self.polygons[index]
                        if any(phigh[i] < low[i] or plow[i] > high[i] for i in range(3)):
                            continue
                        if self._crosses(a, b, normal, points):
                            return False
        ra, rb = self.retail(a), self.retail(b)
        return not any(_segment_box(ra, rb, place, box) for _, place, box in self.boxes)
