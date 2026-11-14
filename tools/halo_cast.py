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
