"""Decode selected Xbox Halo cache assets into AssaultCube's native media formats."""

import math
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib


def write_png(path, width, height, pixels):
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
    scanlines = b''.join(b'\0' + pixels[i:i + width * 4] for i in range(0, len(pixels), width * 4))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(scanlines)) + chunk(b'IEND', b''))


def decode_dxt(data, width, height, fmt):
    pixels = bytearray(width * height * 4)
    size = 8 if fmt == 14 else 16
    position = 0

    def rgb565(value):
        return ((value >> 11) * 255 // 31, (value >> 5 & 63) * 255 // 63, (value & 31) * 255 // 31)

    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            first, second, selectors = struct.unpack_from('<HHI', data, position + size - 8)
            color0, color1 = rgb565(first), rgb565(second)
            colors = [(*color0, 255), (*color1, 255)]
            if fmt != 14 or first > second:
                colors.append((*( (2 * a + b) // 3 for a, b in zip(color0, color1) ), 255))
                colors.append((*( (a + 2 * b) // 3 for a, b in zip(color0, color1) ), 255))
            else:
                colors.append((*( (a + b) // 2 for a, b in zip(color0, color1) ), 255))
                colors.append((0, 0, 0, 0))
            if fmt == 15:
                alpha_bits = struct.unpack_from('<Q', data, position)[0]
            elif fmt == 16:
                alpha0, alpha1 = data[position:position+2]
                steps = 7 if alpha0 > alpha1 else 5
                alpha = [alpha0, alpha1] + [((steps - i) * alpha0 + i * alpha1) // steps for i in range(1, steps)]
                if steps == 5:
                    alpha.extend([0, 255])
                alpha_bits = int.from_bytes(data[position+2:position+8], 'little')
            for y in range(min(4, height - by)):
                for x in range(min(4, width - bx)):
                    index = y * 4 + x
                    red, green, blue, opacity = colors[selectors >> (index * 2) & 3]
                    if fmt == 15:
                        opacity = (alpha_bits >> (index * 4) & 15) * 17
                    elif fmt == 16:
                        opacity = alpha[alpha_bits >> (index * 3) & 7]
                    pixel = ((by + y) * width + bx + x) * 4
                    pixels[pixel:pixel+4] = bytes((red, green, blue, opacity))
            position += size
    return pixels


def xbox_axes(*dimensions):
    # Interleave a bit only on axes that still have bits. Precompute each
    # axis separately so a pixel address needs only ORs in the inner loop.
    bits = [[] for _ in dimensions]
    interleave = 1
    for bit in range(max(dimensions).bit_length() - 1):
        for size, axis in zip(dimensions, bits):
            if 1 << bit < size:
                axis.append(interleave)
                interleave <<= 1
    return [[sum(mask for i, mask in enumerate(axis) if value & (1 << i))
             for value in range(size)] for size, axis in zip(dimensions, bits)]


def decode_pixels(data, width, height, depth, fmt, stride, swizzled):
    if swizzled:
        if any(size & (size - 1) for size in (width, height, depth)):
            raise ValueError('Swizzled bitmap dimensions are not powers of two')
        xs, ys, zs = xbox_axes(width, height, depth)
    else:
        xs = range(width)
        ys = range(0, width * height, width)
        zs = range(0, width * height * depth, width * height)
    # P8 bump PNGs retain the original indices as luminance for later
    # normal-map palette lookup. They aren't ordinary color images.
    if fmt in (0, 1, 2, 17):
        palette = tuple(bytes((255, 255, 255, i) if fmt == 0 else
                              (i, i, i, i if fmt == 2 else 255)) for i in range(256))
    else:
        palette = None
    for zi in zs:
        pixels = bytearray(width * height * 4)
        target = 0
        for yi in ys:
            for xi in xs:
                address = ((xi | yi | zi) if swizzled else (xi + yi + zi)) * stride
                if palette:
                    pixels[target:target+4] = palette[data[address]]
                elif fmt == 3:
                    intensity, alpha = data[address:address+2]
                    pixels[target:target+4] = bytes((intensity, intensity, intensity, alpha))
                elif fmt in (6, 8, 9):
                    value, = struct.unpack_from('<H', data, address)
                    if fmt == 6:
                        r, g, b, a = (value >> 11) * 255 // 31, (value >> 5 & 63) * 255 // 63, (value & 31) * 255 // 31, 255
                    elif fmt == 8:
                        r, g, b, a = (value >> 10 & 31) * 255 // 31, (value >> 5 & 31) * 255 // 31, (value & 31) * 255 // 31, (value >> 15) * 255
                    else:
                        r, g, b, a = (value >> 8 & 15) * 17, (value >> 4 & 15) * 17, (value & 15) * 17, (value >> 12) * 17
                    pixels[target:target+4] = bytes((r, g, b, a))
                else:
                    blue, green, red, alpha = data[address:address+4]
                    pixels[target:target+4] = bytes((red, green, blue, alpha if fmt == 11 else 255))
                target += 4
        yield pixels


def limit_bitmap(width, height, images, resolution):
    if not resolution or max(width, height) <= resolution:
        return width, height, images
    target_width, target_height = min(width, resolution), min(height, resolution)
    offsets = [4 * (y * height // target_height * width + x * width // target_width)
               for y in range(target_height) for x in range(target_width)]
    return target_width, target_height, [bytearray().join(image[i:i+4] for i in offsets) for image in images]


def decode_bitmap(halo, tag, cube=False, image=0, resolution=None, *, volume=False, indexed=False):
    descriptor = halo.tags.block(tag.address + 96, 48)[image]
    width, height, depth, kind, fmt, flags = halo.tags.read('6H', descriptor + 4)
    offset, size = halo.tags.read('2I', descriptor + 24)
    if kind != (2 if cube else 1 if volume else 0) or not 1 <= width <= 2048 or not 1 <= height <= 2048 \
            or not 1 <= depth <= (2048 if volume else 1):
        raise ValueError(f'Unsupported bitmap geometry for {tag.name}')
    if offset < 2048 or size <= 0 or offset + size > len(halo.data):
        raise ValueError(f'Bitmap exceeds the cache: {tag.name}')
    stride = {0: 1, 1: 1, 2: 1, 3: 2, 6: 2, 8: 2, 9: 2, 17: 1}.get(fmt, 4)
    if resolution and not (cube or volume):
        mipmaps, = halo.tags.read('H', descriptor + 20)
        for _ in range(mipmaps):
            if max(width, height) <= resolution or min(width, height) < 16:
                break
            length = width * height * stride
            if fmt in (14, 15, 16):
                length = (width // 4) * (height // 4) * (8 if fmt == 14 else 16)
            offset += length
            size -= length
            width //= 2
            height //= 2
    if fmt in (14, 15, 16) and not volume:
        count = 6 if cube else 1
        length = ((width + 3) // 4) * ((height + 3) // 4) * (8 if fmt == 14 else 16)
        stride = size // count
        if length > stride or size % count or (cube and stride % 128):
            raise ValueError(f'Invalid DXT payload for {tag.name}')
        images = [decode_dxt(halo.data[offset + i * stride:offset + i * stride + length], width, height, fmt)
                  for i in range(count)]
        return limit_bitmap(width, height, images, resolution if not cube else None)
    if fmt in (0, 1, 2, 3, 6, 8, 9, 10, 11) or fmt == 17 and indexed:
        count = 6 if cube else 1
        length = width * height * depth * stride
        face_stride = size // count
        if length > face_stride or size % count or (cube and face_stride % 128):
            raise ValueError(f'Invalid bitmap payload for {tag.name}')
        images = []
        for i in range(count):
            start = offset + i * face_stride
            images.extend(decode_pixels(halo.data[start:start+length], width, height, depth, fmt, stride, bool(flags & 8)))
        return limit_bitmap(width, height, images, resolution if not (cube or volume) else None)
    raise ValueError(f'Unsupported Halo bitmap format {fmt} for {tag.name}')


def filter_lightmap(width, height, pixels, resolution):
    target_w, target_h = min(width, resolution), min(height, resolution)
    colors = [(0, 0, 0, 255)] * (target_w * target_h)
    distance = [width + height] * len(colors)
    checker = {(255, 0, 0), (255, 255, 0)}
    for y in range(target_h):
        for x in range(target_w):
            samples = []
            for sy in range(y * height // target_h, (y + 1) * height // target_h):
                for sx in range(x * width // target_w, (x + 1) * width // target_w):
                    p = 4 * (sy * width + sx)
                    rgb = tuple(pixels[p:p+3])
                    if rgb in checker:
                        px, py = 4 * (sy * width + (sx ^ 1)), 4 * ((sy ^ 1) * width + sx)
                        if tuple(pixels[px:px+3]) == tuple(pixels[py:py+3]) == (255, 255 - rgb[1], 0):
                            continue
                    samples.append(rgb)
            if samples:
                i = y * target_w + x
                count = len(samples)
                colors[i] = (*((sum(c) + count // 2) // count for c in zip(*samples)), 255)
                distance[i] = 0

    # Extend the nearest lit texel into empty atlas space before bilinear sampling.
    for step, indexes in ((1, range(len(colors))), (-1, reversed(range(len(colors))))):
        for i in indexes:
            x, y = i % target_w, i // target_w
            for neighbor in (i - step if 0 <= x - step < target_w else i,
                             i - step * target_w if 0 <= y - step < target_h else i):
                if distance[neighbor] + 1 < distance[i]:
                    distance[i] = distance[neighbor] + 1
                    colors[i] = colors[neighbor]
    return target_w, target_h, bytearray().join(map(bytes, colors))


def bitmap(halo, name):
    return next(tag for tag in halo.directory if tag.group == 'bitm' and tag.name.endswith(name))


def write_materials(halo, output):
    names = {
        'grass': r'levels\a30\bitmaps\detail grass',
        'cliff': r'levels\a30\bitmaps\cliff rock tiled',
        'dirt': r'levels\a30\bitmaps\detail cliff rock smooth',
        'metal': r'levels\b30\bitmaps\metal plate floor ext',
        'water': r'sky\sky_a30\skydusk0\bitmaps\water dusk',
        'moss': r'levels\a30\bitmaps\detail grass',
    }
    tints = {'grass': (0.76, 0.51, 0.76), 'moss': (0.70, 0.44, 0.63), 'cliff': (0.83, 0.84, 0.84),
             'dirt': (0.98, 0.91, 0.77), 'metal': (0.95, 1, 1.05), 'water': (0.53, 0.79, 1.15)}
    for name, tag_name in names.items():
        width, height, (pixels,) = decode_bitmap(halo, bitmap(halo, tag_name))
        tint = tints[name]
        for i in range(0, len(pixels), 4):
            pixels[i:i+4] = bytes((*(min(255, round(pixels[i+c] * tint[c])) for c in range(3)), 255))
        write_png(output / (name + '.png'), width, height, pixels)


def xbox_cube_pixel(faces, size, x, y, z):
    ax, ay, az = abs(x), abs(y), abs(z)
    if ax >= ay and ax >= az:
        face, u, v, length = (0, -z, -y, ax) if x >= 0 else (1, z, -y, ax)
    elif ay >= az:
        face, u, v, length = (2, x, z, ay) if y >= 0 else (3, x, -z, ay)
    else:
        face, u, v, length = (4, x, -y, az) if z >= 0 else (5, -x, -y, az)
    ix = min(size - 1, int((u / length + 1) * size / 2))
    iy = min(size - 1, int((v / length + 1) * size / 2))
    position = (iy * size + ix) * 4
    return faces[face][position:position+4]


def write_sky(halo, output, tag=None):
    if tag is None:
        tag = bitmap(halo, r'sky\sky_a30\skydusk0\bitmaps\cubemap sky_dusk')
    size, height, faces = decode_bitmap(halo, tag, cube=True)
    if size != height:
        raise ValueError('Expected a square Halo sky face')
    # AC cube side directions are defined by draw_envbox(). Xbox cube
    # textures use D3D's Y-up directions and the map mirrors Halo's world Y.
    directions = {
        'lf': lambda s, t: (-1, s, t), 'rt': lambda s, t: (1, -s, t),
        'ft': lambda s, t: (-s, -1, t), 'bk': lambda s, t: (s, 1, t),
        'dn': lambda s, t: (-t, -s, 1), 'up': lambda s, t: (t, -s, -1),
    }
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='halo-a30-sky-') as directory:
        for side, direction in directions.items():
            pixels = bytearray()
            for row in range(size):
                for column in range(size):
                    s, t = (column + .5) * 2 / size - 1, (row + .5) * 2 / size - 1
                    x, y, z = direction(s, t)
                    pixels.extend(xbox_cube_pixel(faces, size, x, z, y))
            png = Path(directory) / (side + '.png')
            write_png(png, size, size, pixels)
            subprocess.run(['sips', '-s', 'format', 'jpeg', str(png), '--out', str(output / f'dusk_{side}.jpg')],
                           check=True, stdout=subprocess.DEVNULL)


SCENERY_MODELS = ('tree_pine', 'tree_pine_tall', 'boulder_granite_large',
                  'boulder_granite_gigantic', 'lifepod_crashed')


def quaternion_matrix(i, j, k, w):
    factor = 2 / (i*i + j*j + k*k + w*w)
    return ((1 - factor*(j*j+k*k), factor*(i*j+w*k), factor*(i*k-w*j)),
            (factor*(i*j-w*k), 1 - factor*(i*i+k*k), factor*(j*k+w*i)),
            (factor*(i*k+w*j), factor*(j*k-w*i), 1 - factor*(i*i+j*j)))


def transform_point(matrix, point, translation):
    return tuple(sum(a*b for a, b in zip(row, point)) + delta for row, delta in zip(matrix, translation))


def bind_matrices(halo, model):
    transforms = []
    for node in halo.tags.block(model.address + 184, 156):
        parent, = halo.tags.read('h', node + 36)
        position = halo.tags.read('3f', node + 40)
        axes = quaternion_matrix(*halo.tags.read('4f', node + 52))
        if parent >= 0:
            parent_axes, parent_position = transforms[parent]
            position = transform_point(parent_axes, position, parent_position)
            axes = tuple(tuple(sum(a*b for a, b in zip(row, column)) for column in zip(*axes)) for row in parent_axes)
        transforms.append((axes, position))
    return transforms


def bind_point(transforms, point, first, second, weight):
    def transform(node):
        if node == 253:
            return point
        axes, translation = transforms[node // 3]
        return transform_point(axes, point, translation)

    if second == 253 or weight == 32767:
        return transform(first)
    blend = weight / 32767
    return tuple(a * blend + b * (1 - blend) for a, b in zip(transform(first), transform(second)))


def model_meshes(halo, model):
    region = halo.tags
    scale_u, scale_v = region.read('2f', model.address + 48)
    transforms = bind_matrices(halo, model)
    geometries = region.block(model.address + 208, 48)
    shaders = region.block(model.address + 220, 32)
    for region_address in region.block(model.address + 196, 76):
        permutations = region.block(region_address + 64, 88)
        geometry, = region.read('h', permutations[0] + 72)
        for part in region.block(geometries[geometry] + 36, 104):
            shader_index, = region.read('h', part + 4)
            shader = halo.reference(region, shaders[shader_index])
            map_scale = (1, 1)
            additive = False
            if shader.group == 'soso':
                texture = halo.reference(region, shader.address + 164, 'bitm')
            elif shader.group in ('sotr', 'schi') and model.name.startswith('sky\\'):
                chicago = shader.group == 'schi'
                maps = region.block(shader.address + 84, 220 if chicago else 100)
                if not maps:
                    continue
                # The space Chicago shader layers a repeating star mask
                # over its base nebula. Use that base on the sky geometry.
                layer = maps[-1] if chicago else maps[0]
                texture = halo.reference(region, layer + (108 if chicago else 28), 'bitm')
                map_scale = tuple(v or 1 for v in region.read('2f', layer + (84 if chicago else 4)))
                additive = region.read('H', shader.address + 44)[0] == 3
            else:
                continue
            if not texture:
                continue
            primitive, count, indices, _, vertex_type, vertices, _, _, descriptor = region.read('9I', part + 68)
            primitive &= 0xffff
            if vertex_type & 0xffff != 5 or primitive not in (0, 1):
                raise ValueError(f'Unexpected model vertex/index format for {model.name}')
            vertex_address, = region.read('I', descriptor + 4)
            points, uv = [], []
            for i in range(vertices):
                address = vertex_address + i * 32
                point = region.read('3f', address)
                u, v, first, second, weight = region.read('hhBBh', address + 24)
                points.append(bind_point(transforms, point, first, second, weight))
                uv.append((u * scale_u * map_scale[0] / 32767, v * scale_v * map_scale[1] / 32767))
            triangles = []
            if primitive == 1:
                strip = region.read(f'{count + 2}H', indices)
                for i in range(count):
                    a, b, c = strip[i:i+3]
                    if a != b and b != c and c != a:
                        triangles.append((b, a, c) if i & 1 else (a, b, c))
            else:
                triangles = [region.read('3H', indices + i * 6) for i in range(count)]
            if not triangles or max(max(triangle) for triangle in triangles) >= vertices:
                raise ValueError(f'Model has invalid triangle indices: {model.name}')
            yield texture, points, uv, triangles, additive


def write_md3(path, name, meshes, xy_scale, z_scale, scale_percent):
    # MD3 uses 1/64 units. AC's loader swaps X/Y and divides by 16 after
    # mdlscale. The result is the Halo mesh in the mirrored AC level axes.
    render_meshes, all_vertices = [], []
    for i, (points, uv, triangles) in enumerate(meshes):
        units = 1600 / scale_percent
        verts = [(-y * xy_scale * units, x * xy_scale * units, z * z_scale * units) for x, y, z in points]
        all_vertices.extend(verts)
        # AssaultCube uses GL_FRONT culling. Reverse indices to compensate
        # for the authored Halo triangle convention after the axis conversion.
        tris = b''.join(struct.pack('<3I', a, c, b) for a, b, c in triangles)
        uvs = b''.join(struct.pack('<2f', *coord) for coord in uv)
        vertices = b''.join(struct.pack('<4h', *(round(v * 64) for v in vertex), 0) for vertex in verts)
        st = 108 + len(tris)
        xyz = st + len(uvs)
        end = xyz + len(vertices)
        header = struct.pack('<4s64s10I', b'IDP3', f'part{i}'.encode(), 0, 1, 0, len(verts), len(triangles),
                             108, st, st, xyz, end)
        render_meshes.append(header + tris + uvs + vertices)
    if not render_meshes:
        raise ValueError(f'No compatible MD3 meshes for {name}')
    axes = list(zip(*all_vertices))
    bounds = [min(axis) for axis in axes] + [max(axis) for axis in axes]
    radius = max(math.hypot(*p) for p in all_vertices)
    frame = struct.pack('<10f16s', *bounds, 0, 0, 0, radius, b'base')
    data = b''.join(render_meshes)
    header = struct.pack('<4sI64s9I', b'IDP3', 15, name.encode(), 0, 1, 0, len(meshes), 0,
                         108, 164, 164, 164 + len(data))
    path.write_bytes(header + frame + data)


def write_scenery(halo, output, xy_scale, z_scale):
    for name in SCENERY_MODELS:
        scenery = next(tag for tag in halo.directory if tag.group == 'scen' and tag.name.endswith('\\' + name))
        model = halo.reference(halo.tags, scenery.address + 40, 'mode')
        directory = output / name
        directory.mkdir(parents=True, exist_ok=True)
        scale_percent = 200 if name == 'tree_pine_tall' else 100
        command = ['md3load tris.md3', f'mdlscale {scale_percent}', 'mdlshadowdist 0']
        if name.startswith('tree_'):
            command.extend(('mdlcullface 0', 'mdlalphatest 38'))
        meshes, skins = [], {}
        for i, (texture, points, uv, triangles, _) in enumerate(model_meshes(halo, model)):
            if texture not in skins:
                filename = f'material_{len(skins)}.png'
                w, h, (pixels,) = decode_bitmap(halo, texture)
                write_png(directory / filename, w, h, pixels)
                skins[texture] = filename
            command.append(f'md3skin part{i} {skins[texture]}')
            meshes.append((points, uv, triangles))
        command.append('md3anim mapmodel 0 1 1')
        # MD3 vertices are 16-bit in 1/64 units of 16/mdlscale-percent: a model too big for that at
        # this scale (the Crash Site's retail layout, 4.1, against the authored 2.75 by 2.0) gets a
        # larger mdlscale, which leaves its size in the level unchanged.
        extent = max((abs(v) * max(xy_scale, z_scale) for points, _, _ in meshes for point in points for v in point), default=0)
        needed = math.ceil(extent * 1600 * 64 / 32000 / 25) * 25
        if needed > scale_percent:
            command[1] = f'mdlscale {needed}'
            scale_percent = needed
        write_md3(directory / 'tris.md3', name, meshes, xy_scale, z_scale, scale_percent)
        (directory / 'md3.cfg').write_text('\n'.join(command) + '\n')


def sampled_light(image, u, v):
    width, height, pixels = image
    x = max(0, min(width - 1, int(u * width)))
    y = max(0, min(height - 1, int(v * height)))
    # The retail RGB565 atlas marks unused cells with a red/yellow checker.
    for ox, oy in ((0, 0), (1, 0), (0, 1), (-1, 0), (0, -1), (2, 0), (0, 2), (-2, 0), (0, -2)):
        ix = max(0, min(width - 1, x + ox))
        iy = max(0, min(height - 1, y + oy))
        pixel = (iy * width + ix) * 4
        color = tuple(pixels[pixel:pixel+3])
        if color not in ((255, 0, 0), (255, 255, 0)):
            return tuple(max(56, min(255, round(42 + value * 1.15))) for value in color)
    return 156, 163, 174


def world_material(halo, shader):
    if shader.group == 'senv':
        flags, = halo.tags.read('H', shader.address + 40)
        return halo.reference(halo.tags, shader.address + 136, 'bitm'), 1 if flags & 1 else 0
    if shader.group == 'swat':
        return halo.reference(halo.tags, shader.address + 76, 'bitm'), 2
    if shader.group == 'schi' and any(part in shader.name for part in ('waterfall', 'river_front')):
        maps = halo.tags.block(shader.address + 84, 220)
        if maps:
            return halo.reference(halo.tags, maps[-1] + 108, 'bitm'), 2
    if shader.group == 'sotr' and halo.name == 'a10':
        maps = halo.tags.block(shader.address + 84, 100)
        blend, = halo.tags.read('H', shader.address + 44)
        if maps and blend == 3:
            return halo.reference(halo.tags, maps[0] + 28, 'bitm'), 4
    return None, 0


def autumn_coronas(halo, output):
    """Bake the original nozzle flare sprites and retain their world anchors."""
    tags = halo.tags
    rows, textures = [], {}
    output.mkdir(parents=True, exist_ok=True)
    for name, pose in halo.scenery():
        if not name.endswith(('\\small exhaust', '\\large exhaust')):
            continue
        scenery = next(tag for tag in halo.directory if tag.group == 'scen' and tag.name == name)
        attachments = tags.block(scenery.address + 320, 72)
        light = halo.reference(tags, attachments[0], 'ligh')
        flare = halo.reference(tags, light.address + 172, 'lens')
        reflection = tags.block(flare.address + 196, 128)[0]
        sample, = tags.read('h', reflection + 4)
        bitmap = halo.reference(tags, flare.address + 32, 'bitm')
        texture = textures.get(name)
        if texture is None:
            texture = f'packages/textures/halo_scene_a10_07/bsp/corona_{len(textures)}.png'
            width, height, (pixels,) = decode_bitmap(halo, bitmap, image=sample)
            # Preserve the bitmap's soft center and compress its outer tail.
            # The large engines are cobalt blue and the smaller jets cyan.
            large = name.endswith('large exhaust')
            tint = (.18, .48, 1.) if large else (.10, .82, 1.)
            for i in range(0, len(pixels), 4):
                intensity = (pixels[i] / 255) ** 1.65
                core = min(1., intensity ** 4 * 1.5)
                pixels[i:i+3] = bytes(round(255 * min(1., intensity * (c + (1-c)*core))) for c in tint)
            write_png(output / Path(texture).name, width, height, pixels)
            textures[name] = texture
        x, y, z = pose[:3]
        # Retail has two coincident placements per nozzle.
        if any(math.dist((x * 4, -y * 4, z * 4), row[:3]) < 2 for _, row in rows):
            continue
        radius, = tags.read('f', reflection + 40)
        brightness, = tags.read('f', reflection + 52)
        if name.endswith('large exhaust'):
            brightness = min(1., brightness * 1.35)
        cosine_falloff, cosine_cutoff = tags.read('2f', flare.address + 8)
        # Nozzle axes follow the surrounding authored exhaust shell.
        rows.append((texture, (x * 4, -y * 4, z * 4, 0., -1., 0., radius * 4,
                               brightness, 1., 1., 1., cosine_falloff, cosine_cutoff)))
    return rows


def autumn_nozzles(halo, coronas):
    """Find each exit rim from the authored exhaust shell, in render coordinates."""
    shell = {(x*4, -y*4, z*4) for shader, _, _, triangle in halo.surfaces(7)
             if shader.name.endswith('poa engine exhaust') for (x, y, z), _, _ in triangle}
    for _, (x, y, z, _, _, _, glow_radius, *_) in coronas:
        limit = glow_radius * .48 if glow_radius > 200 else glow_radius * .17
        candidates = [p for p in shell if math.hypot(p[0]-x, p[2]-z) < limit and abs(p[1]-y) < glow_radius*.4]
        if not candidates:
            continue
        # The aft-most band is the exit rim. Reuse its radius and center,
        # filling the dark interior behind the light sprite.
        rear = min(p[1] for p in candidates)
        rim = [p for p in candidates if p[1] < rear + .4]
        cx = sum(p[0] for p in rim) / len(rim)
        cz = sum(p[2] for p in rim) / len(rim)
        radius = sum(math.hypot(p[0]-cx, p[2]-cz) for p in rim) / len(rim)
        yield cx, rear + .25, cz, radius


def write_world_mesh(halo, output, ac_xy, ac_z, bounds, *, map_name='halo_a30', bsp=0, include_sky=True, sky_index=None, extra_bsps=()):
    # `extra_bsps`: other BSPs drawn in the same mesh (import_halo_campaign.EXTRA_BSPS: c40 section 1's
    # pulse generator room).
    legacy = map_name == 'halo_a30'
    xmin, xmax, ymin, ymax = bounds
    flyby = map_name == 'halo_scene_a10_07'
    # The New 3DS renderer selects the authored 64-texel world mip.
    resolution = 64 if flyby else None
    draw_batches, lightmaps = {}, {}
    textures = output / 'textures' / map_name / 'bsp'
    for shader, lightmap_tag, lightmap_index, vertices in (s for b in (bsp, *extra_bsps) for s in halo.surfaces(b)):
        positions = [vertex[0] for vertex in vertices]
        xs, ys, zs = zip(*positions)
        if max(xs) < xmin or min(xs) > xmax or max(ys) < ymin or min(ys) > ymax or (legacy and max(zs) < 28):
            continue
        material, flags = world_material(halo, shader)
        if not material or (flyby and shader.name.endswith('poa engine exhaust')):
            continue
        if legacy and 'filler' in shader.name:
            flags = 6
        detail_scale = 0.0
        if legacy and material.name.endswith('\\cell01_ground'):
            detail_scale, = halo.tags.read('f', shader.address + 200)
        use_lightmap = flyby and shader.group == 'senv' and lightmap_index >= 0 and lightmap_tag
        key = material, flags, detail_scale, lightmap_index if use_lightmap else -1
        batch = draw_batches.setdefault(key, bytearray())
        lightmap = None
        if lightmap_index >= 0 and flags != 2:
            # Each BSP has its own lightmap bitmap: with extra BSPs the cache is keyed by both.
            cached = (getattr(lightmap_tag, 'name', ''), lightmap_index) if extra_bsps else lightmap_index
            if cached not in lightmaps:
                try:
                    w, h, (pixels,) = decode_bitmap(halo, lightmap_tag, image=lightmap_index)
                    lightmaps[cached] = filter_lightmap(w, h, pixels, resolution) if flyby else (w, h, pixels)
                except ValueError:
                    if legacy:
                        raise
                    lightmaps[cached] = None
            lightmap = lightmaps[cached]
        for (x, y, z), (u, v), (lu, lv) in vertices:
            if use_lightmap:
                color = tuple(round(255 * max(0, min(1, c))) for c in halo.tags.read('3f', shader.address + 0x10c))
            elif flags == 4:
                color = (190, 190, 190)
            else:
                color = sampled_light(lightmap, lu, lv) if lightmap else (192, 193, 201)
            alpha = 154 if flags == 2 else 255
            batch.extend(struct.pack('<5f4B', *ac_xy(x, y), ac_z(z), u, v, *color, alpha))
            if use_lightmap:
                batch.extend(struct.pack('<2f', lu, lv))
    sky_refs = halo.tags.block(halo.scenario + 48, 16)
    sky_model = None
    if include_sky and sky_refs:
        selected = sky_index if sky_index is not None else (-1 if halo.name == 'd20' else 0)
        sky = halo.reference(halo.tags, sky_refs[selected], 'sky ') if -len(sky_refs) <= selected < len(sky_refs) else None
        if sky:
            sky_model = halo.reference(halo.tags, sky.address, 'mode')
    if sky_model:
        # The Halo ring and planets are independent authored sky meshes. Their
        # relative directions are preserved while fitting AC's skybox depth range.
        for texture, points, uv, triangles, additive in model_meshes(halo, sky_model):
            batch = draw_batches.setdefault((texture, 5 if additive else 3, 0.0, -1), bytearray())
            for triangle in triangles:
                for index in triangle:
                    x, y, z = points[index]
                    u, v = uv[index]
                    batch.extend(struct.pack('<5f4B', x / 100, -y / 100, z / 100, u, v, 255, 255, 255, 255))
    batches = bytearray()
    triangle_count = 0
    for i, ((texture, flags, detail_scale, light_index), vertices) in enumerate(sorted(draw_batches.items(), key=lambda item: item[0][1])):
        try:
            w, h, (pixels,) = decode_bitmap(halo, texture, resolution=resolution)
        except (ValueError, IndexError):
            if legacy:
                raise
            w, h, pixels = 4, 4, bytearray(bytes((123, 126, 139, 255)) * 16)
        if flags == 6:
            # These distant tree atlases store their flat background as RGB
            # rather than DXT alpha. Recover that chroma key and keep edges.
            for p in range(0, len(pixels), 4):
                distance = max(abs(pixels[p + c] - component) for c, component in enumerate((65, 76, 41)))
                pixels[p+3] = 255 if distance > 13 else 0
        elif flags in (0, 2):
            # Terrain base-map alpha chooses Halo detail layers and does
            # not make the terrain surface transparent.
            pixels[3::4] = bytes([255]) * (w * h)
        filename = f'packages/textures/{map_name}/bsp/material_{i:02}.png'
        write_png(textures / f'material_{i:02}.png', w, h, pixels)
        count = len(vertices) // (32 if light_index >= 0 else 24)
        batches.extend(struct.pack('<96s2If', filename.encode(), flags, count, detail_scale))
        if flyby:
            batches.extend(struct.pack('<i', light_index))
        batches.extend(vertices)
        triangle_count += count // 3
    if flyby:
        for index, image in lightmaps.items():
            if image is None:
                raise ValueError(f'Missing flyby lightmap {index}')
            write_png(textures / f'lightmap_{index:02}.png', *image)
    if legacy:
        detail = bitmap(halo, r'levels\a30\bitmaps\detail grass')
        w, h, (pixels,) = decode_bitmap(halo, detail)
        # Modulate by a centered, low-contrast luma version of the authored map.
        for p in range(0, len(pixels), 4):
            intensity = 104 + (pixels[p] + 2 * pixels[p+1] + pixels[p+2]) // 11
            pixels[p:p+4] = bytes((intensity, intensity, intensity, 255))
        write_png(textures / 'detail_grass.png', w, h, pixels)
    maps = output / 'maps'
    maps.mkdir(parents=True, exist_ok=True)
    coronas = autumn_coronas(halo, textures) if flyby else []
    if flyby:
        batches.extend(struct.pack('<I', len(coronas)))
        for texture, values in coronas:
            batches.extend(struct.pack('<96s13f', texture.encode(), *values))
        nozzles = list(autumn_nozzles(halo, coronas))
        if len(nozzles) != len(coronas):
            raise ValueError('Missing exhaust rim in the flyby BSP')
        batches.extend(struct.pack('<I', len(nozzles)))
        for nozzle in nozzles:
            batches.extend(struct.pack('<4f', *nozzle))
    magic = b'HWM4' if flyby else b'HWM2'
    (maps / f'{map_name}.mesh').write_bytes(struct.pack('<4sI', magic, len(draw_batches)) + batches)
    print(f'{map_name}: {triangle_count:,} BSP triangles and {len(draw_batches)} texture batches', flush=True)
    return triangle_count
