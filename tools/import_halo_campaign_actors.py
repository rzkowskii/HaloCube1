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
        # 343 Guilty Spark: its graph has only the idle; it floats, so the idle serves throughout.
        ('monitor', 'monitor', 'stand unarmed idle', 'stand unarmed idle', 'stand unarmed idle'),
    ),
    # Named and passive bodies (halo.cpp haloactormodel): Captain Keyes for the captain and
    # captain_ingame variants, the Engineer (b30's cache is the only one that carries it; its idle
    # and move clips, the alert idle as its 'attack': it never fights).
    'a50': (
        ('keyes', 'captain', 'stand unarmed idle%1', 'stand unarmed move-front', 'alert unarmed idle%1'),
    ),
    'b30': (
        ('engineer', 'engineer', 'stand unarmed idle%1', 'stand unarmed move-front', 'alert unarmed idle%1'),
    ),
}
# Bodies whose graph has no landing-dead clip die on their idle.
NO_DEATH = {'monitor', 'engineer'}
# Bodies exported in their bind pose if their animation tracks cannot be read. b30's Engineer graph
# has no node table of its own (its animations name the model's 25 nodes in order, as the device
# graphs do: tools/halo_devices.py); with the model's hierarchy its tracks read, so it is animated.
STATIC_FALLBACK = {'engineer'}


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
                # A graph without a node table animates the model's nodes in order.
                graph = AnimationGraph(halo, path, model.hierarchy)
                death = idle if name in NO_DEATH else 'stand landing-dead'
                clips = [('idle', idle), ('run', walk), ('death', death), ('lying dead', death)]
                kind, = graph.tags.read('H', graph.animations[attack] + 32)
                clips.append(('attack', (attack, idle) if kind == 1 else attack))
                markers = [('tag_head', 'head')] if 'head' in model.markers else []
                try:
                    export(halo, directory, name, [model], graph, clips, 5.6, markers)
                except ValueError as error:
                    if name not in STATIC_FALLBACK:
                        raise
                    # A graph whose tracks the exporter cannot read: the body in its bind pose,
                    # which still floats and turns in the game.
                    print(f'{name}: {error}; exported in its bind pose')
                    export(halo, directory, name, [model], None, (), 5.6, markers)


if __name__ == '__main__':
    main()
