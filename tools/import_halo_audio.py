#!/usr/bin/env python3
"""Decode Halo CE Xbox combat sounds to PCM WAV for AssaultCube.

The fixed SOUNDS come from the A30 cache. Every `sound <role> <tag>` line in
packages/halo/weapons.txt and packages/halo/equipment.txt (tools/halo_weapons.py:
weapon fire, reload, empty, melee, ready, overheat, pickup, zoom and firing-loop sounds,
grenade and equipment sounds, the interface's shield and health warnings),
packages/halo/vehicles.txt (tools/halo_vehicles.py: engine loops, crash and suspension
sounds) and packages/halo/effects.txt (tools/halo_effects.py: the script effects'
sounds) is decoded from the first campaign cache next to the given map that holds
the tag, to packages/audio/halo/tags/<sample>.wav; source/src/halo_weapons.cpp names
the samples the same way (halosoundsample). tags/classes.txt lists each decoded
sample's retail sound class (`<sample> <class>`), which sound_class_set_gain fades
(source/src/halo_fx.cpp). Every permutation of each sound packages/halo/dialogue.txt names
(tools/halo_dialogue.py: the units' dialogue) is decoded on its own to
packages/audio/halo/dialogue/<sample>_<n>.wav, its class in the same classes.txt.
"""

import argparse
import mmap
from pathlib import Path
import struct
import sys
import wave

from import_halo_a30 import HaloMap
from halo_weapons import CAMPAIGN


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


def decode_sound(halo, tag, permutation=0):
    """PCM of one permutation of a sound tag (its first pitch range): the chain from the
    permutation through each next_permutation_index (the dialogue's permutations, decoded one by
    one for tools/halo_dialogue.py's table; every other sound plays its first)."""
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
    index = permutation
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


def sample_name(tag_name):
    """The engine's sample for a sound tag: 'sound\\sfx\\weapons\\needler\\fire' becomes
    'tags/sfx-weapons-needler-fire' (halosoundsample in source/src/halo_weapons.cpp)."""
    name = tag_name.lower()
    if name.startswith('sound\\'):
        name = name[len('sound\\'):]
    return 'tags/' + ''.join('-' if c == '\\' else '_' if c == ' ' else c for c in name)


def table_sounds(root):
    """Sound tags named by `sound <role> <tag>` lines in the weapon, equipment, vehicle and
    effect tables."""
    names = set()
    for table in ('weapons.txt', 'equipment.txt', 'vehicles.txt', 'effects.txt'):
        path = root / 'packages/halo' / table
        if not path.exists():
            print(f'warning: {path} is missing; run the tool that writes it first (docs/macos-build.md)', file=sys.stderr)
            continue
        for line in path.read_text().splitlines():
            key, _, rest = line.partition(' ')
            if key == 'sound':
                _, _, name = rest.partition(' ')
                if name:
                    names.add(name)
    return names


def dialogue_sounds(root):
    """{snd! tag: permutation count} from packages/halo/dialogue.txt (tools/halo_dialogue.py), or {}."""
    path = root / 'packages/halo/dialogue.txt'
    found = {}
    if not path.exists():
        return found
    for line in path.read_text().splitlines():
        key, _, rest = line.partition(' ')
        if key == 'sound':
            fields = rest.split(' ', 3)
            if len(fields) == 4 and fields[1].isdigit():
                found[fields[3]] = int(fields[1])
    return found


def dialogue_sample(tag_name, permutation):
    """The engine's sample for one dialogue permutation: 'sound\\dialog\\grunt\\pain' permutation 2
    becomes 'dialogue/dialog-grunt-pain_2' (dialoguesample in source/src/halo_dialogue.cpp)."""
    return 'dialogue/' + sample_name(tag_name)[len('tags/'):] + f'_{permutation}'


def write_wav(path, pcm, channels, rate):
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(channels)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(pcm)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('map', type=Path,
                        help='expanded Xbox Halo CE a30.map (the other campaign caches are read from its directory), '
                             'or that directory')
    parser.add_argument('destination', type=Path, help='root of the AssaultCube runtime or mod')
    args = parser.parse_args()
    maps = args.map if args.map.is_dir() else args.map.parent
    first = args.map if not args.map.is_dir() else maps / 'a30.map'
    destination = args.destination / 'packages/audio/halo'
    try:
        with first.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            sound_tags = {tag.name: tag for tag in halo.directory if tag.group == 'snd!'}
            decoded = {name: decode_sound(halo, sound_tags[tag_name]) for name, tag_name in SOUNDS.items()}
        destination.mkdir(parents=True, exist_ok=True)
        for name, (pcm, channels, rate) in decoded.items():
            write_wav(destination / (name + '.wav'), pcm, channels, rate)
            print(f'{name}: {len(pcm)/2/channels/rate:.2f}s, {rate} Hz, {channels} channel(s)')
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f'Import failed: {error}\n')

    # Weapon, grenade, equipment and interface sounds from the tables, from whichever cache has them.
    wanted, done, classes = table_sounds(args.destination), set(), {}
    for cache in CAMPAIGN:
        path = maps / f'{cache}.map'
        if not wanted - done or not path.exists():
            continue
        with path.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group != 'snd!' or tag.name not in wanted or tag.name in done:
                    continue
                try:
                    pcm, channels, rate = decode_sound(halo, tag)
                except (ValueError, struct.error) as error:
                    print(f'warning: {cache}: {error}', file=sys.stderr)
                    continue
                write_wav(destination / (sample_name(tag.name) + '.wav'), pcm, channels, rate)
                classes[sample_name(tag.name)] = halo.tags.read('h', tag.address + 4)[0]
                done.add(tag.name)
    (destination / 'tags').mkdir(parents=True, exist_ok=True)
    (destination / 'tags/classes.txt').write_text(
        '# <sample> <retail sound class> (tools/import_halo_audio.py)\n' +
        ''.join(f'{name} {value}\n' for name, value in sorted(classes.items())))
    missing = sorted(wanted - done)
    for name in missing:
        print(f'warning: no decodable {name} in the campaign caches', file=sys.stderr)
    print(f'{len(done)} weapon, equipment and interface sounds -> {destination / "tags"}'
          + (f' ({len(missing)} missing)' if missing else ''))

    # The unit dialogue (packages/halo/dialogue.txt): every permutation of each sound, one file each,
    # whose class goes to the same classes.txt (the engine plays them through haloplaysound).
    dialogue, spoken = dialogue_sounds(args.destination), {}
    for cache in CAMPAIGN:
        path = maps / f'{cache}.map'
        if not set(dialogue) - set(spoken) or not path.exists():
            continue
        with path.open('rb') as source, mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            for tag in halo.directory:
                if tag.group != 'snd!' or tag.name not in dialogue or tag.name in spoken:
                    continue
                written = 0
                for permutation in range(dialogue[tag.name]):
                    try:
                        pcm, channels, rate = decode_sound(halo, tag, permutation)
                    except (ValueError, struct.error) as error:
                        print(f'warning: {cache}: {error} (permutation {permutation})', file=sys.stderr)
                        continue
                    sample = dialogue_sample(tag.name, permutation)
                    write_wav(destination / (sample + '.wav'), pcm, channels, rate)
                    classes[sample] = halo.tags.read('h', tag.address + 4)[0]
                    written += 1
                spoken[tag.name] = written
    if dialogue:
        (destination / 'tags/classes.txt').write_text(
            '# <sample> <retail sound class> (tools/import_halo_audio.py)\n' +
            ''.join(f'{name} {value}\n' for name, value in sorted(classes.items())))
        short = sorted(name for name in dialogue if spoken.get(name, 0) < dialogue[name])
        for name in short:
            print(f'warning: {name}: {spoken.get(name, 0)} of {dialogue[name]} dialogue permutations decoded', file=sys.stderr)
        print(f'{sum(spoken.values())} dialogue permutations of {len(spoken)} sounds -> {destination / "dialogue"}'
              + (f' ({len(short)} incomplete)' if short else ''))


if __name__ == '__main__':
    main()
