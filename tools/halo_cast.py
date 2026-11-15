"""Retail cinematic object markers, seats, and locomotion parameters."""
import math

from import_halo_models import Model, transform


def motion(halo, tag):
    if not tag:
        return -2, (0.,) * 6
    tags, address = halo.tags, tag.address
    if tag.group == 'vehi':
        kind, = tags.read('h', address + 756)
        forward, reverse, accel, decel = tags.read('4f', address + 760)
        turn, = tags.read('f', address + 788)
        slide, = tags.read('f', address + 816)
        return kind, (forward, abs(reverse), accel, decel, math.radians(turn) / 30, abs(slide))
    if tag.group == 'bipd':
        return -1, (2.25 / 30, 2 / 30, .32 / 30, .32 / 30, 0, 2 / 30)
    return -2, (0.,) * 6


def cast_metadata(halo, tag, model):
    kind, rates = motion(halo, tag)
    center = halo.tags.read('3f', tag.address + 8) if tag else (0., 0., 0.)
    markers, seats = [], []
    if model:
        for name, (node, point, rotation) in model.markers.items():
            if node >= len(model.nodes):
                continue
            marker = model.bind_pose[model.nodes[node][0]].compose(transform(point, rotation, 1))
            forward = marker.vector((1, 0, 0))
            yaw = math.atan2(forward[1], forward[0])
            pitch = math.atan2(forward[2], math.hypot(*forward[:2]))
            markers.append((name, *marker.origin, yaw, pitch))
    if tag and tag.group == 'vehi':
        tags = halo.tags
        seats = [(tags.name(seat + 4, 32), tags.name(seat + 36, 32))
                 for seat in tags.block(tag.address + 740, 284)]
    return kind, rates, center, markers, seats


def damage_target(halo, tag):
    """A scripted unit the player damages by shooting it although it has no AI: d40's manifold
    detectors (levels\\d40\\devices\\invisible_collision_object\\invisible damage detector), flat
    invisible bipeds whose health enc5_1_destroyed_poll reads (unit_get_health). Returns
    (body vitality, box low, box high), the box in the object's own frame from its collision
    model's vertices, or None for every other object (zero vitality in the .hscb): units with
    a model the player sees (a10's and a50's Keyes) stay unhurt, as before.

    Only invisible bipeds qualify (the tag path names them "invisible"); no other retail scripted
    biped does."""
    if not tag or tag.group != 'bipd' or 'invisible' not in tag.name.lower():
        return None
    tags = halo.tags
    collision = halo.reference(tags, tag.address + 112, 'coll')
    if not collision:
        return None
    vitality, = tags.read('f', collision.address + 8)
    points = [tags.read('3f', vertex) for node in tags.block(collision.address + 652, 64)
              for bsp in tags.block(node + 52, 96) for vertex in tags.block(bsp + 84, 16)]
    if not points or not vitality > 0:
        return None
    low = tuple(min(p[i] for p in points) for i in range(3))
    high = tuple(max(p[i] for p in points) for i in range(3))
    return vitality, low, high
