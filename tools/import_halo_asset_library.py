#!/usr/bin/env python3
"""Index and extract original Xbox Halo CE campaign assets."""

import argparse
from hashlib import sha256
import json
import mmap
from pathlib import Path
import tempfile

from import_halo_a30 import HaloMap
from import_halo_campaign import MISSIONS
from import_halo_scripts import bytes_at, string_data


SHADER_MAPS = {
    'senv': {'base': 136, 'primary_detail': 184, 'secondary_detail': 204, 'micro_detail': 252,
             'bump': 296, 'emission': 596, 'reflection': 804},
    'soso': {'base': 164, 'multipurpose': 188, 'detail': 220, 'reflection': 356},
    'sgla': {'tint': 100, 'reflection': 172, 'bump': 192, 'diffuse': 344, 'diffuse_detail': 364,
             'specular': 416, 'specular_detail': 436},
    'swat': {'base': 76, 'reflection': 156, 'ripple': 200},
    'spla': {'primary_noise': 212, 'secondary_noise': 284},
    'smet': {'base': 76},
}
TRANSPARENT_MAPS = {'sotr': ((84, 100, 28),), 'schi': ((84, 220, 108),),
                    'scex': ((84, 220, 108), (96, 220, 108))}


class Library:
    def __init__(self, directory):
        self.directory = directory
        self.blobs = directory / 'blobs'
        self.blobs.mkdir(parents=True, exist_ok=True)
        self.payloads = set()

    def store(self, data):
        digest = sha256(data).hexdigest()
        if digest not in self.payloads:
            path = self.blobs / digest
            if not path.is_file():
                self.write(path, data)
            self.payloads.add(digest)
        return digest

    @staticmethod
    def write(path, data):
        with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as output:
            temporary = Path(output.name)
            try:
                output.write(data)
                output.close()
                temporary.replace(path)
            finally:
                temporary.unlink(missing_ok=True)

    def cache_data(self, halo, offset, length):
        if length == 0:
            return None
        if not 2048 <= offset <= len(halo.data) - length:
            raise ValueError(f'{halo.name}: payload at {offset:#x} exceeds the map')
        return self.store(halo.data[offset:offset + length])

    def bitmap(self, halo, tag):
        region = halo.tags
        images = []
        for address in region.block(tag.address + 96, 48):
            width, height, depth, kind, format_id, flags = region.read('6H', address + 4)
            registration_x, registration_y, mips = region.read('hhH', address + 16)
            offset, length = region.read('2I', address + 24)
            images.append({
                'dimensions': [width, height, depth], 'kind': kind, 'format': format_id,
                'flags': flags, 'registration': [registration_x, registration_y], 'mip_count': mips,
                'bytes': length, 'payload': self.cache_data(halo, offset, length),
            })
        sequences = []
        for address in region.block(tag.address + 84, 64):
            first, count = region.read('hh', address + 32)
            sprites = []
            for sprite in region.block(address + 52, 32):
                index, = region.read('h', sprite)
                sprites.append({'image': index, 'bounds': region.read('4f', sprite + 8),
                                'registration': region.read('2f', sprite + 24)})
            sequences.append({'name': region.name(address, 32), 'first_image': first,
                              'image_count': count, 'sprites': sprites})
        kind, format_id, usage, flags = region.read('4H', tag.address)
        return {'kind': kind, 'format': format_id, 'usage': usage, 'flags': flags,
                'images': images, 'sequences': sequences}

    def sound(self, halo, tag):
        region = halo.tags
        sound_class, sample_rate = region.read('2H', tag.address + 4)
        encoding, compression = region.read('2H', tag.address + 108)
        ranges = []
        for address in region.block(tag.address + 152, 72):
            pitch, lower, upper = region.read('3f', address + 32)
            count, = region.read('H', address + 44)
            permutations = []
            for entry in region.block(address + 60, 124):
                skip, gain, codec, next_index = region.read('ffHh', entry + 32)
                length, _, offset = region.read('3I', entry + 64)
                metadata = {}
                for field, name in ((84, 'mouth_data'), (104, 'subtitle_data')):
                    size, _, _, pointer = region.read('4I', entry + field)
                    metadata[name] = self.store(bytes_at(region, pointer, size)) if size else None
                permutations.append({
                    'name': region.name(entry, 32), 'skip_fraction': skip, 'gain': gain,
                    'compression': codec, 'next': next_index, 'bytes': length,
                    'samples': self.cache_data(halo, offset, length), **metadata,
                })
            ranges.append({'name': region.name(address, 32), 'pitch': pitch, 'bend_bounds': [lower, upper],
                           'permutation_count': count, 'permutations': permutations})
        return {'class': sound_class, 'sample_rate_id': sample_rate, 'encoding': encoding,
                'compression': compression, 'pitch_ranges': ranges}

    def tag_data(self, region, descriptor):
        data = string_data(region, descriptor)
        return self.store(data) if data else None

    @staticmethod
    def dependency(halo, address):
        handle, = halo.tags.read('I', address + 12)
        return None if handle == 0xffffffff else handle & 0xffff

    def material(self, halo, tag):
        region, address = halo.tags, tag.address
        if tag.group in SHADER_MAPS:
            maps = {name: self.dependency(halo, address + offset)
                    for name, offset in SHADER_MAPS[tag.group].items()}
            return {'maps': {name: index for name, index in maps.items() if index is not None}}
        blend, fade, fade_source = region.read('3h', address + 44)
        layers = []
        for block, stride, bitmap_offset in TRANSPARENT_MAPS[tag.group]:
            layers.append([{'bitmap': self.dependency(halo, entry + bitmap_offset),
                            'data': self.store(bytes_at(region, entry, stride))}
                           for entry in region.block(address + block, stride)])
        result = {'blend': blend, 'fade': fade, 'fade_source': fade_source, 'map_sets': layers,
                  'lens_flare': self.dependency(halo, address + 56),
                  'extra_layers': [self.dependency(halo, entry) for entry in region.block(address + 72, 16)]}
        if tag.group == 'sotr':
            result['stages'] = [self.store(bytes_at(region, entry, 112))
                                for entry in region.block(address + 96, 112)]
        return result

    def model(self, halo, tag):
        region, base = halo.tags, tag.address
        geometries = []
        for geometry in region.block(base + 208, 48):
            parts = []
            for part in region.block(geometry + 36, 104):
                shader, = region.read('h', part + 4)
                primitive, _, triangles, indices = region.read('HHII', part + 68)
                encoding, _, vertex_count = region.read('HHI', part + 84)
                descriptor, = region.read('I', part + 100)
                index_count = (triangles + 2 if primitive else triangles * 3) if triangles else 0
                if primitive not in (0, 1) or (vertex_count and encoding != 5):
                    raise ValueError(f'{halo.name}: unsupported model geometry in {tag.name}')
                vertices = region.read('I', descriptor + 4)[0] if vertex_count else 0
                parts.append({
                    'shader': shader, 'primitive': primitive, 'triangle_count': triangles,
                    'vertex_encoding': encoding, 'vertex_count': vertex_count,
                    'vertices': self.store(bytes_at(region, vertices, vertex_count * 32)) if vertex_count else None,
                    'indices': self.store(bytes_at(region, indices, index_count * 2)) if index_count else None,
                })
            geometries.append(parts)
        regions = []
        for entry in region.block(base + 196, 76):
            variants = [{'name': region.name(p, 32), 'flags': region.read('I', p + 32)[0],
                         'lod_indices': region.read('5h', p + 64)}
                        for p in region.block(entry + 64, 88)]
            regions.append({'name': region.name(entry, 32), 'permutations': variants})
        shaders = [region.read('I', s + 12)[0] for s in region.block(base + 220, 32)]
        nodes = [{'name': region.name(n, 32), 'hierarchy': region.read('3h', n + 32),
                  'bind_position_rotation': region.read('7f', n + 40),
                  'inverse_bind': region.read('13f', n + 104)}
                 for n in region.block(base + 184, 156)]
        markers = [{'name': region.name(m, 32),
                    'instances': [{'binding': region.read('3B', i), 'transform': region.read('7f', i + 4)}
                                  for i in region.block(m + 52, 32)]}
                   for m in region.block(base + 172, 64)]
        return {'texture_scale': region.read('2f', base + 48), 'shaders': shaders,
                'nodes': nodes, 'markers': markers, 'regions': regions, 'geometries': geometries}

    def animation_graph(self, halo, tag):
        region = halo.tags
        nodes = [{'name': region.name(n, 32), 'hierarchy': region.read('3h', n + 32)}
                 for n in region.block(tag.address + 104, 64)]
        clips = []
        for clip in region.block(tag.address + 116, 180):
            kind, frames, stride, info_type = region.read('4H', clip + 32)
            node_count, loop = region.read('Hh', clip + 44)
            flags, = region.read('H', clip + 58)
            clips.append({
                'name': region.name(clip, 32), 'type': kind, 'frames': frames, 'stride': stride,
                'frame_info_type': info_type, 'node_count': node_count, 'loop_frame': loop, 'flags': flags,
                'frame_info': self.tag_data(region, clip + 72), 'default_data': self.tag_data(region, clip + 140),
                'frame_data': self.tag_data(region, clip + 160),
                'translation_mask': region.read('Q', clip + 92)[0],
                'rotation_mask': region.read('Q', clip + 108)[0],
                'scale_mask': region.read('Q', clip + 124)[0],
                'compressed_offset': region.read('I', clip + 136)[0],
            })
        return {'nodes': nodes, 'clips': clips}

    def scenario(self, halo):
        region, scenario = halo.tags, halo.scenario
        recordings = [{'name': region.name(p, 32), 'encoding': region.read('3B', p + 32),
                       'ticks': region.read('H', p + 36)[0], 'data': self.tag_data(region, p + 44)}
                      for p in region.block(scenario + 876, 64)]
        structure_bsps = []
        for entry in region.block(scenario + 1444, 32):
            offset, length, address = region.read('3I', entry)
            structure_bsps.append({'address': address, 'bytes': length,
                                   'payload': self.cache_data(halo, offset, length)})
        return {'recordings': recordings, 'structure_bsps': structure_bsps,
                'script_syntax': self.tag_data(region, scenario + 1140),
                'script_strings': self.tag_data(region, scenario + 1160)}

    def lens_flare(self, halo, tag):
        region, address = halo.tags, tag.address
        falloff, cutoff = region.read('2f', address + 8)
        reflections = []
        for entry in region.block(address + 196, 128):
            sample, = region.read('h', entry + 4)
            reflections.append({'image': sample, 'radius': region.read('2f', entry + 40),
                                'brightness': region.read('2f', entry + 52),
                                'data': self.store(bytes_at(region, entry, 128))})
        return {'bitmap': self.dependency(halo, address + 32), 'cosine_falloff': falloff,
                'cosine_cutoff': cutoff, 'reflections': reflections}

    def mission(self, source, expected_name):
        with source.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as cache:
            halo = HaloMap(cache)
            if halo.name != expected_name:
                raise ValueError(f'{source}: expected {expected_name}, found {halo.name}')
            region = halo.tags
            tags = [{'id': i, 'group': tag.group, 'name': tag.name, 'address': tag.address}
                    for i, tag in enumerate(halo.directory)]
            bitmaps = {i: self.bitmap(halo, tag) for i, tag in enumerate(halo.directory) if tag.group == 'bitm'}
            sounds = {i: self.sound(halo, tag) for i, tag in enumerate(halo.directory) if tag.group == 'snd!'}
            models = {i: self.model(halo, tag) for i, tag in enumerate(halo.directory) if tag.group == 'mode'}
            animations = {i: self.animation_graph(halo, tag) for i, tag in enumerate(halo.directory) if tag.group == 'antr'}
            materials = {i: self.material(halo, tag) for i, tag in enumerate(halo.directory)
                         if tag.group in SHADER_MAPS or tag.group in TRANSPARENT_MAPS}
            flares = {i: self.lens_flare(halo, tag) for i, tag in enumerate(halo.directory) if tag.group == 'lens'}
            index = {'version': 1, 'map': source.name, 'map_sha256': sha256(cache).hexdigest(),
                     'tag_region': {'address': region.base, 'bytes': region.size,
                                    'payload': self.cache_data(halo, region.offset, region.size)},
                     'tags': tags, 'bitmaps': bitmaps, 'sounds': sounds, 'models': models,
                     'animations': animations, 'materials': materials, 'lens_flares': flares,
                     'scenario': self.scenario(halo)}
            path = self.directory / f'{halo.name}.json'
            self.write(path, json.dumps(index, allow_nan=False, separators=(',', ':')).encode())
            print(f'{halo.name}: {len(tags):,} tags, {len(bitmaps):,} bitmaps, {len(sounds):,} sounds, '
                  f'{len(models):,} models, {len(animations):,} animation graphs, {len(materials):,} materials', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent / 'build/halo-assets')
    parser.add_argument('--mission', choices=[m.cache for m in MISSIONS])
    args = parser.parse_args()
    library = Library(args.output)
    for mission in MISSIONS:
        if args.mission is None or args.mission == mission.cache:
            library.mission(args.maps / f'{mission.cache}.map', mission.cache)
    print(f'{len(library.payloads):,} distinct Xbox payloads in {library.directory}', flush=True)


if __name__ == '__main__':
    main()
