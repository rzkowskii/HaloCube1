"""Export the retail structure collision polygons in campaign coordinates."""

import heapq
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


def overlap_seals(surfaces, other, box=None, depth=.25):
    """The indices of `surfaces` (structure_surfaces' list of one BSP) that seal that BSP where another
    BSP of the same scenario carries on: polygons without a material (index -1, the closing face a
    BSP ends a doorway with; Halo switches BSPs before the player reaches it) whose middle is open
    space in `other` (the other BSP's CollisionTree) on both sides, `depth` campaign units along the
    normal, and inside `box` ((x0, y0, z0), (x1, y1, z1), campaign units) when given. A section that
    holds two BSPs (import_halo_campaign.EXTRA_BSPS) leaves these out of its walking and its .hcol:
    there the player walks from one BSP's doorway straight into the other's (c40 section 1's pulse
    generator room, BSP 12, and BSP 2's platform each seal the shared doorway corridor)."""
    found = []
    for index, (normal, _, _, material, points) in enumerate(surfaces):
        if material >= 0:
            continue
        middle = [sum(p[i] for p in points) / len(points) for i in range(3)]
        if box is not None and not all(box[0][i] <= middle[i] <= box[1][i] for i in range(3)):
            continue
        if all(other.is_open(*(middle[i] + side * depth * normal[i] for i in range(3))) for side in (-1, 1)):
            found.append(index)
    return found


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


def euler_rows(yaw, pitch, roll):
    """The rotation of a scenario object placement's (yaw, pitch, roll), as rows (retail axes).

    Halo's own matrix4x3_rotation_from_angles (math/matrix_math.c of the Halo CE source port in
    ../Halo3DS, which object placement uses through vectors3d_from_euler_angles3d): yaw about +z,
    then pitch about the world's y axis, then roll about the world's x axis, R = Rx(roll) Ry(-pitch)
    Rz(yaw). Its matrix rows n[0], n[1], n[2] are the object's forward, left and up. The final
    integration review replaced the scenery batch's R = Rz(yaw) Ry(-pitch) Rx(roll), which agrees
    only when the object is not both turned and tilted: against the BSP floors of c10, a50 and b40,
    Halo's composition leaves 45 tilted Covenant crates resting with fewer vertices in the floor (most
    with none) against 6 for the old one (25 equal).
    """
    cy, sy, cp, sp, cr, sr = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch), math.cos(roll), math.sin(roll)
    # Columns: forward, left, up.
    forward = (cy * cp, sy * cr - cy * sp * sr, cy * sp * cr + sy * sr)
    left = (-sy * cp, sy * sp * sr + cy * cr, cy * sr - sy * sp * cr)
    up = (-sp, -cp * sr, cp * cr)
    return tuple(tuple(axis[i] for axis in (forward, left, up)) for i in range(3))


# The scenario's scenery placements (scnr +528, 72 bytes each) and palette (scnr +540): the palette
# index, the object name index, the not-placed flags (1 created only by a script, 2/4/8 not on easy,
# normal or hard), the desired permutation, then position and (yaw, pitch, roll) at +8.
SCENERY_BLOCK, SCENERY_PALETTE = 528, 540


def scenery_placements(halo, bsp):
    """Scenery placements in BSP `bsp`: [(placement index, scen tag, name index, not-placed flags,
    desired permutation, (x, y, z, yaw, pitch, roll))], retail units. A placement is in a BSP when its
    origin is inside the BSP's open space (halo_devices.structure_leaves), or failing every BSP, inside
    its bounds, as for the machines (halo_devices.device_rows' bsp mask)."""
    from halo_devices import structure_leaves
    tags, scenario = halo.tags, halo.scenario
    palette = tags.block(scenario + SCENERY_PALETTE, 48)
    count = len(tags.block(scenario + 1444, 32))
    regions = [halo.bsp(i) for i in range(count)]
    bounds = [region.read('6f', address + 200) for region, address in regions]
    found = []
    for index, placement in enumerate(tags.block(scenario + SCENERY_BLOCK, 72)):
        variant, name, not_placed, permutation = tags.read('2hHh', placement)
        if not 0 <= variant < len(palette):
            continue
        tag = halo.reference(tags, palette[variant], 'scen')
        if tag is None:
            continue
        pose = tags.read('6f', placement + 8)
        candidates = [i for i, extent in enumerate(bounds)
                      if all(extent[2*j] - 1 <= c <= extent[2*j+1] + 1 for j, c in enumerate(pose[:3]))]
        inside = [i for i in candidates if structure_leaves(*regions[i], pose[:3])]
        if bsp in (inside or candidates):
            found.append((index, tag, name, not_placed, permutation, pose))
    return found


def _scenery_polygons(halo, tag, permutation):
    """Object-space collision polygons of a scenery tag (its coll tag's BSPs, each node's in the
    model's bind pose): (normal, flags, breakable, material, points). A node with several BSPs (one per
    permutation of its region: the boulders' five) contributes the one at `permutation`, or the first,
    which is the permutation halo_map_art.model_meshes draws."""
    from import_halo_models import Model
    tags = halo.tags
    model_ref = halo.reference(tags, tag.address + 40, 'mode')
    coll = halo.reference(tags, tag.address + 112, 'coll')
    if not model_ref or not coll:
        return []
    try:
        model = Model(halo, model_ref.name, 'part')
    except (ValueError, IndexError):
        return []
    names = [name for name, _ in model.nodes]
    result = []
    for index, node in enumerate(tags.block(coll.address + 652, 64)):
        node_name = tags.name(node, 32)
        model_node = node_name if node_name in model.bind_pose else names[index] if index < len(names) else None
        if model_node is None:
            continue
        bsps = tags.block(node + 52, 96)
        if not bsps:
            continue
        bsp = bsps[permutation] if 0 <= permutation < len(bsps) else bsps[0]
        transform = model.bind_pose[model_node]
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


def scenery_surfaces(halo, bsp, projection, program=None, window=None, skip=()):
    """Collision polygons of BSP `bsp`'s scenery placements, in campaign coordinates.

    Halo collides everything with scenery (rocks, trees, crates, the fallen computer banks); the
    engine's world collision is the .hcol, so a section's scenery goes in as static surfaces, listed
    in its SCN1 trailer (write_structure_collision). Returns (surfaces, placements, left out):
    `placements` is [(scenario scenery index, number of its surfaces)] in the order their polygons
    were appended, and `left out` a {reason: count}. Placements are left out when a script creates, destroys or moves
    them (not-placed flag 1, or a name the scripts use: object_create, object_destroy, the c10
    insertion's props) or when some difficulty does not place them (flags 2, 4, 8), since the .hcol
    is fixed; when their origin is outside `window` (retail xmin, xmax, ymin, ymax: the section's
    map) or their name is in `skip`; and when their tag has no collision model.
    """
    from import_halo_scripts import named_objects, Program
    program = program or Program(halo)
    names = {index: name for index, (name, _, _) in enumerate(named_objects(program))}
    polygons, surfaces, baked, left_out = {}, [], [], {}
    def leave(reason):
        left_out[reason] = left_out.get(reason, 0) + 1
    for index, tag, name_index, not_placed, permutation, pose in scenery_placements(halo, bsp):
        x, y, z, yaw, pitch, roll = pose
        name = names.get(name_index, '') if name_index >= 0 else ''
        if window is not None and not (window[0] <= x <= window[1] and window[2] <= y <= window[3]):
            leave('outside the map')
            continue
        if name and name in skip:
            leave('skipped')
            continue
        if not_placed & 1 or (name and re.search(rb'\b' + re.escape(name.encode()) + rb'\b', program.strings)):
            leave('scripted')
            continue
        if not_placed & 14:
            leave('not on every difficulty')
            continue
        key = tag.address, permutation
        if key not in polygons:
            polygons[key] = _scenery_polygons(halo, tag, permutation)
        if not polygons[key]:
            leave('no collision model')
            continue
        rows = euler_rows(yaw, pitch, roll)
        before = len(surfaces)
        def rotate(v):
            return tuple(sum(a * b for a, b in zip(row, v)) for row in rows)
        for normal, flags, breakable, material, points in polygons[key]:
            nx, ny, nz = rotate(normal)
            length = math.hypot(nx, ny, nz)
            if not length:
                continue
            placed = []
            for v in points[::-1]:
                rx, ry, rz = rotate(v)
                placed.append(projection.point((rx + x, ry + y, rz + z)))
            if flags & 1 and nz < 0:
                # A two-sided polygon collides alike from either side (halo_collision.cpp); face it up,
                # so the planner (floor_samples reads upward normals) walks its back as Halo's biped and
                # the engine's bodies do: the tilted invisible ramps (door_blast_collision) over c10's
                # crate piles face down.
                nx, ny, nz = -nx, -ny, -nz
                placed.reverse()
            # Campaign projection reflects Y: flip the normal's Y and reverse the winding.
            surfaces.append(((nx/length, -ny/length, nz/length), flags & 15, breakable, material, placed))
        baked.append((index, len(surfaces) - before))
    return surfaces, baked, left_out


def assist_surfaces(assists, projection):
    """Campaign-space collision polygons of a section's authored walking assists
    (import_halo_campaign.ASSISTS): each `points` is a convex, planar polygon in retail units.

    An assist is not retail data. It is an invisible, two-sided walking surface (a plank or a
    ramp) standing in for a jump or climb that Halo's biped makes and the engine's fixed jump
    cannot. The polygons are wound to agree with their upward normal, as the structure's are, and
    must be walkable (normal z at least 0.65, the planner's and retailmove's floor limit).
    """
    surfaces = []
    for assist in assists:
        points = [projection.point(p) for p in assist.points]
        if not 3 <= len(points) <= 8:
            raise ValueError(f'assist {assist.why!r}: needs 3 to 8 points')
        # Newell's normal; the campaign projection reflects Y, so wind it to point up.
        nx = sum((a[1] - b[1]) * (a[2] + b[2]) for a, b in zip(points, points[1:] + points[:1]))
        ny = sum((a[2] - b[2]) * (a[0] + b[0]) for a, b in zip(points, points[1:] + points[:1]))
        nz = sum((a[0] - b[0]) * (a[1] + b[1]) for a, b in zip(points, points[1:] + points[:1]))
        length = math.hypot(nx, ny, nz)
        if not length:
            raise ValueError(f'assist {assist.why!r}: degenerate polygon')
        if nz < 0:
            points.reverse()
            nx, ny, nz = -nx, -ny, -nz
        normal = (nx / length, ny / length, nz / length)
        if normal[2] < .65:
            raise ValueError(f'assist {assist.why!r}: too steep to walk (normal z {normal[2]:.2f})')
        for i, a in enumerate(points):
            b, c = points[(i + 1) % len(points)], points[(i + 2) % len(points)]
            u, v = [q - p for p, q in zip(a, b)], [q - p for p, q in zip(b, c)]
            turn = (u[1]*v[2] - u[2]*v[1], u[2]*v[0] - u[0]*v[2], u[0]*v[1] - u[1]*v[0])
            if sum(t * n for t, n in zip(turn, normal)) < -1e-6:
                raise ValueError(f'assist {assist.why!r}: polygon is not convex')
            if abs(sum((p - q) * n for p, q, n in zip(a, points[0], normal))) > .05:
                raise ValueError(f'assist {assist.why!r}: polygon is not planar')
        # Flag 1: two-sided, like the retail invisible ramps laid over stairs. Material -1: none.
        surfaces.append((normal, 1, 0, -1, points))
    return surfaces


def write_structure_collision(halo, bsp, projection, destination, machines=((), ()), assists=(), scenery=((), ()),
                              structure=None):
    """Write the section's .hcol; `machines` is static_device_surfaces' (surfaces, devices),
    `assists` assist_surfaces' polygons (authored, non-retail), listed in an AST1 trailer, and
    `scenery` scenery_surfaces' (surfaces, placements), listed in an SCN1 trailer. `structure`, when
    given, replaces the BSP's own structure_surfaces (a section holding extra BSPs leaves out the
    seals between them: overlap_seals)."""
    surfaces = list(structure) if structure is not None else structure_surfaces(halo, bsp, projection)[0]
    extra, baked = machines
    surfaces = surfaces + list(extra)
    scenery_polygons, placements = scenery
    first_scenery = len(surfaces)
    surfaces = surfaces + list(scenery_polygons)
    first_assist = len(surfaces)
    surfaces = surfaces + list(assists)
    # HCL1 stores little-endian surface records followed by their boundary vertices. Optional
    # trailers follow, in this order: DEV1 lists the scenario devices whose polygons are included;
    # SCN1 the scenery placements baked in (scenery_surfaces: each its scenario scenery index, then
    # the first surface and count of its polygons in file order; they are ordinary static surfaces,
    # which everything collides with, as in Halo); AST1 the surface indices (file order) that are
    # walking assists: the engine collides bodies with them but rays (shots, sight, probes) and
    # grenades pass through, as there is nothing there in Halo. A file without SCN1 is byte for byte
    # what this writer wrote before scenery collision (SCENERY_COLLISION in import_halo_campaign.py).
    data = bytearray(struct.pack('<4sI', b'HCL1', len(surfaces)))
    for normal, flags, breakable, material, points in surfaces:
        data.extend(struct.pack('<3fBBhI', *normal, flags, breakable, material, len(points)))
        for point in points:
            data.extend(struct.pack('<3f', *point))
    if baked:
        data.extend(struct.pack(f'<4sI{len(baked)}I', b'DEV1', len(baked), *baked))
    if placements:
        # scenery_surfaces appends each placement's polygons in turn: recover the ranges from the
        # per-placement counts it reports.
        rows, first = [], first_scenery
        for index, count in placements:
            rows.extend((index, first, count))
            first += count
        if first != first_assist:
            raise ValueError('scenery placements do not account for the scenery surfaces')
        data.extend(struct.pack(f'<4sI{len(rows)}I', b'SCN1', len(placements), *rows))
    if assists:
        indices = list(range(first_assist, len(surfaces)))
        data.extend(struct.pack(f'<4sI{len(indices)}I', b'AST1', len(indices), *indices))

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


class GrenadeArcs:
    """A frag grenade's flight on the retail collision, for the route planner's throws (ThrowTarget in
    tools/import_halo_campaign.py). It follows the engine step by step: halofrag's velocity (1.8 along
    the view, raised by 0.14), the halogrenade bounceent (radius 0.2, eye 0.3, maxspeed 30) moved by
    moveplayer's ENT_BOUNCE branch (gravity on the velocity in the air, friction on the floor) and
    retailmove (one contact a step, reflected at 1.5 times the inward speed and slowed to 0.7, a floor
    contact stopping the fall), ten 10 ms launch steps and then 5 ms physics steps, and the device
    boxes as Scenario::collideDevices sees them (a grenade that runs into one stays where it was, with
    its velocity, and lands on a top it falls onto). The sphere sweep is halocollisionsweep's (the
    route batch, gap 9 of batch 5's critic: before, it ignored polygon edges beyond the radius): the
    nearest point on each polygon's face, edges or corners, conservative advancement and the rounded
    contact normal, so a frag that grazes an edge or a corner bounces off it as in the engine. The
    planner still prefers throws whose neighbours land too. Points are in campaign (cube) coordinates;
    `boxes` are machine_boxes entries (retail) in the pose of the throw."""

    RADIUS, EYE = .2, .3
    # halo_collision.cpp contactSlop: a contact within it counts.
    CONTACT_SLOP = .0025
    SPEED = 30.0
    GRAVITY = .5 * 9.81 / 4 / 1000
    SKIN, SLOPE = .015, .65

    def __init__(self, surfaces, projection, boxes=()):
        self.p = projection
        self.polygons, self.buckets = [], {}
        for normal, flags, _, _, points in surfaces:
            low = [min(q[i] for q in points) - 1 for i in range(3)]
            high = [max(q[i] for q in points) + 1 for i in range(3)]
            edges = []
            for a, b in zip(points, points[1:] + points[:1]):
                e = (b[0]-a[0], b[1]-a[1], b[2]-a[2])
                inward = (normal[1]*e[2]-normal[2]*e[1], normal[2]*e[0]-normal[0]*e[2], normal[0]*e[1]-normal[1]*e[0])
                length = math.sqrt(sum(v*v for v in inward)) or 1
                edges.append((a, tuple(v/length for v in inward), b))
            index = len(self.polygons)
            self.polygons.append((normal, flags & 1, points, low, high, edges))
            for gx in range(int(low[0]) >> 3, (int(high[0]) >> 3) + 1):
                for gy in range(int(low[1]) >> 3, (int(high[1]) >> 3) + 1):
                    self.buckets.setdefault((gx, gy), []).append(index)
        # Each box with the radius round its origin that holds it, for a quick rejection.
        self.boxes = [(ox, oy, oz, c, s, *box, (max(abs(box[0]), abs(box[3])) ** 2 + max(abs(box[1]), abs(box[4])) ** 2) ** .5 + .1)
                      for _, (ox, oy, oz, c, s), box in boxes]
        # The boxes by the 8-unit campaign cells their reach (and the grenade's radius) covers.
        self.box_cells = {}
        for entry in self.boxes:
            ox, oy, reach = entry[0], entry[1], entry[-1] * projection.scale + self.RADIUS + 1
            cx, cy = (ox - projection.x0) * projection.scale + 9, (projection.y0 - oy) * projection.scale + 9
            for gx in range(int(cx - reach) >> 3, (int(cx + reach) >> 3) + 1):
                for gy in range(int(cy - reach) >> 3, (int(cy + reach) >> 3) + 1):
                    self.box_cells.setdefault((gx, gy), []).append(entry)
        self.nearby = {}

    def _sweep(self, centre, delta):
        """The sphere's first contact moving centre -> centre + delta: (fraction, normal) or None.

        halocollisionsweep for the grenade's body (a sphere: radius 0.2, height max(0.3, 0.4)): per
        polygon, conservative advancement along the delta by the gap between the sphere's centre and
        the polygon's nearest point, on its face or, outside the face, on its edges and corners
        (capsulegap and segmentgap with a zero axis), up to 24 steps; a contact within the engine's
        slop counts when the polygon is two-sided or the contact normal (from the nearest point to the
        centre, so rounded round an edge) faces the polygon's front (frontcontact). The earliest
        contact of all polygons wins, its normal the contact normal."""
        r, slop, small = self.RADIUS, self.CONTACT_SLOP, 1e-6
        x0, y0, x1, y1 = centre[0], centre[1], centre[0] + delta[0], centre[1] + delta[1]
        key = (int(min(x0, x1) - r) >> 3, int(max(x0, x1) + r) >> 3, int(min(y0, y1) - r) >> 3, int(max(y0, y1) + r) >> 3)
        candidates = self.nearby.get(key)
        if candidates is None:
            found = set()
            for gx in range(key[0], key[1] + 1):
                for gy in range(key[2], key[3] + 1):
                    found.update(self.buckets.get((gx, gy), ()))
            candidates = self.nearby[key] = sorted(found)
        lx, hx = min(x0, x1) - r, max(x0, x1) + r
        ly, hy = min(y0, y1) - r, max(y0, y1) + r
        z0, z1 = centre[2], centre[2] + delta[2]
        lz, hz = min(z0, z1) - r, max(z0, z1) + r
        dx, dy, dz = delta
        best, limit = None, 1.0
        polygons = self.polygons
        for index in candidates:
            n, two_sided, points, plow, phigh, edges = polygons[index]
            if phigh[0] < lx or plow[0] > hx or phigh[1] < ly or plow[1] > hy or phigh[2] < lz or plow[2] > hz:
                continue
            a = points[0]
            d0 = n[0]*(centre[0]-a[0]) + n[1]*(centre[1]-a[1]) + n[2]*(centre[2]-a[2])
            d1 = d0 + n[0]*dx + n[1]*dy + n[2]*dz
            # The sphere never comes within its radius of the polygon's plane in this step.
            if (d0 > r + slop and d1 > r + slop) or (d0 < -r - slop and d1 < -r - slop):
                continue
            # Behind a one-sided polygon every contact normal faces its back: never a contact.
            if not two_sided and d0 < 0:
                continue
            fraction = 0.0
            for _ in range(24):
                if fraction > limit:
                    break
                px, py, pz = centre[0] + dx*fraction, centre[1] + dy*fraction, centre[2] + dz*fraction
                d = n[0]*(px-a[0]) + n[1]*(py-a[1]) + n[2]*(pz-a[2])
                qx, qy, qz = px - n[0]*d, py - n[1]*d, pz - n[2]*d
                if all((qx-e[0])*w[0] + (qy-e[1])*w[1] + (qz-e[2])*w[2] >= 0 for e, w, _ in edges):
                    gx, gy, gz = n[0]*d, n[1]*d, n[2]*d
                else:
                    gap = None
                    for e, _, f in edges:
                        ex, ey, ez = f[0]-e[0], f[1]-e[1], f[2]-e[2]
                        length = ex*ex + ey*ey + ez*ez
                        t = 0.0 if length <= small*small else \
                            max(0.0, min(1.0, ((px-e[0])*ex + (py-e[1])*ey + (pz-e[2])*ez) / length))
                        cx, cy, cz = px - e[0] - ex*t, py - e[1] - ey*t, pz - e[2] - ez*t
                        squared = cx*cx + cy*cy + cz*cz
                        if gap is None or squared < gap[0]:
                            gap = (squared, cx, cy, cz)
                    _, gx, gy, gz = gap
                distance = math.sqrt(gx*gx + gy*gy + gz*gz)
                if distance > small:
                    normal = (gx/distance, gy/distance, gz/distance)
                else:
                    normal = tuple(-v for v in n) if d < 0 and two_sided else n
                closing = -(normal[0]*dx + normal[1]*dy + normal[2]*dz)
                if closing <= small:
                    break
                clearance = distance - r
                if clearance <= slop:
                    if two_sided or normal[0]*n[0] + normal[1]*n[1] + normal[2]*n[2] >= -small:
                        best, limit = (fraction, normal), fraction
                    break
                fraction += clearance / closing
        return best

    def _devices(self, o):
        """Scenario::collideDevices for the grenade at eye point o: (blocked, the top it may stand on)."""
        p = self.p
        rx, ry = (o[0] - 9) / p.scale + p.x0, p.y0 - (o[1] - 9) / p.scale
        radius = self.RADIUS / p.scale
        feet, height = o[2] - self.EYE, self.EYE
        lo, hi = -1e9, 1e9
        for ox, oy, oz, c, s, x1, y1, z1, x2, y2, z2, reach in self.box_cells.get((int(o[0]) >> 3, int(o[1]) >> 3), ()):
            dx, dy = rx - ox, ry - oy
            if dx*dx + dy*dy > reach*reach:
                continue
            lx, ly = dx*c + dy*s, -dx*s + dy*c
            if not (x1 - radius < lx < x2 + radius and y1 - radius < ly < y2 + radius):
                continue
            bottom, top = (z1 + oz - p.z0) * p.scale, (z2 + oz - p.z0) * p.scale
            if feet >= top - .23 * p.scale:
                lo = max(lo, top)
            elif feet + height <= bottom + .10 * p.scale:
                hi = min(hi, bottom)
            elif top - bottom > .05 * p.scale:
                return True, lo
            else:
                lo = max(lo, top)
            if hi - lo < height:
                return True, lo
        return False, lo

    def throw(self, eye, yaw, pitch, fuse=(1500, 500), limit=4000, bounds=None):
        """The blast point (campaign) and clock (ms) of a frag the player with eye point `eye` throws
        facing `yaw`, `pitch` (the engine's degrees), or None if it leaves `bounds` ((low, high)
        campaign corners) or has not gone off within `limit` ms. `fuse`: the arming time from the
        throw and the time from the first bounce (packages/halo/equipment.txt)."""
        ya, pa = math.radians(yaw), math.radians(pitch)
        vel = [math.sin(ya)*math.cos(pa)*1.8, -math.cos(ya)*math.cos(pa)*1.8, (math.sin(pa) + .14)*1.8]
        o = list(eye)
        onfloor, bounced, clock = False, None, 0
        # The launch moves it ten 10 ms steps in the frame it is thrown; then 5 ms physics steps.
        schedule = [(10, False)] * 10
        while True:
            for curtime, ticking in schedule:
                clock += curtime if ticking else 0
                if onfloor:
                    friction = max(6 * 20 / curtime, 1)
                    vel = [v * (friction - 1) / friction for v in vel]
                else:
                    vel[2] -= self.GRAVITY * curtime
                speed = curtime * self.SPEED / 1000
                delta = [v * speed for v in vel]
                original = list(o)
                normal, floor, collided = None, False, False
                if sum(v*v for v in delta) > 1e-8:
                    hit = self._sweep([o[0], o[1], o[2] - self.EYE + self.RADIUS], delta)
                    if hit is None:
                        o = [o[i] + delta[i] for i in range(3)]
                    else:
                        t, normal = hit
                        approach = -sum(delta[i]*normal[i] for i in range(3))
                        margin = self.SKIN / approach if approach > self.SKIN else 0
                        o = [o[i] + delta[i]*max(0.0, t - margin) for i in range(3)]
                        collided, floor = True, normal[2] >= self.SLOPE
                blocked, lo = self._devices(o)
                if not blocked and o[2] - self.EYE < lo:
                    if original[2] - self.EYE >= lo - self.SKIN:
                        o[2], floor = lo + self.EYE + self.SKIN, True
                    elif o[2] - self.EYE < lo - self.SKIN:
                        blocked = True
                if blocked:
                    # retailmove puts it back, its velocity as it was; that counts as a bounce.
                    o, collided = original, True
                else:
                    onfloor = floor
                    if floor and vel[2] < 0:
                        vel[2] = 0
                    if normal is not None:
                        inward = sum(vel[i]*normal[i] for i in range(3))
                        if inward < 0:
                            vel = [vel[i] - 1.5*inward*normal[i] for i in range(3)]
                        vel = [v * .7 for v in vel]
                if collided and bounced is None:
                    bounced = clock
                if bounds is not None and any(not bounds[0][i] <= o[i] <= bounds[1][i] for i in range(3)):
                    return None
                if ticking and bounced is not None and clock >= fuse[0] and clock - bounced >= fuse[1]:
                    return tuple(o), clock
                if clock > limit:
                    return None
                if ticking and bounced is not None and onfloor and not blocked and \
                        math.sqrt(sum(v*v for v in vel)) * self.SPEED * 5 / 1000 * 24 < .01:
                    # All but at rest on a floor: friction takes a 24th of the velocity a 5 ms step, so
                    # it rolls less than a hundredth of a unit further in all; it goes off where it lies.
                    end = max(fuse[0], bounced + fuse[1], clock)
                    return (tuple(o), end) if end <= limit else None
            schedule = [(5, True)] * 50


class VehicleGround:
    """Where a ground vehicle (the Warthog) can drive on a section's retail collision, and a route for
    it (import_halo_campaign.VEHICLE_ROUTES, the driven sections).

    Floor samples on the campaign grid (one cell per AssaultCube unit) of every collision polygon
    whose normal is at least `minimum_normal` (the slope limit; the engine's car treats faces under
    0.3 as walls and climbs what its throttle and run-up carry it up, halo_vehicle.cpp simulate), with
    open space round the chassis: the structure BSP's open-space test at the chassis bottom (`step`
    above the floor, following the polygon's slope) and top (`height`) on a ring of `radius` (the
    chassis capsule's radius and a margin), as the engine sweeps two capsules of the vehicle's
    half-width (chassisBlocked). `length` is the distance from the body's centre to the front of
    its capsules: each step of a route also needs open space that far ahead along the step. Where
    the floor ends the route may jump (jump): off the surface's slope at speed, under the vehicle's
    retail-derived gravity, onto floor under both axles (supported).
    Coordinates are campaign units, heights in quarter units, as floor_samples'."""

    RING = tuple((math.cos(a * math.pi / 4), math.sin(a * math.pi / 4)) for a in range(8))

    def __init__(self, surfaces, tree, size, radius, height, step, length, minimum_normal=.4, extra_walls=None):
        self.tree, self.radius, self.height, self.step, self.length = tree, radius, height, step, length
        self.walls = extra_walls
        self.samples = {}
        for normal, flags, _, _, points in surfaces:
            nx, ny, nz = normal
            if nz < minimum_normal or flags & 1:
                continue
            x0, y0, z0 = points[0]
            low_x = max(6, math.ceil(min(p[0] for p in points)))
            high_x = min(size - 7, math.floor(max(p[0] for p in points)))
            low_y = max(6, math.ceil(min(p[1] for p in points)))
            high_y = min(size - 7, math.floor(max(p[1] for p in points)))
            if low_x > high_x or low_y > high_y:
                continue
            edges = [(b[1] - a[1], a[0] - b[0], b[0]*a[1] - a[0]*b[1]) for a, b in zip(points, points[1:] + points[:1])]
            cx = sum(p[0] for p in points) / len(points)
            cy = sum(p[1] for p in points) / len(points)
            flip = -1 if sum(e[0]*cx + e[1]*cy + e[2] for e in edges) < 0 else 1

            def plane(x, y):
                return z0 - ((x - x0)*nx + (y - y0)*ny) / nz
            for x in range(low_x, high_x + 1):
                for y in range(low_y, high_y + 1):
                    if any(flip*(e[0]*x + e[1]*y + e[2]) < -1e-6 for e in edges):
                        continue
                    z = plane(x, y)
                    if self._fits(x, y, z, plane):
                        self.samples.setdefault((x, y), set()).add(round(4 * z))
        self.samples = {p: sorted(levels) for p, levels in self.samples.items()}

    def _fits(self, x, y, z, plane):
        tree, step, height = self.tree, self.step, self.height
        if not tree.is_open(x, y, z + step + .05) or not tree.is_open(x, y, z + height):
            return False
        for dx, dy in self.RING:
            px, py = x + dx * self.radius, y + dy * self.radius
            base = max(z, plane(px, py))
            if not tree.is_open(px, py, base + step + .05) or not tree.is_open(px, py, base + height):
                return False
        if self.walls is not None and self.walls.blocked(x, y, round(4 * (z + step))):
            return False
        return True

    def nearest(self, point, limit=12):
        """The sample (x, y, z4) nearest a campaign point (x, y, z), or None within `limit` cells."""
        best = None
        for x in range(round(point[0]) - limit, round(point[0]) + limit + 1):
            for y in range(round(point[1]) - limit, round(point[1]) + limit + 1):
                for z in self.samples.get((x, y), ()):
                    d = (x - point[0])**2 + (y - point[1])**2 + (z / 4 - point[2])**2 * 4
                    if best is None or d < best[0]:
                        best = (d, (x, y, z))
        return best and best[1]

    def _ahead(self, node, dx, dy):
        """Open space ahead of a body at `node` heading (dx, dy): the front of its chassis."""
        length = math.hypot(dx, dy)
        ux, uy = dx / length, dy / length
        x, y, z = node[0] + ux * self.length, node[1] + uy * self.length, node[2] / 4
        # Up a slope the chassis's front stands on the floor ahead, not at the body's height.
        rise = 4 * (self.step + 1.25) * self.length
        ahead = [o for o in self.samples.get((round(x), round(y)), ()) if abs(o - node[2]) <= rise]
        if ahead:
            z = max(z, max(ahead) / 4)
        return self.tree.is_open(x, y, z + self.step + .05) and self.tree.is_open(x, y, z + self.height * .7)

    def supported(self, node, ux, uy):
        """Whether a vehicle standing at `node` (x, y, z4) heading (ux, uy) has floor under both
        axles: a sample near its height at the front and rear wheels (0.7 of the half length
        either way, as halo_vehicle.cpp dims places them), so a landing is on floor, not on a
        strut or beam the chassis's ring of open space happens to fit over."""
        reach = .7 * self.length
        rise = 4 * (self.step + 1.25) * max(1.0, reach)
        for sign in (1, -1):
            cell = (round(node[0] + sign * ux * reach), round(node[1] + sign * uy * reach))
            if not any(abs(o - node[2]) <= rise for o in self.samples.get(cell, ())):
                return False
        return True

    def jump(self, here, back, dx, dy, speed, gravity, drop4):
        """Where a vehicle driving off the edge at `here` at `speed` lands. It leaves the ground with
        its velocity along the surface it was on (halo_vehicle.cpp simulate keeps the tangent: the
        horizontal speed and the vertical rate of the floor under its trailing wheels, the last to
        leave, at most 0.8 of the speed upwards; a downhill edge keeps its descent): the slope from
        `back` (a cell about two before, along the route) to `here`. It falls at `gravity` (the
        retail-derived vehicle gravity, vehicle_gravity in import_halo_campaign.py) and lands on the
        first floor sample under its path, at least three cells on, with both axles on floor
        (supported) and open space above the path at its middle and at its front (the chassis's
        front meets a doorway's lintel first). None when it lands nowhere (or falls further than
        `drop4` quarter units below the take-off).
        The engine's car stays on the floor until its rear wheels (0.7 of the half length behind
        its origin, halo_vehicle.cpp dims) are past the edge too, riding at their floor's height:
        so the fall starts that far on, past the edge (taken as half a cell past `here`, the last
        cell with floor), at the edge's height."""
        length = math.hypot(dx, dy)
        ux, uy = dx / length, dy / length
        run = math.dist(here[:2], back[:2]) if back is not None else 0.0
        rise = (here[2] - back[2]) / 4 if back is not None and run > 0 else 0.0
        vz = max(-speed, min(.8 * speed, speed * rise / run)) if run > 0 else 0.0
        edge, rollout = .5, .7 * self.length + .5
        x, y, z = float(here[0]), float(here[1]), here[2] / 4
        slope = vz / speed if speed > 0 else 0.0
        dt, travelled, seen = 1 / 60, 0.0, set()
        while travelled < 160:
            x += ux * speed * dt
            y += uy * speed * dt
            travelled += speed * dt
            if travelled < rollout:
                # Still on its rear wheels: at the floor's height under them, up to the edge.
                z = here[2] / 4 + slope * min(travelled, edge)
            else:
                z += vz * dt
                vz -= gravity * dt
            cell = (round(x), round(y))
            if here[2] / 4 - z > drop4 / 4:
                return None
            if not self.tree.is_open(x, y, z + self.step + .05) or not self.tree.is_open(x, y, z + self.height * .8) or \
                    not self.tree.is_open(x + ux * self.length, y + uy * self.length, z + self.height * .8):
                # Into a wall (or its front into a lintel), or down onto something without floor samples there.
                return None
            if cell in seen:
                continue
            seen.add(cell)
            if math.hypot(cell[0] - here[0], cell[1] - here[1]) < 3:
                continue
            for other in self.samples.get(cell, ()):
                if z - .6 - abs(vz) * dt * 2 <= other / 4 <= z + self.step:
                    node = (cell[0], cell[1], other)
                    if not self.supported(node, ux, uy):
                        # A strut or a beam's edge: the wheels would come down either side of it.
                        continue
                    return node, travelled
        return None

    def route(self, start, inside, goal, drop=0, slope_rise=1.25, speed=0.0, gravity=40.0, jump_drop=None):
        """The cheapest drive from `start` (a sample) to a sample `inside` accepts (x, y, z4), or, with
        none reachable, the one nearest `goal` (campaign point): [(x, y, z4)], with whether it ended
        inside. Eight neighbours a step; a step up may rise `step` plus `slope_rise` units a cell (the
        slope limit already chose the floors); a step down may fall up to `drop` units (the vehicle
        drives off a ledge or down a ramp too steep to stand on: up to eight cells on, open above).
        Cells whose ring of four points at the radius has no floor near the body's height cost more,
        so the route keeps to the middle of a corridor and off edges. With a `speed`, where the floor
        ends the vehicle may jump (jump) at that speed under `gravity`, landing up to `jump_drop`
        units below the take-off (default: `drop`, else a step)."""
        samples, step4 = self.samples, 4 * (self.step + slope_rise)
        drop4 = 4 * drop
        jump4 = 4 * jump_drop if jump_drop is not None else drop4 or step4
        # The take-off's slope: over the last two cells, the floor under the trailing axle (the
        # engine measures it from that floor's normal, halo_vehicle.cpp simulate).
        takeoff_base = 2.0
        best, parents = {start: 0.0}, {start: None}
        queue = [(0.0, start)]
        done, ahead_ok, edge_cost = set(), {}, {}

        def edge(node):
            if node not in edge_cost:
                missing = 0
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    cell = (round(node[0] + dx * self.radius * 1.6), round(node[1] + dy * self.radius * 1.6))
                    if not any(abs(z - node[2]) <= step4 * 2 for z in samples.get(cell, ())):
                        missing += 1
                edge_cost[node] = missing * 2.0
            return edge_cost[node]

        def push(here, node, price, dx, dy):
            key = (node, dx, dy)
            if key not in ahead_ok:
                ahead_ok[key] = self._ahead(node, dx, dy)
            if not ahead_ok[key]:
                return
            total = best[here] + price + edge(node)
            if total < best.get(node, float('inf')):
                best[node] = total
                parents[node] = here
                heapq.heappush(queue, (total, node))
        found = None
        self.jumps = set()
        while queue:
            cost, here = heapq.heappop(queue)
            if here in done:
                continue
            done.add(here)
            if inside is not None and inside(*here):
                found = here
                break
            x, y, z = here
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1)):
                length = math.hypot(dx, dy)
                ahead = False
                for other in samples.get((x + dx, y + dy), ()):
                    rise = other - z
                    if rise <= step4 * length and -rise <= max(step4 * length, drop4):
                        push(here, (x + dx, y + dy, other), length, dx, dy)
                        ahead = True
                if speed > 0 and not ahead:
                    # The floor ends this way: a jump at speed across the gap (the Maw's run), off
                    # the slope of the route's last two cells (the surface the trailing wheels were on).
                    back = parents.get(here)
                    while back is not None and parents.get(back) is not None and math.dist(here[:2], back[:2]) < takeoff_base:
                        back = parents[back]
                    landed = self.jump(here, back, dx, dy, speed, gravity, jump4)
                    if landed is not None:
                        node, travelled = landed
                        before = best.get(node, float('inf'))
                        push(here, node, travelled + 20, dx, dy)
                        if best.get(node, float('inf')) < before:
                            self.jumps.add(node)
                if drop4 and abs(dx) + abs(dy) == 1:
                    # Off a ledge or down a ramp too steep for floor samples: land up to eight cells on.
                    for reach in range(2, 9):
                        cell = (x + dx * reach, y + dy * reach)
                        landing = [o for o in samples.get(cell, ()) if step4 < z - o <= drop4]
                        if not landing:
                            continue
                        between = [(x + dx * k, y + dy * k) for k in range(1, reach)]
                        if any(abs(o - z) <= step4 for c in between for o in samples.get(c, ())):
                            break
                        other = max(landing)
                        if all(self.tree.is_open(cx, cy, z / 4 + self.step + .05) and self.tree.is_open(cx, cy, z / 4 + self.height)
                               for cx, cy in between):
                            push(here, (cell[0], cell[1], other), reach + 4, dx, dy)
                        break
        ended = found is not None
        if found is None:
            if not best:
                return [], False
            found = min(best, key=lambda n: (n[0] - goal[0])**2 + (n[1] - goal[1])**2 + ((n[2] / 4 - goal[2]) * 2)**2)
        path = [found]
        while parents[path[-1]] is not None:
            path.append(parents[path[-1]])
        return path[::-1], ended


class FlightSpace:
    """Free space for a flyer (the Banshee) over a section's retail collision, on a coarse 3D grid, and
    a flight through it (import_halo_campaign.VEHICLE_ROUTES, the flown c40 sections).

    A node is a body position (the vehicle's origin, at the bottom of its body) on a grid of `cell`
    campaign units. It is free when the body's box round it (`radius` across, `height` up, as the
    engine's flight capsules, flightBlocked) is open space (`is_open(x, y, z)`: the structure BSP's
    open-space test, with points outside the BSPs the section holds counting as open, as the engine
    loads no collision there) and
    no collision polygon (two-sided ones included) crosses the lines from its middle to its sides,
    top and bottom (SightLines). A move to a neighbour (26) is free when both ends are and nothing
    crosses the lines between their middles, tops and bottoms. Nodes are checked as the search
    reaches them."""

    def __init__(self, is_open, sights, cell, radius, height):
        self._open, self.sights = is_open, sights
        self.cell, self.radius, self.height = cell, radius, height
        self.cache, self.edges = {}, {}
        self.explored = 0

    def point(self, node):
        return (node[0] * self.cell, node[1] * self.cell, node[2] * self.cell)

    def node(self, point):
        return tuple(round(v / self.cell) for v in point)

    def body_free(self, p):
        """Whether the body with its origin at campaign point `p` is in open space."""
        x, y, z = p
        middle = (x, y, z + self.height / 2)
        probes = [(x, y, z + .1), (x, y, z + self.height)] + \
                 [(x + dx * self.radius, y + dy * self.radius, z + self.height * f) for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1))
                  for f in (.15, .85)]
        if not self._open(*middle) or not all(self._open(*q) for q in probes):
            return False
        return all(self.sights.clear(middle, q) for q in probes)

    def free(self, node):
        if node not in self.cache:
            self.cache[node] = self.body_free(self.point(node))
        return self.cache[node]

    def clear(self, a, b):
        """Whether the body moves in a straight line between campaign points a and b."""
        for f in (.1, .5, .9):
            if not self.sights.clear((a[0], a[1], a[2] + self.height * f), (b[0], b[1], b[2] + self.height * f)):
                return False
        steps = max(1, int(math.dist(a, b) / self.cell))
        return all(self.body_free(tuple(a[i] + (b[i] - a[i]) * k / steps for i in range(3))) for k in range(1, steps))

    def route(self, start, goal, inside=None, limit=60000, weight=1.6):
        """A* (the distance to `goal` weighted by `weight`, which finds a near-shortest flight much
        sooner) from campaign point `start` to a free node `inside` accepts (a campaign point), else
        the free node nearest `goal` once `limit` nodes are explored: a list of campaign points (start
        first, then the straightened flight), and whether it arrived."""
        first = self.node(start)
        # Lift off: the nearest free node at or above the start.
        for up in range(0, 12):
            if self.free((first[0], first[1], first[2] + up)):
                first = (first[0], first[1], first[2] + up)
                break
        else:
            return [start], False
        target = self.node(goal)

        def h(n):
            return math.dist(n, target) * weight
        best, parents, queue, done = {first: 0.0}, {first: None}, [(h(first), first)], set()
        steps = [(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1) if dx or dy or dz]
        found = None
        while queue and len(done) < limit:
            _, here = heapq.heappop(queue)
            if here in done:
                continue
            done.add(here)
            if (inside(self.point(here)) if inside is not None else here == target):
                found = here
                break
            for dx, dy, dz in steps:
                node = (here[0] + dx, here[1] + dy, here[2] + dz)
                if node in done or not self.free(node):
                    continue
                key = (here, node) if here < node else (node, here)
                if key not in self.edges:
                    self.edges[key] = self.sights.clear(*[(p[0], p[1], p[2] + self.height * f)
                                                          for p in (self.point(here), self.point(node)) for f in (.5,)])
                if not self.edges[key]:
                    continue
                total = best[here] + math.sqrt(dx*dx + dy*dy + dz*dz)
                if total < best.get(node, float('inf')):
                    best[node] = total
                    parents[node] = here
                    heapq.heappush(queue, (total + h(node), node))
        self.explored = len(done)
        arrived = found is not None
        if found is None:
            if not best:
                return [start], False
            found = min(best, key=h)
        path = [found]
        while parents[path[-1]] is not None:
            path.append(parents[path[-1]])
        points = [start] + [self.point(n) for n in reversed(path)]
        # Straighten: keep a point only where the straight line from the last kept one is blocked.
        kept = [points[0]]
        i = 0
        while i < len(points) - 1:
            j = min(len(points) - 1, i + 10)
            while j > i + 1 and not self.clear(points[i], points[j]):
                j -= 1
            kept.append(points[j])
            i = j
        return kept, arrived
