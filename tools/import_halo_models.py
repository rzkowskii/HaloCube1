#!/usr/bin/env python3
"""Bake Halo CE Xbox A30's first-person and biped art into AssaultCube MD3 models."""

import argparse
from array import array
from bisect import bisect_right
from dataclasses import dataclass
import math
import mmap
from pathlib import Path
import shutil
import struct

from halo_map_art import decode_bitmap, write_png
from import_halo_a30 import HaloMap


HANDS = r'characters\cyborg\fp\fp'
VIEW_MODELS = {
    'ma5b': ('assault rifle', 'firing', 'reload-full'),
    'pistol': ('pistol', 'fire-1', 'reload-full'),
    'plasma-rifle': ('plasma rifle', 'fire-1', None),
}
WORLD_MODELS = {
    'ma5b-world': 'assault rifle',
    'plasma-rifle-world': 'plasma rifle',
    'plasma-pistol-world': 'plasma pistol',
}
BIPEDS = {
    'cyborg': (r'characters\cyborg\cyborg', r'characters\cyborg\cyborg',
               'stand rifle idle', 'stand rifle move-front', 'stand rifle ar fire-1'),
    'elite': (r'characters\elite\elite', r'characters\elite\elite',
              'stand pistol idle%0', 'stand pistol move-front', 'stand pistol pp fire-1'),
    'grunt': (r'characters\grunt\grunt', r'characters\grunt\grunt',
              'stand pistol idle%0', 'stand pistol move-front%0', 'stand pistol pp fire-1'),
    'marine': (r'characters\marine_armored\marine_armored', r'characters\marine\marine',
               'stand rifle idle', 'stand rifle move-front', 'stand rifle ar fire-1'),
}


def skin(halo, bitmap, destination, tint=None):
    try:
        width, height, (pixels,) = decode_bitmap(halo, bitmap)
    except ValueError:
        width, height, faces = decode_bitmap(halo, bitmap, cube=True)
        pixels = faces[0]
    if tint:
        # The Xbox model shader uses the multipurpose map's blue channel
        # to interpolate between unmodified diffuse colour and actor tint.
        mask_tag, color = tint
        w, h, (mask,) = decode_bitmap(halo, mask_tag)
        for y in range(height):
            for x in range(width):
                offset = (y*width+x)*4
                amount = mask[((y*h//height)*w + x*w//width)*4+2] / 255
                for channel in range(3):
                    pixels[offset+channel] = round(pixels[offset+channel] * (1 - amount*(1-color[channel])))
    write_png(destination, width, height, pixels)


def write_digits(halo, bitmap, destination):
    if len(halo.tags.block(bitmap.address + 96, 48)) != 10:
        raise ValueError('Expected the ten authored MA5B counter bitmaps')
    for digit in range(10):
        width, height, (pixels,) = decode_bitmap(halo, bitmap, image=digit)
        # Chicago adds the illuminated digits over the base display plate.
        # The AC model uses an alpha cutout to retain the source silhouette.
        for offset in range(0, len(pixels), 4):
            intensity = max(pixels[offset:offset+3])
            pixels[offset+3] = 255 if intensity > 24 else 0
        write_png(destination / f'digit_{digit}.png', width, height, pixels)


@dataclass(frozen=True)
class Transform:
    # A row-major rotation/scale and a position, as a 3×4 affine transform.
    rows: tuple
    origin: tuple

    def vector(self, vector):
        return tuple(sum(a*b for a, b in zip(row, vector)) for row in self.rows)

    def point(self, point):
        return tuple(a+b for a, b in zip(self.vector(point), self.origin))

    def compose(self, other):
        columns = tuple(zip(*other.rows))
        rows = tuple(tuple(sum(a*b for a, b in zip(row, column))
                           for column in columns) for row in self.rows)
        return Transform(rows, self.point(other.origin))


def transform(point, rotation, scale):
    x, y, z, w = rotation
    norm = x*x + y*y + z*z + w*w
    q = 2/norm if norm else 0
    rows = (
        (1-q*(y*y+z*z), q*(x*y+w*z), q*(x*z-w*y)),
        (q*(x*y-w*z), 1-q*(x*x+z*z), q*(y*z+w*x)),
        (q*(x*z+w*y), q*(y*z-w*x), 1-q*(x*x+y*y)),
    )
    return Transform(tuple(tuple(scale*a for a in row) for row in rows), point)


def lookup(halo, group, name):
    for tag in halo.directory:
        if tag.group == group and tag.name == name:
            return tag.address
    raise ValueError(f'Missing {group} tag {name}')


class AnimationGraph:
    def __init__(self, halo, name, nodes=()):
        self.tags = halo.tags
        body = lookup(halo, 'antr', name)
        self.nodes = tuple((self.tags.name(node, 32), self.tags.read('h', node + 36)[0])
                           for node in self.tags.block(body + 104, 64)) or tuple(nodes)
        self.animations = {self.tags.name(a, 32): a for a in self.tags.block(body + 116, 180)}

    def tracks(self, name):
        tags = self.tags
        address = self.animations[name]
        kind, count, stride = tags.read('3H', address + 32)
        num_nodes, = tags.read('H', address + 44)
        flags, = tags.read('H', address + 58)
        if kind not in (0, 1) or num_nodes != len(self.nodes) or not 0 < count <= 1024:
            raise ValueError(f'Unsupported animation track: {name}')
        channels = (('4h', tags.read('Q', address + 108)[0]),
                    ('3f', tags.read('Q', address + 92)[0]),
                    ('f', tags.read('Q', address + 124)[0]))
        default_bytes, _, _, default = tags.read('4I', address + 140)
        frame_bytes, _, _, frames = tags.read('4I', address + 160)
        if flags & 1:
            offset, = tags.read('I', address + 136)
            yield from self.compressed_tracks(frames + offset, frame_bytes - offset, count, num_nodes, channels)
            return
        if count * stride != frame_bytes:
            raise ValueError(f'Animation frame length does not match track: {name}')
        for frame in range(count):
            cursor = [default, frames + stride*frame]
            local_tracks = []
            for i in range(num_nodes):
                attributes = []
                for fmt, mask in channels:
                    source = (mask >> i) & 1
                    if kind == 0 or source:
                        attributes.append(tags.read(fmt, cursor[source]))
                        cursor[source] += struct.calcsize('<'+fmt)
                    else:
                        attributes.append(None)
                rotation, point, scale = attributes
                quaternion = tuple(component/32767 for component in rotation) if rotation else None
                local_tracks.append((quaternion, point, scale[0] if scale else None))
            if ((kind == 0 and cursor[0] != default+default_bytes)
                    or cursor[1] != frames+stride*(frame+1)):
                raise ValueError(f'Animation channels do not match track sizes: {name}')
            yield local_tracks

    def compressed_tracks(self, start, length, count, nodes, channels):
        if length < 44:
            raise ValueError('Truncated compressed animation')
        tags = self.tags

        def read(fmt, offset):
            if not 0 <= offset <= length - struct.calcsize('<' + fmt):
                raise ValueError('Compressed animation offset exceeds its track')
            return tags.read(fmt, start + offset)

        r_time, r_base, r_data, t_node, t_time, t_base, t_data, s_node, s_time, s_base, s_data = read('11I', 0)

        def quaternion(offset):
            a, b, c = read('3H', offset)
            components = ((a >> 12) | (a & 0xfff0),
                          ((b >> 4) & 0xff0) | (a & 15) | (a << 12),
                          ((((c >> 4) & 0xf00) | (b & 0xf0)) >> 4) | (b << 8),
                          ((c >> 8) & 15) | (c << 4))
            return tuple((((v + 32768) & 65535) - 32768) / 32767 for v in components)

        def tracks(channel, header, timing, default, data, stride, decode, unit):
            mask = channels[channel][1]
            active = 0
            for node in range(nodes):
                base = unit if channel == 2 else decode(default + node * stride)
                times, keys = [0], [base]
                if mask >> node & 1:
                    entry, = read('I', header + active * 4)
                    index, key_count = entry >> 12, entry & 0xfff
                    if channel == 2:
                        base = decode(default + active * stride)
                        keys[0] = base
                    for key in range(index, index + key_count):
                        tick, = read('H', timing + 2 * key)
                        if tick >= count or tick < times[-1]:
                            raise ValueError('Invalid compressed animation keyframe')
                        value = decode(data + stride * key)
                        if tick == times[-1]:
                            keys[-1] = value
                        else:
                            times.append(tick)
                            keys.append(value)
                    active += 1
                yield times, keys

        def sample(times, values, frame, rotation=False):
            index = bisect_right(times, frame) - 1
            current = values[index]
            if index + 1 < len(values) and times[index] != frame:
                next_value = values[index + 1]
                fraction = (frame - times[index]) / (times[index + 1] - times[index])
                sign = -1 if rotation and sum(a*b for a, b in zip(current, next_value)) < 0 else 1
                current = tuple(a * (1 - fraction) + b * sign * fraction for a, b in zip(current, next_value))
            if rotation:
                norm = math.sqrt(sum(v*v for v in current))
                return tuple(v/norm for v in current) if norm else (0, 0, 0, 1)
            return current

        rotations = list(tracks(0, 44, r_time, r_base, r_data, 6, quaternion, None))
        translations = list(tracks(1, t_node, t_time, t_base, t_data, 12, lambda p: read('3f', p), None))
        scales = list(tracks(2, s_node, s_time, s_base, s_data, 4, lambda p: read('f', p), (1.0,)))
        for frame in range(count):
            yield [(sample(*rotations[node], frame, True), sample(*translations[node], frame),
                    sample(*scales[node], frame)[0]) for node in range(nodes)]

    def poses(self, name, base=None, bind=None):
        idle = next(self.tracks(base)) if base else bind
        for tracks in self.tracks(name):
            poses = {}
            for i, (node, parent) in enumerate(self.nodes):
                rotation, point, scale = tracks[i]
                if idle:
                    base_rotation, base_point, base_scale = idle[i]
                    base_transform = transform(base_point, base_rotation, base_scale)
                    # Halo adds overlay translation and post-multiplies the
                    # base rotation matrix by the overlay's local rotation.
                    overlay = transform((0, 0, 0), rotation or (0, 0, 0, 1), scale if scale is not None else 1)
                    posed = base_transform.compose(overlay)
                    origin = tuple(a+b for a, b in zip(base_point, point)) if point else base_point
                    local = Transform(posed.rows, origin)
                else:
                    local = transform(point, rotation, scale)
                if i:
                    if not 0 <= parent < i:
                        raise ValueError(f'Invalid parent for {node}: {parent}')
                    local = poses[self.nodes[parent][0]].compose(local)
                poses[node] = local
            yield poses


@dataclass(frozen=True)
class Mesh:
    name: str
    bitmap: object
    tint: object
    triangles: list
    vertices: list


class Model:
    def __init__(self, halo, name, prefix, palette=None):
        self.halo = halo
        tags = halo.tags
        body = lookup(halo, 'mode', name)
        self.nodes = []
        self.hierarchy = []
        self.local_bind = []
        self.bind_pose = {}
        for node in tags.block(body + 184, 156):
            node_name = tags.name(node, 32)
            scale, *rows_and_origin = tags.read('13f', node + 104)
            axes = [rows_and_origin[i:i+3] for i in (0, 3, 6)]
            inverse_bind = Transform(tuple(tuple(scale*v for v in axis) for axis in zip(*axes)),
                                     tuple(rows_and_origin[9:12]))
            parent, = tags.read('h', node + 36)
            x, y, z, qx, qy, qz, qw = tags.read('7f', node + 40)
            self.hierarchy.append((node_name, parent))
            self.local_bind.append(((qx, qy, qz, qw), (x, y, z), 1))
            bind = transform((x, y, z), (qx, qy, qz, qw), 1)
            self.bind_pose[node_name] = bind if parent == -1 else self.bind_pose[self.nodes[parent][0]].compose(bind)
            self.nodes.append((node_name, inverse_bind))
        self.markers = {}
        for marker in tags.block(body + 172, 64):
            for instance in tags.block(marker + 52, 32):
                _, perm, node = tags.read('3B', instance)
                if not perm:
                    marker_name = tags.name(marker, 32)
                    self.markers[marker_name] = (node, tags.read('3f', instance + 4), tags.read('4f', instance + 16))
                    break

        geometries = tags.block(body + 208, 48)
        materials = tags.block(body + 220, 32)
        u_scale, v_scale = tags.read('2f', body + 48)
        self.meshes = []
        digit_planes = set()
        for region in tags.block(body + 196, 76):
            perms = tags.block(region + 64, 88)
            if not perms:
                continue
            lod, = tags.read('h', perms[0] + 72)
            if not 0 <= lod < len(geometries):
                raise ValueError('Invalid highest-detail model permutation')
            for part in tags.block(geometries[lod] + 36, 104):
                shader_id, = tags.read('h', part + 4)
                if not 0 <= shader_id < len(materials):
                    raise ValueError('Invalid model shader index')
                shader = halo.reference(tags, materials[shader_id])
                if not shader:
                    continue
                bitmap, material_uv, tint = material(halo, shader, palette)
                if not bitmap:
                    continue
                scale_u, scale_v = material_uv
                mesh_name = f'{prefix}_{len(self.meshes)}'
                if name == r'weapons\assault rifle\fp\fp' and shader.name.endswith(r'\numbers'):
                    digit_plane, = tags.read('h', materials[shader_id] + 16)
                    if digit_plane not in (0, 1) or digit_plane in digit_planes:
                        raise ValueError('Unexpected MA5B counter geometry')
                    mesh_name = ('counter_units', 'counter_tens')[digit_plane]
                    digit_planes.add(digit_plane)
                mesh = self.mesh(part, mesh_name, bitmap, tint,
                                 (u_scale*scale_u, v_scale*scale_v))
                if mesh:
                    self.meshes.append(mesh)

    def mesh(self, part, name, bitmap, tint, uv):
        tags = self.halo.tags
        triangle_type, _, count, indices = tags.read('HHII', part + 68)
        vertex_type, _, num_vertices = tags.read('HHI', part + 84)
        if not count or not num_vertices:
            return None
        if vertex_type != 5 or triangle_type not in (0, 1) or not 0 < num_vertices <= 65535:
            raise ValueError(f'Unsupported Xbox model part: {vertex_type}, {triangle_type}, {num_vertices}')
        descriptor, = tags.read('I', part + 100)
        source, = tags.read('I', descriptor + 4)
        vertices = []
        for i in range(num_vertices):
            x, y, z, packed_normal, _, _, u, v, b0, b1, w = tags.read('3fIIIhhBBh', source + i*32)
            normal = decode_normal(packed_normal)
            position = (x, y, z)
            if name.startswith('counter_'):
                # Xbox submits these digits as additive overlays. Lift the
                # cutout planes slightly above AC's depth-writing display.
                position = tuple(p + 0.0015*n for p, n in zip(position, normal))
            bones = []
            for index, weight in ((b0, w/32767), (b1, 1-w/32767)):
                if weight:
                    if index % 3 or index//3 >= len(self.nodes) or not 0 < weight <= 1:
                        raise ValueError(f'Invalid Xbox skinning influence: {index}, {weight}')
                    bones.append((index//3, weight))
            vertices.append((position, normal, (uv[0]*(2*u+1)/65535, uv[1]*(2*v+1)/65535), bones))
        if triangle_type:
            corners = tags.read(f'{count+2}H', indices)
            source_triangles = (corners[i:i+3] if not i%2 else
                                (corners[i+1], corners[i], corners[i+2]) for i in range(count))
        else:
            corners = tags.read(f'{count*3}H', indices)
            source_triangles = (corners[i:i+3] for i in range(0, len(corners), 3))
        triangles = []
        for a, b, c in source_triangles:
            if max(a, b, c) >= num_vertices:
                raise ValueError('Model triangle exceeds the part vertex count')
            if len({a, b, c}) == 3:
                # Xbox triangles are clockwise; the Halo-to-AC Y reflection
                # flips them, so the original index order becomes CCW in AC.
                triangles.append((a, b, c))
        return Mesh(name, bitmap, tint, triangles, vertices) if triangles else None


def material(halo, shader, palette):
    tags = halo.tags
    address = shader.address
    uv = (1, 1)
    tint = None
    if shader.group == 'soso':
        uv = tuple(n or 1 for n in tags.read('2f', address + 156))
        offset = address + 164
        tint_source, = tags.read('h', address + 76)
        if palette and tint_source in palette:
            mask = halo.reference(tags, address + 188)
            if mask:
                tint = (mask, palette[tint_source])
    elif shader.group in ('schi', 'sotr'):
        is_chicago = shader.group == 'schi'
        maps = tags.block(address + 84, 220 if is_chicago else 100)
        if not maps:
            return None, uv, tint
        start = maps[0]
        uv = tuple(n or 1 for n in tags.read('2f', start + (84 if is_chicago else 4)))
        offset = start + (108 if is_chicago else 28)
    elif shader.group == 'smet':
        offset = address + 76
    elif shader.group == 'senv':
        offset = address + 136
    else:
        return None, uv, tint
    return halo.reference(tags, offset, 'bitm'), uv, tint


def decode_normal(packed):
    def component(shift, bits):
        half = 1 << (bits-1)
        value = (packed >> shift) & (2*half-1)
        value = value - 2*half if value >= half else value
        return (value*2+1)/(2*half-1)
    return component(0, 11), component(11, 11), component(22, 10)


def md3_axes(vector):
    x, y, z = vector
    return -y, x, z


def normal_code(normal):
    x, y, z = md3_axes(normal)
    length = math.sqrt(x*x + y*y + z*z)
    azimuth = round(math.atan2(y, x) * 255/math.tau) & 255 if length else 0
    polar = round(math.acos(max(-1, min(1, z/length))) * 255/math.tau) & 255 if length else 0
    return (azimuth << 8) | polar


def pack_vertex(point, normal):
    return struct.pack('<3hH', *(round(coord*64) for coord in point), normal_code(normal))


# MD3 vertices are signed 16-bit in 1/64 units (pack_vertex). export keeps every packed coordinate
# within MD3_LIMIT, a little under 32767/64 so rounding cannot reach the edge.
MD3_LIMIT = 32700 / 64
# The frame name that marks an MD3 frame whose local_origin the engine adds to the frame's vertices
# at load (md3.h): export's per-frame origins for clips whose root motion leaves the 16-bit range.
# Stock MD3s never use it, so they load as before.
ORIGIN_FRAME = b'halo_origin'


def posed_vertices(model, mesh, frames):
    """Every frame's skinned vertex positions (MD3 axes, before the model scale; a flat array of
    doubles, three per vertex) and packed normals, so export can pick the geometry scale from the
    posed extent before packing anything."""
    positions, normals = array('d'), array('H')
    for pose, _ in frames:
        bones = [pose[name].compose(inverse) for name, inverse in model.nodes]
        for vertex, normal, _, influences in mesh.vertices:
            position = [0.0, 0.0, 0.0]
            direction = [0.0, 0.0, 0.0]
            for index, weight in influences:
                bone = bones[index]
                p = bone.point(vertex)
                n = bone.vector(normal)
                for i in range(3):
                    position[i] += weight*p[i]
                    direction[i] += weight*n[i]
            positions.extend(md3_axes(position))
            normals.append(normal_code(direction))
    return positions, normals


def mesh_frames(posed, frames, model_scale, origins=None):
    """Pack the posed vertices at model_scale. With per-frame `origins` (MD3 axes, before the scale),
    each frame's vertices are packed relative to its origin, which the frame record carries
    (ORIGIN_FRAME); the bounds stay those of the absolute positions."""
    positions, normals = posed
    output = bytearray()
    count = len(normals) // max(1, len(frames))
    for f, (_, frame_bounds) in enumerate(frames):
        origin = origins[f] if origins else (0.0, 0.0, 0.0)
        for i in range(f*count, (f+1)*count):
            point = tuple(coord * model_scale for coord in positions[3*i:3*i+3])
            for axis, coordinate in enumerate(point):
                frame_bounds[axis] = min(frame_bounds[axis], coordinate)
                frame_bounds[axis+3] = max(frame_bounds[axis+3], coordinate)
            frame_bounds[6] = max(frame_bounds[6], math.sqrt(sum(v*v for v in point)))
            if origins:
                point = tuple((coord - o) * model_scale for coord, o in zip(positions[3*i:3*i+3], origin))
            output.extend(struct.pack('<3hH', *(round(coord*64) for coord in point), normals[i]))
    return bytes(output)


def frame_origins(surfaces, count):
    """Each frame's origin for ORIGIN_FRAME packing: the centre of the box round that frame's posed
    vertices over every surface (MD3 axes, before the model scale), and the largest half extent of a
    frame's box, which is what then has to fit the 16-bit range."""
    origins, half = [], 0.0
    for f in range(count):
        low, high = [math.inf]*3, [-math.inf]*3
        for _, (positions, normals) in surfaces:
            n = len(normals) // count
            frame = positions[3*n*f:3*n*(f+1)]
            for axis in range(3):
                values = frame[axis::3]
                if values:
                    low[axis], high[axis] = min(low[axis], min(values)), max(high[axis], max(values))
        origins.append(tuple((a + b)/2 if a <= b else 0.0 for a, b in zip(low, high)))
        half = max([half] + [(b - a)/2 for a, b in zip(low, high) if a <= b])
    return origins, half


def marker_frames(model, markers, frames, model_scale):
    records = []
    for pose, _ in frames:
        for name, marker in markers:
            index, point, rotation = model.markers[marker]
            bone = pose[model.nodes[index][0]]
            # Inverse bind applies only to the vertices. Marker transforms
            # are already relative to their named node.
            matrix = bone.compose(transform(point, rotation, 1))
            point = tuple(coord * model_scale for coord in md3_axes(matrix.origin))
            axis = ((0, -1, 0), (1, 0, 0), (0, 0, 1))
            axes = [md3_axes(matrix.vector(basis)) for basis in axis]
            records.append(struct.pack('<64s12f', name.encode(), *point, *axes[0], *axes[1], *axes[2]))
    return b''.join(records)


def export(halo, directory, name, models, graph, clips, scale, markers=(), muzzle_flash=None, **options):
    """Write directory/name/tris.md3, its md3.cfg and skins; the mdlscale it used (write_export).
    An export that fails removes the directory it created, so no skins-only directory is left."""
    destination = directory / name
    created = not destination.exists()
    try:
        return write_export(halo, destination, name, models, graph, clips, scale, markers, muzzle_flash, **options)
    except Exception:
        if created:
            shutil.rmtree(destination, ignore_errors=True)
        raise


def write_export(halo, destination, name, models, graph, clips, scale, markers, muzzle_flash, *,
                 quiet=False, geometry_scale=100, default_pose=None, position_poses=()):
    destination.mkdir(parents=True, exist_ok=True)
    frames = []
    anims = []
    if position_poses:
        frames.extend((pose, [math.inf]*3 + [-math.inf]*3 + [0.0]) for pose in position_poses)
        anims.append(f'md3anim "mapmodel" 0 {len(frames)} 30')
    for label, clip in clips:
        start = len(frames)
        poses = list(graph.poses(*clip)) if isinstance(clip, tuple) else list(graph.poses(clip))
        if label == 'lying dead':
            poses = poses[-1:]
        frames.extend((pose, [math.inf]*3 + [-math.inf]*3 + [0.0]) for pose in poses)
        rate = 45 if label in ('gun melee', 'gun throw') else 30
        anims.append(f'md3anim "{label}" {start} {len(poses)} {rate}')
    if not frames:
        pose = models[0].bind_pose if default_pose is None else default_pose
        frames.append((pose, [math.inf]*3 + [-math.inf]*3 + [0.0]))
    count = len(frames)
    surfaces = []
    skins = []
    skin_names = {}
    digits_written = False
    for model in models:
        for mesh in model.meshes:
            key = (mesh.bitmap, mesh.tint)
            if mesh.name.startswith('counter_'):
                if not digits_written:
                    write_digits(halo, mesh.bitmap, destination)
                    digits_written = True
                skin_name = 'digit_6.png' if mesh.name == 'counter_tens' else 'digit_0.png'
            elif key not in skin_names:
                skin_name = f'skin_{len(skin_names)}.png'
                try:
                    skin(halo, mesh.bitmap, destination/skin_name, mesh.tint)
                except (ValueError, IndexError):
                    if not quiet:
                        raise
                    continue
                skin_names[key] = skin_name
            else:
                skin_name = skin_names[key]
            skins.append(f'md3skin {mesh.name} {skin_name}')
            surfaces.append((mesh, posed_vertices(model, mesh, frames)))
    if not surfaces:
        raise ValueError(f'No supported mesh surfaces for {name}')
    # geometry_scale is the smallest mdlscale the caller wants. A clip whose posed frames reach past
    # the MD3 range at that scale (root motion: c10's x50 Chief and marines, d20's jump down, a50's
    # lift, a10's and a30's lifepods flying off) is packed per frame about the centre of that frame's
    # box (frame_origins), which its frame record carries for the engine to add back at load
    # (ORIGIN_FRAME, md3.h), so the vertex step stays that of the model's own size. Only when a
    # single frame's box is still too big does the scale rise, to the smallest that fits; the cfg's
    # mdlscale divides it back out, so the model draws the same size. Every export that fits packs
    # byte for byte as before.
    low = min((min(positions) for _, (positions, _) in surfaces if positions), default=0.0)
    high = max((max(positions) for _, (positions, _) in surfaces if positions), default=0.0)
    model_scale = scale*1600/geometry_scale
    origins = None
    if round(low*model_scale*64) < -32768 or round(high*model_scale*64) > 32767:
        origins, half = frame_origins(surfaces, count)
        if half * model_scale > MD3_LIMIT * 0.999:
            geometry_scale = math.ceil(half * scale*1600 / (MD3_LIMIT * 0.999))
            model_scale = scale*1600/geometry_scale
    mesh_data = []
    for mesh, posed in surfaces:
        triangles = b''.join(struct.pack('<3i', *row) for row in mesh.triangles)
        uv = b''.join(struct.pack('<2f', *vertex[2]) for vertex in mesh.vertices)
        vertices = mesh_frames(posed, frames, model_scale, origins)
        ofs_uv = 108 + len(triangles)
        ofs_vertices = ofs_uv + len(uv)
        mesh_data.append(
            struct.pack('<4s64s10i', b'IDP3', mesh.name.encode(), 0, count, 0,
                        len(mesh.vertices), len(mesh.triangles), 108, ofs_uv, ofs_uv,
                        ofs_vertices, ofs_vertices + len(vertices))
            + triangles + uv + vertices
        )
    if origins:
        frame_data = b''.join(struct.pack('<10f16s', *bounds[:6], *(c * model_scale for c in origin), bounds[6], ORIGIN_FRAME)
                              for (_, bounds), origin in zip(frames, origins))
    else:
        frame_data = b''.join(struct.pack('<10f16s', *bounds[:6], 0, 0, 0, bounds[6], b'')
                              for _, bounds in frames)
    tags = marker_frames(models[0], markers, frames, scale*1600/geometry_scale)
    meshes = b''.join(mesh_data)
    ofs_tags = 108 + len(frame_data)
    ofs_meshes = ofs_tags + len(tags)
    header = struct.pack('<4si64s9i', b'IDP3', 15, name.encode(), 0, count, len(markers), len(mesh_data),
                         0, 108, ofs_tags, ofs_meshes, ofs_meshes+len(meshes))
    (destination/'tris.md3').write_bytes(header + frame_data + tags + meshes)
    commands = ['md3load tris.md3', f'mdlscale {geometry_scale}', 'mdlalphatest 40', *skins, *anims]
    if muzzle_flash:
        commands.append('md3emit tag_muzzle ' + muzzle_flash)
    (destination/'md3.cfg').write_text('\n'.join(commands) + '\n', encoding='ascii')
    if not quiet:
        print(f'{name}: {count} frames, {len(mesh_data)} surfaces, {len(header + frame_data + tags + meshes)} bytes')
    return geometry_scale


def convert(halo, directory):
    arms = Model(halo, HANDS, 'arms')
    for name, (weapon, fire, reload) in VIEW_MODELS.items():
        model = rf'weapons\{weapon}\fp\fp'
        clips = [('gun idle', 'first-person idle'), ('gun shoot', 'first-person ' + fire),
                 ('gun throw', 'first-person throw-grenade'), ('gun melee', 'first-person melee')]
        if reload:
            clips.append(('gun reload', 'first-person ' + reload))
        flash = {'ma5b': 'HUDMUZZLEFLASH 50 75', 'pistol': 'HUDMUZZLEFLASH 50 25'}.get(name)
        export(halo, directory, name, [Model(halo, model, 'weapon'), arms],
               AnimationGraph(halo, model), clips, 4.5, [('tag_muzzle', 'primary trigger')], flash)
    for name, weapon in WORLD_MODELS.items():
        model = rf'weapons\{weapon}\{weapon}'
        flash = 'MUZZLEFLASH 50 65' if name == 'ma5b-world' else None
        export(halo, directory, name, [Model(halo, model, 'weapon')], None, (), 5.6,
               [('tag_muzzle', 'primary trigger')], flash)
    for name, (model, animation, idle, run, attack) in BIPEDS.items():
        palette = {}
        region = halo.tags
        if name == 'elite':
            variant = lookup(halo, 'actv', r'characters\elite\elite minor\elite minor plasma rifle')
            colors = region.block(variant + 556, 32)
            palette[1] = region.read('3f', colors[0])
        elif name in ('cyborg', 'grunt'):
            biped = lookup(halo, 'bipd', model)
            colors = region.block(biped + 356, 44)
            for source, color in enumerate(colors, 1):
                perms = region.block(color + 32, 28)
                if perms:
                    palette[source] = region.read('3f', perms[0] + 4)
        clips = [('idle', idle), ('run', run), ('attack', (attack, idle)), ('death', 'stand landing-dead'),
                 ('lying dead', 'stand landing-dead')]
        hand = 'right hand elite' if name == 'elite' else 'right hand'
        export(halo, directory, name, [Model(halo, model, 'biped', palette)],
               AnimationGraph(halo, animation), clips, 5.6, [('tag_head', 'head'), ('tag_weapon', hand)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('map', type=Path, help='expanded Xbox Halo CE a30.map')
    parser.add_argument('destination', type=Path, help='root of the AssaultCube runtime or mod')
    args = parser.parse_args()
    try:
        with args.map.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            convert(HaloMap(data), args.destination / 'packages/models/halo')
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'Import failed: {error}\n')


if __name__ == '__main__':
    main()
