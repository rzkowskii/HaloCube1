"""Export the retail structure collision polygons in campaign coordinates."""

import math
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


def write_structure_collision(halo, bsp, projection, destination):
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

    # HCL1 stores little-endian surface records followed by their boundary vertices.
    data = bytearray(struct.pack('<4sI', b'HCL1', len(surfaces)))
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
        data.extend(struct.pack('<3fBBhI', sign*x/length, -sign*y/length, sign*z/length,
                                flags, breakable, material, len(points)))
        # Campaign projection reflects Y. Reverse the edge loop so its winding
        # still agrees with the outward collision normal.
        for point in reversed(points):
            data.extend(struct.pack('<3f', *point))

    with tempfile.TemporaryDirectory(dir=destination.parent) as directory:
        temporary = Path(directory) / destination.name
        temporary.write_bytes(data)
        temporary.replace(destination)
    return len(surfaces)
