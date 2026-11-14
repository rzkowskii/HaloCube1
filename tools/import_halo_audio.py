#!/usr/bin/env python3
"""Decode selected Halo CE Xbox A30 combat sounds to PCM WAV for AssaultCube."""

import argparse
import mmap
from pathlib import Path
import struct
import wave

from import_halo_a30 import HaloMap


WEAPONS = r'sound\sfx\weapons'
UI = r'sound\sfx\ui'
SOUNDS = {
    'ma5b-fire': WEAPONS + r'\assault rifle\fire',
    'ma5b-reload': WEAPONS + r'\weapon_anims\ar_reload',
    'pistol-fire': WEAPONS + r'\pistol\fire',
    'pistol-reload': WEAPONS + r'\weapon_anims\pistol_reload',
    'plasma-fire': WEAPONS + r'\plasma rifle\fire',
    'frag-explode': WEAPONS + r'\frag grenade\expl',
    'frag-throw': WEAPONS + r'\frag grenade\throwgren',
    'shield-hit': UI + r'\shield_hit',
    'shield-depleted': UI + r'\shield_depleted',
    'shield-charge': UI + r'\shield_charge',
    'melee': WEAPONS + r'\weapon_anims\ar_melee',
}

# Xbox IMA uses 36 bytes per channel per block. Sample zero is the block
# predictor, followed by 63 four-bit updates and one padding nibble.
STEPS = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
)
INDEX_DELTA = (-1, -1, -1, -1, 2, 4, 6, 8)


def xbox_ima(source, channels):
    stride = 36 * channels
    if channels not in (1, 2) or not source or len(source) % stride:
        raise ValueError('Invalid Xbox IMA sample extent')
    pcm = bytearray(len(source)//stride * 128*channels)
    for start in range(0, len(source), stride):
        frame_offset = start//stride * 128*channels
        for channel in range(channels):
            sample, index, reserved = struct.unpack_from('<hBB', source, start + 4*channel)
            if reserved or index >= len(STEPS):
                raise ValueError('Invalid Xbox IMA block header')
            output = frame_offset + channel*2
            struct.pack_into('<h', pcm, output, sample)
            for i in range(63):
                byte = source[start + 4*channels + (i//8*channels + channel)*4 + (i%8)//2]
                code = (byte >> (4*(i%2))) & 15
                magnitude = code & 7
                delta = ((2*magnitude + 1) * STEPS[index]) >> 3
                sample = max(-32768, min(32767, sample + (-delta if code & 8 else delta)))
                index = max(0, min(88, index + INDEX_DELTA[magnitude]))
                output += channels*2
                struct.pack_into('<h', pcm, output, sample)
    return pcm


def decode_sound(halo, tag):
    tags = halo.tags
    sample_rate_id, = tags.read('H', tag.address + 6)
    channel_id, codec = tags.read('2H', tag.address + 108)
    if sample_rate_id not in (0, 1) or channel_id not in (0, 1) or codec != 1:
        raise ValueError(f'Unsupported Xbox sound format for {tag.name}')
    pitch_ranges = tags.block(tag.address + 152, 72)
    if not pitch_ranges:
        raise ValueError(f'Missing pitch range for {tag.name}')
    permutations = tags.block(pitch_ranges[0] + 60, 124)
    if not permutations:
        raise ValueError(f'Missing sound permutation for {tag.name}')

    pcm = bytearray()
    seen = set()
    index = 0
    while index != -1:
        if not 0 <= index < len(permutations) or index in seen:
            raise ValueError(f'Invalid sound permutation chain for {tag.name}')
        seen.add(index)
        entry = permutations[index]
        format_id, next_index = tags.read('Hh', entry + 40)
        length, _, offset = tags.read('3I', entry + 64)
        if format_id != codec or not 0 < offset <= len(halo.data)-length or not 0 < length <= 4*1024*1024:
            raise ValueError(f'Invalid audio sample descriptor for {tag.name}')
        pcm += xbox_ima(halo.data[offset:offset+length], channel_id+1)
        index = next_index
    return pcm, channel_id+1, (22050, 44100)[sample_rate_id]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('map', type=Path, help='expanded Xbox Halo CE a30.map')
    parser.add_argument('destination', type=Path, help='root of the AssaultCube runtime or mod')
    args = parser.parse_args()
    destination = args.destination / 'packages/audio/halo'
    try:
        with args.map.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            sound_tags = {tag.name: tag for tag in halo.directory if tag.group == 'snd!'}
            decoded = {name: decode_sound(halo, sound_tags[tag_name]) for name, tag_name in SOUNDS.items()}
        destination.mkdir(parents=True, exist_ok=True)
        for name, (pcm, channels, rate) in decoded.items():
            with wave.open(str(destination / (name + '.wav')), 'wb') as output:
                output.setnchannels(channels)
                output.setsampwidth(2)
                output.setframerate(rate)
                output.writeframes(pcm)
            print(f'{name}: {len(pcm)/2/channels/rate:.2f}s, {rate} Hz, {channels} channel(s)')
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'Import failed: {error}\n')


if __name__ == '__main__':
    main()
