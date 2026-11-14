"""Retail scenario machines, controls, and their position animations."""
from import_halo_models import AnimationGraph, Model, export


def animation(halo, tag, model):
    tags = halo.tags
    reference = halo.reference(tags, tag.address + 56, 'antr')
    if reference:
        graph = AnimationGraph(halo, reference.name, model.hierarchy)
        for device in tags.block(reference.address + 48, 96):
            slots = tags.block(device + 84, 2)
            if not slots:
                continue
            index, = tags.read('h', slots[0])
            names = list(graph.animations)
            if 0 <= index < len(names):
                clip = names[index]
                kind, = tags.read('H', graph.animations[clip] + 32)
                return list(graph.poses(clip, bind=model.local_bind if kind == 1 else None))
    return [model.bind_pose]


def collision_frames(halo, tag, model, poses):
    tags = halo.tags
    reference = halo.reference(tags, tag.address + 112, 'coll')
    if not reference:
        return []
    nodes = []
    for index, node in enumerate(tags.block(reference.address + 652, 64)):
        if index >= len(model.nodes):
            break
        points = []
        for bsp in tags.block(node + 52, 96):
            points.extend(tags.read('3f', vertex) for vertex in tags.block(bsp + 84, 16))
        if points:
            nodes.append((index, model.nodes[index][0], points))
    result = []
    for index, name, points in nodes:
        frames = []
        for pose in poses:
            axes = list(zip(*(pose[name].point(point) for point in points)))
            frames.append(tuple(map(min, axes)) + tuple(map(max, axes)))
        result.append((index, frames))
    return result


def structure_leaves(region, address, point):
    # A placement's origin may lie in a wall. Sample around it before falling
    # back to the BSP bounds, which can overlap across section boundaries.
    collision = region.block(address + 176, 96)
    if not collision:
        return False
    nodes = region.block(collision[0], 12)
    planes = region.block(collision[0] + 12, 16)
    if not nodes:
        return False
    for dx, dy, dz in ((0, 0, 0), (0, 0, .15), (.35, 0, .15), (-.35, 0, .15),
                       (0, .35, .15), (0, -.35, .15), (0, 0, .6)):
        x, y, z = point[0] + dx, point[1] + dy, point[2] + dz
        node = 0
        while 0 <= node < len(nodes):
            plane, back, front = region.read('3i', nodes[node])
            if not 0 <= plane < len(planes):
                break
            a, b, c, d = region.read('4f', planes[plane])
            node = front if a*x + b*y + c*z >= d else back
        if node < -1:
            return True
    return False


def device_rows(halo, output, rebuild, geometry_scale):
    tags, scenario = halo.tags, halo.scenario
    directory = output / 'packages/models/halo/scene' / halo.name
    bsps = [halo.bsp(i) for i in range(len(tags.block(scenario + 1444, 32)))]
    bounds = [region.read('6f', address + 200) for region, address in bsps]
    assets = {}
    for placement_block, palette_block, group in ((660, 672, 'mach'), (684, 696, 'ctrl')):
        palette = tags.block(scenario + palette_block, 48)
        for placement in tags.block(scenario + placement_block, 64):
            variant, name = tags.read('2h', placement)
            if not 0 <= variant < len(palette):
                continue
            tag = halo.reference(tags, palette[variant], group)
            if not tag:
                continue
            a = tag.address
            pose = tags.read('6f', placement + 8)
            power, position, flags, attributes = tags.read('2h2I', placement + 40)
            not_placed, = tags.read('H', placement + 4)
            kind, definition_flags = tags.read('2H', a + 656)
            elevator = tags.read('h', a + 746)[0] if group == 'mach' and definition_flags & 4 else -1
            hold = tags.read('i', a + 800)[0] if group == 'mach' else 0
            call = tags.read('f', a + 660)[0] if group == 'ctrl' else 0
            radius, = tags.read('f', a + 540)
            radius = radius or tags.read('f', a + 4)[0]
            rates = tags.read('6f', a + 628)
            loops, = tags.read('I', a + 380)
            candidates = [i for i, extent in enumerate(bounds)
                          if all(extent[2*j] - 1 <= coord <= extent[2*j+1] + 1 for j, coord in enumerate(pose[:3]))]
            inside = [i for i in candidates if structure_leaves(*bsps[i], pose[:3])]
            bsp_mask = sum(1 << i for i in inside or candidates)
            if a not in assets:
                reference = halo.reference(tags, a + 40, 'mode')
                path, frames, collision = '', 1, []
                if reference:
                    model = Model(halo, reference.name, 'part')
                    poses = animation(halo, tag, model)
                    frames = len(poses)
                    slug = f'device_{halo.directory.index(tag)}'
                    if model.meshes:
                        if rebuild or not (directory / slug / 'tris.md3').is_file():
                            try:
                                export(halo, directory, slug, [model], None, (), 4, quiet=True,
                                       geometry_scale=max(geometry_scale(model, animated=True),
                                       16 * int(1 + max(abs(v) for pose in poses for bone in pose.values() for v in bone.origin))),
                                       position_poses=poses)
                            except ValueError as error:
                                if not str(error).startswith('No supported mesh surfaces'):
                                    raise
                        if (directory / slug / 'tris.md3').is_file():
                            path = f'halo/scene/{halo.name}/{slug}'
                    if group == 'mach':
                        collision = collision_frames(halo, tag, model, poses)
                assets[a] = path, frames, collision
            path, frames, collision = assets[a]
            yield (name, power, position, elevator, flags, attributes, not_placed, loops, bsp_mask,
                   group == 'ctrl', kind, definition_flags, hold, call, radius, *rates,
                   path, frames, pose, collision)
