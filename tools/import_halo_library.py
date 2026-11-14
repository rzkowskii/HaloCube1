#!/usr/bin/env python3
"""Stage expanded Xbox caches and export native assets beyond the campaign route."""

import argparse
from concurrent.futures import ProcessPoolExecutor
from contextlib import contextmanager
import hashlib
import json
import mmap
from pathlib import Path
import shutil
import struct
import tempfile

from halo_collision import write_structure_collision
from halo_map_art import decode_bitmap, write_png
from import_halo_a30 import HaloMap
from import_halo_campaign import MISSIONS, Projection
from import_halo_models import AnimationGraph, Model, export
from import_halo_scripts import OBJECT_BLOCKS, export_worlds, impulse, model_pack_scale


CAMPAIGN = tuple(mission.cache for mission in MISSIONS)


@contextmanager
def opened_cache(path):
    with path.open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        if halo.name != path.stem:
            raise ValueError(f'{path} contains the {halo.name} cache')
        yield halo


def sha256_file(path):
    with path.open('rb') as source:
        digest = hashlib.sha256()
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
        return digest.hexdigest()


def stage_source(path, directory):
    """Keep all cache data, including tags the engine cannot yet interpret."""
    destination = directory / path.name
    checksum = sha256_file(path)
    if destination.resolve() != path.resolve():
        if destination.is_file() and sha256_file(destination) == checksum:
            return destination, checksum
        with tempfile.TemporaryDirectory(dir=directory) as staging:
            temp = Path(staging) / path.name
            shutil.copy2(path, temp)
            temp.replace(destination)
    return destination, checksum


def placements(halo):
    tags, scenario = halo.tags, halo.scenario
    names = tags.block(scenario + 516, 36)
    for kind, (offset, stride, palette) in OBJECT_BLOCKS.items():
        definitions = tags.block(scenario + palette, 48)
        for index, item in enumerate(tags.block(scenario + offset, stride)):
            variant, name, flags, permutation = tags.read('4h', item)
            definition = halo.reference(tags, definitions[variant]) if 0 <= variant < len(definitions) else None
            model = halo.reference(tags, definition.address + 40) if definition else None
            yield {'kind': kind, 'index': index, 'name_index': name, 'flags': flags & 0xffff,
                   'name': tags.name(names[name], 32) if 0 <= name < len(names) else None,
                   'permutation': permutation, 'tag': definition.name if definition else None,
                   'model': model.name if model and model.group == 'mode' else None, 'pose': tags.read('6f', item + 8),
                   'bsp_flags': tags.read('H', item + 32)[0]}


def player_starts(halo):
    for address in halo.tags.block(halo.scenario + 852, 52):
        x, y, z, facing, team, bsp, *modes = halo.tags.read('4f6h', address)
        yield {'position': [x, y, z], 'facing': facing, 'team': team, 'bsp': bsp, 'game_modes': modes}


def animation_graphs(halo):
    for tag in halo.directory:
        if tag.group == 'antr':
            tracks = []
            for address in halo.tags.block(tag.address + 116, 180):
                kind, frames = halo.tags.read('2H', address + 32)
                tracks.append({'name': halo.tags.name(address, 32), 'address': address,
                               'kind': kind, 'frames': frames})
            yield {'name': tag.name, 'tracks': tracks}


def loop_definitions(halo):
    tags = halo.tags

    def reference(address):
        tag = halo.reference(tags, address, 'snd!')
        return tag.name if tag else None

    for tag in halo.directory:
        if tag.group != 'lsnd':
            continue
        tracks = []
        for track in tags.block(tag.address + 60, 160):
            flags, gain, fade_in, fade_out = tags.read('I3f', track)
            segments = ('start', 'loop', 'stop', 'alternate_loop', 'alternate_stop')
            tracks.append({'flags': flags, 'gain': gain, 'fade_in': fade_in, 'fade_out': fade_out,
                           **{key: reference(track + offset) for key, offset in
                              zip(segments, (48, 64, 80, 128, 144))}})
        details = []
        for detail in tags.block(tag.address + 72, 104):
            minimum, maximum, gain, flags = tags.read('3fI', detail + 16)
            details.append({'sound': reference(detail), 'period': [minimum, maximum], 'gain': gain, 'flags': flags,
                            'bounds': tags.read('6f', detail + 80)})
        yield {'name': tag.name, 'flags': tags.read('I', tag.address)[0], 'tracks': tracks, 'details': details}


def import_map(job):
    path, library, output, rebuild = job
    staged, checksum = stage_source(path, library / 'sources')
    with opened_cache(staged) as halo:
        # Scene geometry and collision share the unshifted (4X, -4Y, 4Z)
        # transform, separate from the cropped playable route's projections.
        worlds = export_worlds(halo, output, rebuild)
        projection = Projection(9/4, -9/4, 0, 4)
        bsp_count = len(halo.tags.block(halo.scenario + 1444, 32))
        bsps, skipped = [], []
        for index in range(bsp_count):
            base = f'halo_scene_{halo.name}_{index:02}'
            collision = f'maps/{base}.hcol'
            target = output / 'packages' / collision
            region, address = halo.bsp(index)
            structures = region.block(address + 176, 96)
            if not structures or not region.block(structures[0] + 60, 12):
                collision = None
            elif rebuild or not target.is_file():
                try:
                    write_structure_collision(halo, index, projection, target)
                except ValueError as error:
                    skipped.append((index, str(error)))
                    collision = None
            bsps.append({'index': index, 'mesh': f'maps/{base}.mesh' if index in worlds else None,
                         'collision': collision})
        kind, = halo.tags.read('h', halo.scenario + 60)
        row = {'name': halo.name, 'kind': kind, 'source': f'sources/{path.name}', 'sha256': checksum,
               'tag_data_offset': halo.tags.offset, 'tag_address_base': halo.tags.base,
               'tags': [[tag.group, tag.name, tag.address] for tag in halo.directory],
               'bsps': bsps, 'collision_issues': skipped, 'placements': list(placements(halo)),
               'player_starts': list(player_starts(halo)),
               'animation_graphs': list(animation_graphs(halo)), 'sound_loops': list(loop_definitions(halo))}
        print(f'{halo.name}: {len(halo.directory)} source tags, {len(worlds)} render BSPs, '
              f'{sum(bsp["collision"] is not None for bsp in bsps)} collision BSPs', flush=True)
        return row


def name_for(group, name):
    # Use the full tag path for identity, including names shared by different groups.
    leaf = name.rsplit('\\', 1)[-1]
    slug = ''.join(c if c.isascii() and c.isalnum() else '-' for c in leaf).strip('-')[:36]
    digest = hashlib.sha256(f'{group}:{name}'.encode()).hexdigest()[:12]
    return f'{slug}-{digest}'


def write_model(halo, output, group, name, models, graph=None, clips=(), *, rebuild):
    slug = name_for(group, name)
    destination = output / 'packages/models/halo/library'
    if rebuild or not all((destination / slug / filename).is_file() for filename in ('tris.md3', 'md3.cfg')):
        geometry_scale = max(model_pack_scale(model, animated=bool(clips)) for model in models)
        markers = [('tag_muzzle', 'primary trigger')] if 'primary trigger' in models[0].markers else []
        export(halo, destination, slug, models, graph, clips, 4, markers, quiet=True,
               geometry_scale=geometry_scale)
    return f'models/halo/library/{slug}'


def import_models(paths, output, rebuild):
    static, weapons, issues = {}, {}, {}
    recoverable = (IndexError, KeyError, ValueError, struct.error, ZeroDivisionError, StopIteration)
    for path in paths:
        with opened_cache(path) as halo:
            arms = None
            for tag in halo.directory:
                key = f'{tag.group}:{tag.name}'
                try:
                    if tag.group == 'mode' and tag.name not in static:
                        model = Model(halo, tag.name, 'part')
                        target = write_model(halo, output, tag.group, tag.name, [model], rebuild=rebuild)
                        static[tag.name] = {'cache': halo.name, 'path': target}
                        issues.pop(key, None)
                    elif tag.group == 'weap' and tag.name not in weapons:
                        # The Xbox weapon interface points to the viewmodel and
                        # animation graph independently of the third-person model.
                        view = halo.reference(halo.tags, tag.address + 1116, 'mode')
                        graph_tag = halo.reference(halo.tags, tag.address + 1132, 'antr')
                        if not (view and graph_tag):
                            continue
                        if arms is None:
                            arms = Model(halo, r'characters\cyborg\fp\fp', 'arms')
                        model = Model(halo, view.name, 'weapon')
                        graph = AnimationGraph(halo, graph_tag.name)
                        choices = {
                            'gun idle': ('idle',), 'gun shoot': ('fire-1', 'firing'),
                            'gun reload': ('reload-full', 'reload-empty'),
                            'gun melee': ('melee',), 'gun throw': ('throw-grenade',),
                        }
                        clips = []
                        for slot, names in choices.items():
                            clip = next((f'first-person {name}' for name in names
                                         if f'first-person {name}' in graph.animations), None)
                            if clip:
                                clips.append((slot, clip))
                        target = write_model(halo, output, 'view', tag.name, [model, arms], graph, clips,
                                             rebuild=rebuild)
                        weapons[tag.name] = {'cache': halo.name, 'path': target, 'clips': dict(clips)}
                        issues.pop(key, None)
                except recoverable as error:
                    # Try other caches that carry the same tag before reporting
                    # a conversion gap. The original remains in each staged cache.
                    issues.setdefault(key, str(error))
    return static, weapons, issues


def import_sounds(path, names, library, rebuild):
    destination = library / 'sounds'
    sounds, issues = {}, {}
    with opened_cache(path) as halo:
        for index, tag in enumerate(halo.directory):
            if tag.group != 'snd!' or tag.name not in names:
                continue
            variants = []
            try:
                pitches = halo.tags.block(tag.address + 152, 72)
                if not pitches:
                    raise ValueError('No authored pitch ranges')
                slug = name_for(tag.group, tag.name)
                for pitch, address in enumerate(pitches):
                    count, = halo.tags.read('H', address + 44)
                    if not count or count > len(halo.tags.block(address + 60, 124)):
                        raise ValueError('Invalid playable permutation count')
                    for permutation in range(count):
                        ticks, subtitle, wav = impulse(halo, index, pitch, permutation)
                        # Keep the original Xbox ADPCM blocks in a WAV
                        # container. The asset library is separate from AC's
                        # in-game PCM audio exports.
                        file = destination / slug / f'{pitch}-{permutation}.wav'
                        if rebuild or not file.is_file():
                            file.parent.mkdir(parents=True, exist_ok=True)
                            temp = file.with_suffix('.tmp')
                            temp.write_bytes(wav)
                            temp.replace(file)
                        variants.append({'path': file.relative_to(library).as_posix(),
                                         'ticks': ticks, 'subtitle': subtitle})
            except (IndexError, ValueError, struct.error) as error:
                issues[tag.name] = str(error)
            if variants:
                sounds[tag.name] = {'cache': halo.name, 'variants': variants}
    print(f'{path.stem}: {sum(len(sound["variants"]) for sound in sounds.values())} sound variants', flush=True)
    return sounds, issues


def import_bitmaps(path, names, library, rebuild):
    bitmaps, issues = {}, {}
    with opened_cache(path) as halo:
        for tag in halo.directory:
            if tag.group != 'bitm' or tag.name not in names:
                continue
            entries = []
            try:
                descriptors = halo.tags.block(tag.address + 96, 48)
                if not descriptors:
                    raise ValueError('No authored bitmap images')
                directory = library / 'bitmaps' / name_for(tag.group, tag.name)
                for i, descriptor in enumerate(descriptors):
                    width, height, depth, kind, fmt = halo.tags.read('5H', descriptor + 4)
                    files = [directory / f'{i}-{plane}.png' for plane in range(6 if kind == 2 else depth)]
                    if rebuild or not all(file.is_file() for file in files):
                        w, h, images = decode_bitmap(halo, tag, cube=kind == 2, volume=kind == 1, image=i, indexed=True)
                        if len(images) != len(files):
                            raise ValueError('Unexpected number of bitmap faces or slices')
                        for file, pixels in zip(files, images):
                            temp = file.with_suffix('.tmp')
                            write_png(temp, w, h, pixels)
                            temp.replace(file)
                    entries.append({'size': [width, height, depth], 'kind': kind, 'format': fmt,
                                    'indexed': fmt == 17,
                                    'paths': [file.relative_to(library).as_posix() for file in files]})
            except (IndexError, ValueError, struct.error) as error:
                issues[tag.name] = str(error)
            if entries:
                bitmaps[tag.name] = {'cache': halo.name, 'images': entries}
    print(f'{path.stem}: {len(bitmaps)} bitmap tags', flush=True)
    return bitmaps, issues


def import_media(maps, library, rebuild, pool, group, importer):
    # Campaign caches come first for shared tags; each cache's distinct
    # version still remains in the source index.
    seen = set()
    pending = []
    for row in maps:
        names = {name for kind, name, _ in row['tags'] if kind == group} - seen
        seen.update(names)
        if names:
            pending.append(pool.submit(importer, library / row['source'], names, library, rebuild))
    assets, issues = {}, {}
    for job in pending:
        imported, unavailable = job.result()
        assets.update(imported)
        issues.update(unavailable)
    return assets, issues


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path, help='directory of expanded retail Xbox .map files')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--rebuild', action='store_true', help='regenerate all converted assets')
    args = parser.parse_args()
    paths = sorted(args.maps.glob('*.map'), key=lambda path:
                   (CAMPAIGN.index(path.stem) if path.stem in CAMPAIGN else len(CAMPAIGN), path.stem))
    if not paths:
        parser.error('No expanded .map files found')
    library = args.output / 'build/retail-assets'
    (library / 'sources').mkdir(parents=True, exist_ok=True)
    (args.output / 'packages/maps').mkdir(parents=True, exist_ok=True)
    companion = {}
    for filename in ('setup-manifest.json', 'ntsc2276-hud-strings.bin', 'ntsc2276-loading.bin'):
        path = args.maps / filename
        if path.is_file():
            staged, checksum = stage_source(path, library / 'sources')
            companion[filename] = {'path': staged.relative_to(library).as_posix(), 'sha256': checksum}
    jobs = [(path, library, args.output, args.rebuild) for path in paths]
    with ProcessPoolExecutor(max_workers=min(4, len(jobs))) as pool:
        maps = list(pool.map(import_map, jobs))
        sounds, audio_issues = import_media(maps, library, args.rebuild, pool, 'snd!', import_sounds)
        bitmaps, bitmap_issues = import_media(maps, library, args.rebuild, pool, 'bitm', import_bitmaps)
    models, weapons, issues = import_models([library / row['source'] for row in maps], args.output, args.rebuild)
    index = {'version': 1, 'maps': maps, 'companion_files': companion,
             'models': models, 'viewmodels': weapons, 'model_issues': issues,
             'sounds': sounds, 'sound_issues': audio_issues, 'bitmaps': bitmaps, 'bitmap_issues': bitmap_issues}
    with tempfile.TemporaryDirectory(dir=library) as staging:
        catalog = Path(staging) / 'catalog.json'
        catalog.write_text(json.dumps(index, separators=(',', ':'), sort_keys=True) + '\n')
        catalog.replace(library / 'catalog.json')
    print(f'{len(maps)} expanded caches, {len(models)} native static models, {len(weapons)} weapon viewmodels, '
          f'{len(sounds)} sound tags, {len(bitmaps)} bitmap tags; '
          f'{len(issues)} model, {len(audio_issues)} sound and {len(bitmap_issues)} bitmap conversion gaps', flush=True)
    print(library / 'catalog.json')


if __name__ == '__main__':
    main()
