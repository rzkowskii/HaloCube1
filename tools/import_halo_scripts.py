#!/usr/bin/env python3
"""Import retail Xbox HaloScript programs and their cinematic dependencies."""
import argparse
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, ThreadPoolExecutor
import math
import mmap
from pathlib import Path
import struct
import subprocess
import unicodedata

from halo_map_art import write_world_mesh
from halo_actors import actor_index
from halo_cast import cast_metadata
from halo_devices import device_rows
from halo_recordings import decode
from import_halo_a30 import HaloMap, ORIGIN_X, ORIGIN_Y, SOURCE_FLOOR, HORIZONTAL_SCALE, VERTICAL_SCALE, MARGIN
from import_halo_campaign import ACTORS, MISSIONS, section_layout

NIL = 0xffffffff
OBJECT_BLOCKS = {
    0: (552, 120, 564), 1: (576, 120, 588), 2: (624, 92, 636),
    3: (600, 40, 612), 6: (528, 72, 540), 7: (660, 64, 672),
    8: (684, 64, 696), 9: (708, 88, 720),
}


def bytes_at(region, address, size):
    position = address - region.base
    if not 0 <= position <= region.size - size:
        raise ValueError(f'Invalid cache data: {address:#x}, {size} bytes')
    return region.data[region.offset + position:region.offset + position + size]


def ascii_display(text):
    for original, replacement in {'‘': "'", '’': "'", '“': '"', '”': '"', '…': '...', '—': '-'}.items():
        text = text.replace(original, replacement)
    return unicodedata.normalize('NFKD', text).encode('ascii', 'ignore').decode().strip()


def string_data(region, descriptor):
    size, _, _, pointer = region.read('4I', descriptor)
    return bytes_at(region, pointer, size) if size else b''


class Program:
    def __init__(self, halo):
        self.halo = halo
        t, s = halo.tags, halo.scenario
        syntax = string_data(t, s + 1140)
        strings = string_data(t, s + 1160)
        if len(syntax) < 56 or (len(syntax) - 56) % 20 or struct.unpack_from('<I', syntax, 40)[0] != 0x64407440:
            raise ValueError('Unexpected HaloScript datum-array header')
        self.strings = strings
        self.nodes = list(struct.iter_unpack('<4H3I', syntax[56:]))
        self.scripts = [(t.name(p, 32), *t.read('HHI', p + 32)) for p in t.block(s + 1180, 92)]
        self.globals = [(t.name(p, 32), *t.read('H', p + 32), *t.read('I', p + 40)) for p in t.block(s + 1192, 92)]
        for row in (*self.scripts, *self.globals):
            self.node(row[-1])
        self.sounds = {data & 0xffff for salt, op, ty, flags, _, _, data in self.nodes
                       if salt and flags & 5 == 1 and ty != 2 and op == 24 and data != NIL}
        self.loops = {data & 0xffff for salt, op, ty, flags, _, _, data in self.nodes
                      if salt and flags & 5 == 1 and ty != 2 and op == 27 and data != NIL}
        self.animations = self.animation_uses()

    def node(self, datum):
        salt, index = datum >> 16, datum & 0xffff
        if index >= len(self.nodes) or self.nodes[index][0] != salt or not salt:
            raise ValueError(f'Invalid HaloScript datum {datum:#x}')
        return self.nodes[index]

    def text(self, offset):
        if offset >= len(self.strings):
            return ''
        end = self.strings.find(b'\0', offset)
        return ascii_display(self.strings[offset:end if end >= 0 else len(self.strings)].decode('cp1252', 'replace'))

    def animation_uses(self):
        uses = Counter()
        for salt, opcode, _, flags, _, _, data in self.nodes:
            # custom_animation and unit_custom_animation_at_frame.
            if not salt or flags & 3 or opcode not in (101, 102):
                continue
            first = self.node(self.node(data)[4])
            graph = self.node(first[4])
            clip = self.node(graph[4])
            if first[3] & 5 == 1 and graph[1] == 28 and clip[1] == 9:
                uses[first[6] & 0xffff, graph[6] & 0xffff, self.text(clip[5])] += 1
        return uses


class Writer:
    def __init__(self, path):
        self.file = path.open('wb')

    def write(self, fmt, *values):
        self.file.write(struct.pack('<' + fmt, *values))

    def text(self, text):
        data = text.encode('ascii')
        self.write('H', len(data))
        self.file.write(data)


def impulse(halo, index, pitch=0, permutation=0):
    """Read one authored pitch/permutation chain and its Xbox audio blocks."""
    tag = halo.directory[index]
    if tag.group != 'snd!':
        raise ValueError(f'Expected sound at {index}, got {tag.group}')
    t = halo.tags
    rate_id, = t.read('H', tag.address + 6)
    channel_id, codec = t.read('2H', tag.address + 108)
    if rate_id not in (0, 1) or channel_id not in (0, 1) or codec not in (0, 1):
        raise ValueError(f'Unsupported Xbox sound format: {tag.name}')
    rate, channels = (22050, 44100)[rate_id], channel_id + 1
    permutations = t.block(t.block(tag.address + 152, 72)[pitch] + 60, 124)
    frames, samples, subtitles = 0, [], []
    seen = set()
    while permutation != -1:
        if not 0 <= permutation < len(permutations) or permutation in seen:
            raise ValueError(f'Invalid Xbox sound permutation chain: {tag.name}')
        seen.add(permutation)
        entry = permutations[permutation]
        fmt, permutation = t.read('Hh', entry + 40)
        length, _, offset = t.read('3I', entry + 64)
        stride = (36 if codec == 1 else 2) * channels
        if fmt != codec or length % stride or not 2048 <= offset <= len(halo.data) - length:
            raise ValueError(f'Invalid Xbox sound data: {tag.name}')
        samples.append(halo.data[offset:offset + length])
        frames += length // stride * (64 if codec == 1 else 1)
        subtitle = string_data(t, entry + 104)
        if subtitle:
            subtitles.append(ascii_display(subtitle.decode('utf-16le').rstrip('\0')))
    data = b''.join(samples)
    if codec == 1:
        fmt = struct.pack('<HHIIHHHH', 0x69, channels, rate, rate * stride // 64, stride, 4, 2, 64)
    else:
        fmt = struct.pack('<HHIIHH', 1, channels, rate, rate * stride, stride, 16)
    riff = struct.pack('<4sI4s4sI', b'RIFF', 20 + len(fmt) + len(data), b'WAVE', b'fmt ', len(fmt))
    return math.ceil(frames * 30 / rate), ' '.join(subtitles), riff + fmt + struct.pack('<4sI', b'data', len(data)) + data


def write_pcm_wave(destination, data):
    temp = destination.with_suffix('.tmp')
    try:
        subprocess.run(['ffmpeg', '-nostdin', '-loglevel', 'error', '-y', '-i', 'pipe:0',
                        '-c:a', 'pcm_s16le', '-f', 'wav', str(temp)], input=data, check=True)
        temp.replace(destination)
    finally:
        temp.unlink(missing_ok=True)


def export_sound(halo, out, index):
    duration, subtitle, data = impulse(halo, index)
    destination = out / f'{index}.wav'
    if not destination.is_file():
        write_pcm_wave(destination, data)
    longest_ms, = halo.tags.read('I', halo.directory[index].address + 132)
    ticks = longest_ms * 30 // 1000 if 0 < longest_ms < 600000 else duration
    return index, duration, ticks, subtitle


def table(output, rows, write):
    output.write('I', len(rows))
    for row in rows:
        write(*row)


def position_row(output, name, *coords):
    output.text(name)
    output.write(f'{len(coords)}f', *coords)


def named_objects(program):
    halo = program.halo
    t, s = halo.tags, halo.scenario
    objects = t.block(s + 516, 36)
    for p in objects:
        name = t.name(p, 32)
        kind, index = t.read('hh', p + 32)
        tag, pose = None, (0.0,) * 6
        if kind in OBJECT_BLOCKS and index >= 0:
            placements, stride, palette = OBJECT_BLOCKS[kind]
            points = t.block(s + placements, stride)
            palette = t.block(s + palette, 48)
            if index < len(points):
                placement = points[index]
                variant, = t.read('h', placement)
                if t.read('h', placement + 2)[0] == (p - objects.start) // objects.step:
                    pose = t.read('6f', placement + 8)
                    if 0 <= variant < len(palette):
                        tag = halo.reference(t, palette[variant])
        yield name, tag, pose


def title_rows(halo):
    t, s = halo.tags, halo.scenario
    help_tag = halo.reference(t, s + 1412, 'ustr')
    strings = t.block(help_tag.address, 20) if help_tag else ()
    for p in t.block(s + 1276, 96):
        index, = t.read('h', p + 48)
        if 0 <= index < len(strings):
            text = ascii_display(string_data(t, strings[index]).decode('utf-16le').rstrip('\0'))
        else:
            text = ''
        yield t.name(p + 4, 32), text, t.read('3f', p + 68)


def hud_rows(halo):
    t, s = halo.tags, halo.scenario
    tag = halo.reference(t, s + 1428, 'hmt ')
    if not tag:
        return
    text = string_data(t, tag.address).decode('utf-16le')
    elements = t.block(tag.address + 20, 2)
    for p in t.block(tag.address + 32, 64):
        start, index, count = t.read('hhB', p + 32)
        parts = []
        for i in range(index, index + count):
            if not 0 <= i < len(elements):
                break
            kind, value = t.read('2B', elements[i])
            if kind == 0:
                parts.append(text[start:start + value].rstrip('\0'))
                start += value
            else:
                parts.append(' ')
        yield t.name(p, 32), ascii_display(''.join(parts).replace('|n', ' '))


def conversation_rows(program):
    h = program.halo
    t, s = h.tags, h.scenario
    for p in t.block(s + 1128, 116):
        lines = []
        for line in t.block(p + 92, 124):
            delay, = t.read('f', line + 12)
            variants = [index & 0xffff for i in range(6)
                        if (index := t.read('I', line + 40 + 16*i)[0]) != NIL]
            program.sounds.update(variants)
            lines.append((max(0, round(delay * 30)), variants))
        yield t.name(p, 32), lines


def looping_rows(program):
    h, t = program.halo, program.halo.tags
    for index in sorted(program.loops):
        tag = h.tag(index, 'lsnd')
        flags, = t.read('I', tag.address)
        tracks = []
        for p in t.block(tag.address + 60, 160):
            gain, = t.read('f', p + 4)
            handles = [t.read('I', p + offset + 12)[0] for offset in (48, 64, 80, 128, 144)]
            indices = [value & 0xffff if value != NIL else -1 for value in handles]
            program.sounds.update(value for value in indices if value >= 0)
            tracks.append((gain, *indices))
        yield index, not bool(flags & 2), tracks


def squad_rows(halo, limit_to_normal=False, actors=None):
    """Encounters for the .hscb. Each also carries its AI data for the version 6 section:
    firing positions, platoons, and per squad and start the states, masks and indices."""
    t, s = halo.tags, halo.scenario
    palette = t.block(s + 1056, 16)
    actors = actors or {}
    for encounter in t.block(s + 1068, 176):
        name = t.name(encounter, 32)
        team, = t.read('h', encounter + 36)
        bsp, = t.read('h', encounter + 126)
        squads, squad_ai = [], []
        for squad in t.block(encounter + 128, 232):
            default, platoon = t.read('2h', squad + 32)
            normal_count, = t.read('h', squad + 124)
            starts, start_ai = [], []
            for p in t.block(squad + 208, 28):
                variant, = t.read('h', p + 24)
                variant = default if variant < 0 else variant
                actor = halo.reference(t, palette[variant]) if 0 <= variant < len(palette) else None
                family = actor.name.split('\\')[1] if actor else ''
                starts.append((ACTORS.get(family, -1), *t.read('4f', p)))
                initial, default_state = t.read('2h', p + 22)[0], t.read('h', p + 20)[0]
                command_list, = t.read('h', p + 26)
                start_ai.append((actors.get(actor.name, -1) if actor else -1, initial, default_state, command_list))
            if limit_to_normal:
                # The other positions are alternatives or reserved for a
                # higher difficulty. The campaign runs on Normal.
                keep = [i for i, start in enumerate(starts) if start[0] >= 0][:max(0, normal_count)]
                starts = [starts[i] for i in keep]
                start_ai = [start_ai[i] for i in keep]
            squads.append((t.name(squad, 32), platoon, starts))
            initial, default_state = t.read('2h', squad + 36)
            flags, = t.read('I', squad + 40)
            maneuver, = t.read('h', squad + 78)
            masks = t.read('7I', squad + 84)
            squad_ai.append((initial, default_state, flags, maneuver, *masks, start_ai))
        positions = [(*t.read('3f', p), *t.read('2h', p + 12)) for p in t.block(encounter + 152, 24)]
        platoons = [(t.read('I', p + 32)[0], *t.read('2h', p + 48), *t.read('2h', p + 60))
                    for p in t.block(encounter + 140, 172)]
        yield name, team, bsp, squads, (positions, platoons, squad_ai)


def command_lists(halo):
    """Scenario command lists: name, flags, atoms and points (retail coordinates)."""
    t, s = halo.tags, halo.scenario
    for entry in t.block(s + 1080, 96):
        atoms = [(*t.read('2h', a), *t.read('2f', a + 4), *t.read('7h', a + 12)) for a in t.block(entry + 48, 32)]
        points = [t.read('3f', p) for p in t.block(entry + 60, 20)]
        yield t.name(entry, 32), t.read('I', entry + 32)[0], atoms, points


def write_program(program, mission, output, objects, animations, sounds, conversations, loops, worlds, devices):
    h = program.halo
    t, s = h.tags, h.scenario
    path = output / f'{mission.cache}.hscb'
    out = Writer(path.with_suffix('.tmp'))
    try:
        out.write('4sI', b'HSCB', 6)
        table(out, program.nodes, lambda *v: out.write('4H3I', *v))
        out.write('I', len(program.strings))
        out.file.write(program.strings)
        table(out, program.scripts, lambda name, *v: (out.text(name), out.write('HHI', *v)))
        table(out, program.globals, lambda name, *v: (out.text(name), out.write('HI', *v)))
        table(out, [(t.name(p + 4, 32), *t.read('7f', p + 40)) for p in t.block(s + 1264, 104)],
              lambda *v: position_row(out, *v))
        table(out, [(t.name(p + 4, 32), *t.read('5f', p + 36)) for p in t.block(s + 1252, 92)],
              lambda *v: position_row(out, *v))
        table(out, [(t.name(p + 4, 32), *t.read('H', p), *t.read('12f', p + 48)) for p in t.block(s + 864, 96)],
              lambda name, *v: (out.text(name), out.write('H12f', *v)))
        table(out, list(title_rows(h)), lambda name, text, times: (out.text(name), out.text(text), out.write('3f', *times)))
        recordings = []
        for p in t.block(s + 876, 64):
            name = t.name(p, 32)
            version, raw, unit = t.read('3B', p + 32)
            ticks, = t.read('H', p + 36)
            if version != 4 or raw:
                raise ValueError(f'{name}: unsupported recording format {version}, raw={raw}')
            recordings.append((name, ticks, decode(string_data(t, p + 44), unit, ticks)))
        table(out, recordings, lambda name, ticks, samples:
              (out.text(name), out.write('H', ticks), table(out, samples, lambda *v: out.write('HBH8f', *v))))

        def write_object(name, model, pose, metadata, locomotion):
            kind, rates, center, markers, seats = metadata
            out.text(name)
            out.text(model)
            out.write('6fh9f', *pose, kind, *rates, *center)
            table(out, markers, lambda *v: position_row(out, *v))
            table(out, seats, lambda name, marker: (out.text(name), out.text(marker)))
            for path in locomotion:
                out.text(path)

        table(out, objects, write_object)
        table(out, animations, lambda obj, graph, clip, model, ticks:
              (out.write('HH', obj, graph), out.text(clip), out.text(model), out.write('H', ticks)))
        table(out, sounds, lambda index, duration, script_duration, subtitle: (out.write('HII', index, duration, script_duration), out.text(subtitle)))
        table(out, conversations, lambda name, lines: (out.text(name), table(out, lines,
              lambda delay, variants: (out.write('H', delay), table(out, [(v,) for v in variants], lambda v: out.write('H', v))))))
        table(out, loops, lambda index, loop, tracks: (out.write('HB', index, loop), table(out, tracks,
              lambda *v: out.write('f5i', *v))))
        projections = []
        for part, bsp in enumerate(mission.sections):
            if mission.cache == 'a30' and part == 0:
                projections.append((bsp, ORIGIN_X, ORIGIN_Y, SOURCE_FLOOR, HORIZONTAL_SCALE, VERTICAL_SCALE, MARGIN))
            projection = section_layout(h, mission, part)[-1]
            projections.append((bsp, projection.x0, projection.y0, projection.z0, projection.scale, projection.scale, 9))
        table(out, projections, lambda *v: out.write('H6f', *v))
        table(out, [(i,) for i in worlds], lambda i: out.write('H', i))
        table(out, list(hud_rows(h)), lambda name, text: (out.text(name), out.text(text)))
        table(out, [t.read('f', p + 32) for p in t.block(s + 648, 52)], lambda value: out.write('f', value))
        encounters = list(squad_rows(h, limit_to_normal=mission.cache == 'b30', actors=actor_index(output.parent.parent)))
        table(out, encounters, lambda name, team, bsp, squads, ai:
              (out.text(name), out.write('2h', team, bsp), table(out, squads, lambda name, platoon, starts:
               (out.text(name), out.write('h', platoon), table(out, starts, lambda *v: out.write('h4f', *v))))))
        table(out, [t.read('4h', p) for p in t.block(s + 924, 8)], lambda *v: out.write('4h', *v))
        table(out, [t.read('I', p + 36) for p in t.block(s + 648, 52)], lambda v: out.write('I', v))

        def write_device(*values):
            *definition, path, frames, pose, collision = values
            out.write('4h5IB2Hi8f', *definition)
            out.text(path)
            out.write('H6f', frames, *pose)
            table(out, collision, lambda index, frames:
                  (out.write('H', index), table(out, frames, lambda *box: out.write('6f', *box))))

        table(out, devices, write_device)

        # Version 6: encounter AI data in the same encounter and squad order, then command lists.
        def write_encounter_ai(name, team, bsp, squads, ai):
            positions, platoons, squad_ai = ai
            table(out, positions, lambda *v: out.write('3f2h', *v))
            table(out, platoons, lambda *v: out.write('I4h', *v))
            table(out, squad_ai, lambda *v: (out.write('2hIh7I', *v[:11]),
                                             table(out, v[11], lambda *start: out.write('4h', *start))))

        for encounter in encounters:
            write_encounter_ai(*encounter)
        table(out, list(command_lists(h)), lambda name, flags, atoms, points:
              (out.text(name), out.write('I', flags), table(out, atoms, lambda *v: out.write('2h2f7h', *v)),
               table(out, points, lambda *v: out.write('3f', *v))))
    finally:
        out.file.close()
    path.with_suffix('.tmp').replace(path)


def export_worlds(halo, output, rebuild):
    available = []
    for bsp in range(len(halo.tags.block(halo.scenario + 1444, 32))):
        name = f'halo_scene_{halo.name}_{bsp:02}'
        target = output / 'packages/maps' / f'{name}.mesh'
        if not rebuild and target.is_file():
            if name != 'halo_scene_a10_07':
                available.append(bsp)
                continue
            with target.open('rb') as mesh:
                if mesh.read(4) == b'HWM4':
                    available.append(bsp)
                    continue
        region, address = halo.bsp(bsp)
        if not region.block(address + 260, 32) and not region.block(address + 272, 32):
            continue
        clusters = region.block(address + 308, 104)
        sky = Counter(region.read('h', c)[0] for c in clusters)
        sky.pop(-1, None)
        sky_index = sky.most_common(1)[0][0] if sky else None
        # Cinematic cameras, object transforms, and render BSPs share retail coordinates.
        count = write_world_mesh(halo, output / 'packages', lambda x, y: (x * 4, -y * 4), lambda z: z * 4,
                                 (-math.inf, math.inf, -math.inf, math.inf), map_name=name, bsp=bsp,
                                 include_sky=sky_index is not None, sky_index=sky_index)
        if count:
            available.append(bsp)
        else:
            target.unlink(missing_ok=True)
    return available


def model_pack_scale(model, animated=False):
    extent = max((abs(c) for mesh in model.meshes for point, *_ in mesh.vertices for c in point), default=1)
    return max(100, math.ceil(extent * (48 if animated else 18)))


def export_cast(program, output, rebuild):
    from import_halo_models import AnimationGraph, Model, export

    halo, t = program.halo, program.halo.tags
    directory = output / 'packages/models/halo/scene' / halo.name
    model_cache, graph_cache, idle_cache, walk_cache = {}, {}, {}, {}

    def graph_for(graph_tag):
        if graph_tag.name not in graph_cache:
            graph_cache[graph_tag.name] = AnimationGraph(halo, graph_tag.name)
        return graph_cache[graph_tag.name]

    def idle_pose(tag, model):
        graph_tag = halo.reference(t, tag.address + 56, 'antr')
        if not graph_tag:
            return None
        if graph_tag.name not in idle_cache:
            graph = graph_for(graph_tag)
            candidates = sorted(n for n in graph.animations
                                if n.lower().startswith(('stand unarmed idle', 'stand rifle idle')))
            idle_cache[graph_tag.name] = next(graph.poses(candidates[0])) if candidates else None
        idle = idle_cache[graph_tag.name]
        return idle if idle and all(node in idle for node, _ in model.nodes) else None
    objects, clips = [], []
    for obj, (name, tag, pose) in enumerate(named_objects(program)):
        path, model = '', None
        locomotion = ('', '', '')
        if tag and (ref := halo.reference(t, tag.address + 40)) and ref.group == 'mode':
            def get_model():
                if ref.name not in model_cache:
                    model_cache[ref.name] = Model(halo, ref.name, 'part')
                return model_cache[ref.name]

            slug = f'object_{halo.directory.index(ref)}'
            idle_slug = f'{slug}_idle' if tag.group == 'bipd' else slug
            try:
                model = get_model()
                if rebuild or not (directory / idle_slug / 'tris.md3').is_file():
                    if model.meshes:
                        default_pose = None
                        if tag.group == 'bipd':
                            try:
                                default_pose = idle_pose(tag, model)
                            except (IndexError, KeyError, ValueError, struct.error, ZeroDivisionError, StopIteration):
                                pass
                        export(halo, directory, idle_slug, [model], None, (), 4, quiet=True,
                               geometry_scale=model_pack_scale(model), default_pose=default_pose)
                if (directory / idle_slug / 'tris.md3').is_file():
                    path = f'halo/scene/{halo.name}/{idle_slug}'
                if tag.group == 'bipd' and model.meshes:
                    graph_tag = halo.reference(t, tag.address + 56, 'antr')
                    key = (slug, graph_tag.name) if graph_tag else None
                    if key and key not in walk_cache:
                        graph = graph_for(graph_tag)
                        paths = []
                        for gait in ('move-front', 'move-back', 'move-left'):
                            candidates = sorted((n for n in graph.animations if f' {gait}' in n.lower()),
                                                key=lambda n: ('stand' not in n, 'unarmed' not in n, len(n), n))
                            motion_slug = f'{slug}_{gait}'
                            motion_path = directory / motion_slug / 'tris.md3'
                            if candidates and (rebuild or not motion_path.is_file()):
                                try:
                                    export(halo, directory, motion_slug, [model], graph, [('mapmodel', candidates[0])], 4,
                                           quiet=True, geometry_scale=model_pack_scale(model, animated=True))
                                except (IndexError, KeyError, ValueError, struct.error, ZeroDivisionError):
                                    pass
                            paths.append(f'halo/scene/{halo.name}/{motion_slug}' if motion_path.is_file() else '')
                        walk_cache[key] = tuple(paths)
                    if key:
                        locomotion = walk_cache[key]
            except (IndexError, KeyError, ValueError, struct.error, ZeroDivisionError, StopIteration):
                pass
            for i, (actor, graph_id, clip) in enumerate(program.animations):
                if actor != obj or graph_id >= len(halo.directory):
                    continue
                graph_tag = halo.directory[graph_id]
                try:
                    if graph_tag.group != 'antr':
                        continue
                    graph = graph_for(graph_tag)
                    authored = next(n for n in graph.animations if n.lower() == clip.lower())
                    anim = graph.animations[authored]
                    ticks, = t.read('H', anim + 34)
                    animation = f'{slug}_clip_{i}'
                    if rebuild or not (directory / animation / 'tris.md3').is_file():
                        model = get_model()
                        if not model.meshes:
                            continue
                        export(halo, directory, animation, [model], graph, [('mapmodel', authored)], 4,
                               quiet=True, geometry_scale=model_pack_scale(model, animated=True))
                    clips.append((obj, graph_id, clip, f'halo/scene/{halo.name}/{animation}', ticks))
                except (IndexError, KeyError, ValueError, struct.error, ZeroDivisionError, StopIteration):
                    continue
        objects.append((name, path, pose, cast_metadata(halo, tag, model), locomotion))
    return objects, clips


def convert(job):
    source, output, mission, rebuild = job
    out = output / 'packages/halo'
    out.mkdir(parents=True, exist_ok=True)
    with (source / f'{mission.cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        if halo.name != mission.cache:
            raise ValueError(f'Unexpected mission in {mission.cache}.map')
        program = Program(halo)
        conversations = list(conversation_rows(program))
        loops = list(looping_rows(program))
        audio = output / 'packages/audio/halo/scene' / mission.cache
        audio.mkdir(parents=True, exist_ok=True)
        with ThreadPoolExecutor(max_workers=3) as pool:
            sounds = list(pool.map(lambda index: export_sound(halo, audio, index), sorted(program.sounds)))
        worlds = export_worlds(halo, output, rebuild)
        objects, clips = export_cast(program, output, rebuild)
        devices = list(device_rows(halo, output, rebuild, model_pack_scale))
        write_program(program, mission, out, objects, clips, sounds, conversations, loops, worlds, devices)
        print(f'{mission.cache}: {len(program.scripts)} scripts, {len(sounds)} sound tags, {len(conversations)} conversations, '
              f'{len(worlds)} BSPs, {len(clips)} animation clips, {len(devices)} devices', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--mission', choices=[m.cache for m in MISSIONS])
    parser.add_argument('--rebuild', action='store_true', help='regenerate BSP meshes and cinematic models')
    args = parser.parse_args()
    jobs = [(args.maps, args.output, mission, args.rebuild)
            for mission in MISSIONS if not args.mission or args.mission == mission.cache]
    with ProcessPoolExecutor(max_workers=min(len(jobs), 4)) as pool:
        list(pool.map(convert, jobs))


if __name__ == '__main__':
    main()
