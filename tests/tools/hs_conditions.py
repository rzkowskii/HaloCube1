"""Prints the calls inside every script condition of the retail missions.

Reads the compiled HaloScript from the expanded Xbox maps (same node layout as
inspect_hs.py) and lists, per mission, each call name that appears inside a
sleep_until, if, cond or while condition together with the scripts using it.
With --all every call is listed, not just those in conditions.
Output: one JSON object {mission: [{call: [script, ...]}, [every script name]]}.
"""
import json
import mmap
import struct
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_a30 import HaloMap  # noqa: E402

IDS = 'a10 a30 a50 b30 b40 c10 c20 c40 d20 d40'.split()
CONDITIONAL = ('sleep_until', 'if', 'cond', 'while')


def mission_calls(cache, every=False):
    with (HALO_SOURCE / f'{cache}.map').open('rb') as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
        h = HaloMap(m)
        t, a = h.tags, h.scenario
        p = t.read('I', a + 1152)[0]
        s = t.read('I', a + 1172)[0]
        ln = t.read('I', a + 1160)[0]
        roots = [(t.name(b, 32), *t.read('HHI', b + 32)) for b in t.block(a + 1180, 92)]
        nodes = [t.read('4H3I', p + 56 + 20 * i) for i in range((t.read('I', a + 1140)[0] - 56) // 20)]

        def name(i):
            return t.name(s + i, 256) if i < ln else str(i)

        def children(i):
            data = nodes[i & 65535][6]
            out = []
            while data != 0xffffffff:
                out.append(data)
                data = nodes[data & 65535][4]
            return out

        def call_name(i):
            _, op, ty, flag, nxt, st, data = nodes[i & 65535]
            if flag & 1:
                return None
            kids = children(i)
            if not kids:
                return None
            head = nodes[kids[0] & 65535]
            return name(head[5]) if head[3] & 1 else None

        def calls_in(i, out):
            _, op, ty, flag, nxt, st, data = nodes[i & 65535]
            if flag & 1:
                return
            kids = children(i)
            if kids:
                head = nodes[kids[0] & 65535]
                if head[3] & 1:
                    out.add(name(head[5]))
                    kids = kids[1:]
            for kid in kids:
                calls_in(kid, out)

        found = {}

        def walk(i, script, conditional):
            _, op, ty, flag, nxt, st, data = nodes[i & 65535]
            if flag & 1:
                return
            kids = children(i)
            if not kids:
                return
            head = nodes[kids[0] & 65535]
            head_name = name(head[5]) if head[3] & 1 else None
            rest = kids[1:] if head_name else kids
            if every and head_name:
                found.setdefault(head_name, set()).add(script)
            if head_name in CONDITIONAL and rest:
                # sleep_until/while/if take the condition first; cond takes clauses.
                targets = rest if head_name == 'cond' else rest[:1]
                for kid in targets:
                    calls = set()
                    calls_in(kid, calls)
                    for call in calls:
                        found.setdefault(call, set()).add(script)
                    walk(kid, script, conditional)
                for kid in rest:
                    if kid not in targets:
                        walk(kid, script, conditional)
                return
            for kid in rest:
                walk(kid, script, conditional)

        for script, kind, ty, root_node in roots:
            walk(root_node, script, False)
        return ({call: sorted(scripts) for call, scripts in sorted(found.items())}, [r[0] for r in roots])


if __name__ == '__main__':
    every = '--all' in sys.argv
    ids = [a for a in sys.argv[1:] if a != '--all'] or IDS
    print(json.dumps({cache: mission_calls(cache, every) for cache in ids}))
