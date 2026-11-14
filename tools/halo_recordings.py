"""Decode the sparse 30 Hz input streams stored in retail scenario recordings."""
import math
import struct


def decode(data, unit_version, duration):
    cursor = 0

    def read(fmt):
        nonlocal cursor
        values = struct.unpack_from('<' + fmt, data, cursor)
        cursor += struct.calcsize('<' + fmt)
        return values

    state, _, flags, _, _, forward, left, *vectors = read('BB3H11f')
    for extension in ('I', 'H', 'H')[:max(0, unit_version - 1)]:
        read(extension)
    angles = [list(read('2h')) for _ in range(3)]
    directions = [(math.atan2(y, x), math.atan2(z, math.hypot(x, y)))
                  for x, y, z in zip(vectors[::3], vectors[1::3], vectors[2::3])]
    tick, samples = 0, {}

    def sample():
        samples[tick] = (tick, state, flags, forward, left, *(a for v in directions for a in v))

    sample()
    while cursor < len(data):
        header, = read('B')
        event, timing = header >> 2, header & 3
        tick += timing if timing < 2 else read('B' if timing == 2 else 'H')[0]
        if event == 1:
            if not duration - 1 <= tick <= duration or any(data[cursor:]):
                raise ValueError(f'Unexpected recording end at {tick} of {duration}')
            return list(samples.values())
        if tick >= duration:
            raise ValueError(f'Recording event at {tick} of {duration}')
        if event == 2:
            state, = read('B')
        elif event == 3:
            read('B')
        elif event == 4:
            flags, = read('H')
        elif event == 5:
            read('H')
        elif event == 6:
            forward, left = read('2f')
        elif 7 <= event <= 22:
            mask = event - (7 if event < 15 else 15)
            dyaw, dpitch = read('2b' if event < 15 else '2h')
            changed = [i for i in range(3) if mask & (1 << i)]
            if changed:
                yaw, pitch = angles[changed[0]]
                yaw += dyaw
                yaw -= 1000 if yaw > 1000 else -1000 if yaw < -1000 else 0
                pitch += dpitch
                for i in changed:
                    angles[i] = [yaw, pitch]
                    directions[i] = yaw * math.pi / 1000, pitch * math.pi / 1000
        elif event:
            raise ValueError(f'Unknown recording event {event}')
        sample()
    raise ValueError('Recording has no end event')
