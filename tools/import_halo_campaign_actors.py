#!/usr/bin/env python3
"""Bake additional original Halo CE campaign enemy models and animations."""
import argparse
import mmap
from pathlib import Path

from import_halo_a30 import HaloMap
from import_halo_models import AnimationGraph, Model, export


ACTORS = {
    'a50': (
        ('jackal', 'jackal', 'stand pistol idle', 'stand pistol move-front', 'stand pistol pr fire-1'),
        ('hunter', 'hunter', 'alert unarmed idle%0', 'stand unarmed move-front', 'stand unarmed melee'),
    ),
    'c20': (
        ('flood-elite', 'floodcombat elite', 'stand pistol idle', 'stand pistol move-front%0', 'stand pistol melee%0'),
        ('flood-human', 'floodcombat_human', 'stand pistol idle%0', 'stand pistol move-front', 'stand pistol melee'),
        ('infection', 'flood_infection', 'stand unarmed idle', 'stand unarmed move-front', 'stand unarmed melee'),
        ('carrier', 'floodcarrier', 'stand unarmed idle', 'stand unarmed move-front%0', 'stand unarmed melee'),
        ('sentinel', 'sentinel', 'stand fixed idle%0', 'stand fixed idle%0', 'alert fixed idle%0'),
    ),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('maps', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    directory = args.output / 'packages/models/halo'
    with (args.maps / 'd40.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        halo = HaloMap(data)
        if halo.name != 'd40':
            raise ValueError('Wrong cache in d40.map')
        warthog = Model(halo, r'vehicles\warthog\warthog', 'vehicle')
        export(halo, directory, 'warthog', [warthog], None, (), 4.5)

    for cache, actors in ACTORS.items():
        with (args.maps / f'{cache}.map').open('rb') as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
            halo = HaloMap(data)
            if halo.name != cache:
                raise ValueError(f'Wrong cache in {cache}.map')
            for name, folder, idle, walk, attack in actors:
                path = rf'characters\{folder}\{folder}'
                model = Model(halo, path, 'biped')
                graph = AnimationGraph(halo, path)
                clips = [('idle', idle), ('run', walk), ('death', 'stand landing-dead'), ('lying dead', 'stand landing-dead')]
                kind, = graph.tags.read('H', graph.animations[attack] + 32)
                clips.append(('attack', (attack, idle) if kind == 1 else attack))
                markers = [('tag_head', 'head')] if 'head' in model.markers else []
                export(halo, directory, name, [model], graph, clips, 5.6, markers)


if __name__ == '__main__':
    main()
