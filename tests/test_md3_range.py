"""MD3 range: a cinematic export whose posed frames overflow the 16-bit vertex range at the requested
geometry scale is packed per frame about the frame's own origin (or, when one frame is still too big,
at a larger scale) instead of failing, and failed and orphan exports do not linger.

    python3 tests/test_md3_range.py

No game, no caches: a synthetic one-bone model, its skin writer stubbed.

  * `export` (tools/import_halo_models.py) on a model that fits at mdlscale 100 returns 100, writes
    `mdlscale 100` and packs every vertex exactly as `pack_vertex` does (exports that fit are byte for
    byte what they were before the range fix).
  * The same model moved 600 units along a frame (root motion, as a10's lifepods or d20's jump down)
    keeps mdlscale 100, names every frame ORIGIN_FRAME with the centre of its box as local_origin, and
    the engine's decode ((short / 64 + local_origin) * mdlscale / 100 / 16: md3.h's halo_origin load,
    vertmodel.h scaleverts) gives back 4 x the posed position within one vertex step.
  * A model whose single frame is past the range (300 units across) gets the smallest mdlscale that
    fits, written to md3.cfg, every packed coordinate inside MD3_LIMIT, and decodes the same way.
  * An export with no surface left (its only skin fails) raises and removes the directory it created;
    one that fails in a directory that was already there leaves it.
  * `prune_scene_models` (tools/import_halo_scripts.py) removes only directories named like the cast's
    and devices' exports that the owned set leaves out, and lists without removing when asked.
  * With the generated assets: the 27 clips the fix brought back (c10's x50 Chief, marines and
    infection form, a50's lift marines and Chief, d20's jump down and lift, a10's and a30's lifepods)
    are in their `.hscb`, every clip model a `.hscb` names has its tris.md3 and an integer mdlscale of
    at least 100, and no `packages/models/halo/scene/<map>/` directory lacks its tris.md3.

Written while testing was on hold: not yet run.
"""
import struct
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

root = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(root / 'tools'))
import import_halo_models as models  # noqa: E402
from import_halo_models import MD3_LIMIT, ORIGIN_FRAME, Mesh, Transform, export, md3_axes, pack_vertex  # noqa: E402
from import_halo_scripts import prune_scene_models  # noqa: E402

problems = []


def check(ok, message):
    if not ok:
        problems.append(message)


def fake_skin(halo, bitmap, destination, tint=None):
    if bitmap == 'bad':
        raise ValueError('unsupported bitmap')
    destination.write_bytes(b'png')


models.skin = fake_skin
IDENTITY = Transform(((1, 0, 0), (0, 1, 0), (0, 0, 1)), (0.0, 0.0, 0.0))
POINTS = [(1.5, -0.75, 2.0), (-1.25, 0.5, 0.0), (0.25, 1.0, -0.5)]


def model(bitmap='ok'):
    vertices = [(p, (0.0, 0.0, 1.0), (0.0, 0.0), [(0, 1.0)]) for p in POINTS]
    mesh = Mesh('part_0', bitmap, None, [(0, 1, 2)], vertices)
    return SimpleNamespace(nodes=[('root', IDENTITY)], meshes=[mesh], markers={}, bind_pose={'root': IDENTITY})


def read_md3(path):
    """Frame count, vertex count, the packed vertices, their bytes, and each frame's (name, local_origin)."""
    data = path.read_bytes()
    _, _, _, _, frames, _, surfaces, _, ofs_frames, _, ofs_meshes, _ = struct.unpack_from('<4si64s9i', data)
    records = [struct.unpack_from('<10f16s', data, ofs_frames + 56*f) for f in range(frames)]
    origins = [(r[10].rstrip(b'\0'), r[6:9]) for r in records]
    _, _, _, numframes, _, numverts, _, _, _, _, ofs_xyz, _ = struct.unpack_from('<4s64s10i', data, ofs_meshes)
    start = ofs_meshes + ofs_xyz
    shorts = [struct.unpack_from('<3hH', data, start + 8*i) for i in range(numframes*numverts)]
    return frames, numverts, shorts, data[start:start + 8*numframes*numverts], origins


def decodes(label, directory, poses, points, used):
    """md3.h's load (a halo_origin frame's local_origin added) and scaleverts, against 4 x the pose."""
    frames, count, shorts, _, origins = read_md3(directory / 'tris.md3')
    check(frames == len(poses) and count == len(points), f'{label}: {frames} frames of {count} vertices')
    check(all(abs(v) <= MD3_LIMIT*64 for row in shorts for v in row[:3]), f'{label}: a packed coordinate past MD3_LIMIT')
    step = used / 100 / 16 / 64
    for f, pose in enumerate(poses):
        name, origin = origins[f]
        offset = origin if name == ORIGIN_FRAME else (0.0, 0.0, 0.0)
        for i, p in enumerate(points):
            engine = [(v / 64 + o) * used / 100 / 16 for v, o in zip(shorts[f*count + i][:3], offset)]
            want = [4 * c for c in md3_axes(pose.point(p))]
            check(all(abs(a - b) <= step + 1e-3 for a, b in zip(engine, want)),
                  f'{label} frame {f} vertex {i}: decodes to {engine}, expected {want}')
    return origins


def mdlscale(directory):
    line = next(line for line in (directory / 'md3.cfg').read_text().splitlines() if line.startswith('mdlscale '))
    return int(line.split()[1])


with tempfile.TemporaryDirectory(prefix='halo-md3-') as scratch:
    scratch = Path(scratch)

    # Fits: scale 100, bytes as pack_vertex packs them.
    used = export(None, scratch, 'small', [model()], None, (), 4, quiet=True, geometry_scale=100)
    check(used == 100 and mdlscale(scratch / 'small') == 100, f'small model: mdlscale {used}, expected 100')
    _, _, _, packed, origins = read_md3(scratch / 'small/tris.md3')
    check(all(name == b'' and origin == (0.0, 0.0, 0.0) for name, origin in origins), 'small model: frames carry an origin')
    expected = b''.join(pack_vertex(tuple(c * 4*1600/100 for c in md3_axes(p)), (0.0, 0.0, 1.0)) for p in POINTS)
    check(packed == expected, 'small model: vertices not packed as pack_vertex packs them')

    # Root motion past the range: per-frame origins, the scale and step of the model's own size.
    far = Transform(IDENTITY.rows, (600.0, 0.0, 0.0))
    used = export(None, scratch, 'far', [model()], None, (), 4, quiet=True, geometry_scale=100,
                  position_poses=[{'root': IDENTITY}, {'root': far}])
    check(used == 100 and mdlscale(scratch / 'far') == 100, f'far model: mdlscale {used}, expected 100 with frame origins')
    origins = decodes('far model', scratch / 'far', (IDENTITY, far), POINTS, used)
    check(all(name == ORIGIN_FRAME for name, _ in origins), 'far model: a frame without its origin')

    # One frame past the range on its own: the smallest larger scale that fits.
    big = [tuple(200 * c for c in p) for p in POINTS]
    vertices = [(p, (0.0, 0.0, 1.0), (0.0, 0.0), [(0, 1.0)]) for p in big]
    huge = SimpleNamespace(nodes=[('root', IDENTITY)], meshes=[Mesh('part_0', 'ok', None, [(0, 1, 2)], vertices)],
                           markers={}, bind_pose={'root': IDENTITY})
    used = export(None, scratch, 'huge', [huge], None, (), 4, quiet=True, geometry_scale=100)
    axes = list(zip(*(md3_axes(p) for p in big)))
    half = max((max(a) - min(a)) / 2 for a in axes)
    check(used > 100 and mdlscale(scratch / 'huge') == used, f'huge model: mdlscale {used}, md3.cfg {mdlscale(scratch / "huge")}')
    check(half * 4*1600 / used <= MD3_LIMIT and half * 4*1600 / (used - 1) > MD3_LIMIT * 0.999,
          f'huge model: mdlscale {used} is not the smallest that fits')
    decodes('huge model', scratch / 'huge', (IDENTITY,), big, used)

    # A failed export removes the directory it created, and only that.
    try:
        export(None, scratch, 'bad', [model('bad')], None, (), 4, quiet=True)
        check(False, 'bad model: export did not raise')
    except ValueError:
        pass
    check(not (scratch / 'bad').exists(), 'bad model: the failed export left its directory')
    (scratch / 'kept').mkdir()
    try:
        export(None, scratch, 'kept', [model('bad')], None, (), 4, quiet=True)
    except ValueError:
        pass
    check((scratch / 'kept').is_dir(), 'bad model: a failed export removed a directory it did not create')

    # Orphan pruning.
    scene = scratch / 'scene'
    names = ['object_1', 'object_1_clip_2', 'object_1_move-front', 'object_9_idle', 'device_3', 'device_4', 'notes', 'object_x']
    for name in names:
        (scene / name).mkdir(parents=True)
    owned = {'object_1', 'device_3'}
    listed = prune_scene_models(scene, owned, remove=False)
    want = ['device_4', 'object_1_clip_2', 'object_1_move-front', 'object_9_idle']
    check(listed == want, f'prune_scene_models listed {listed}, expected {want}')
    check(all((scene / name).is_dir() for name in names), 'prune_scene_models removed a directory when only listing')
    check(prune_scene_models(scene, owned) == want, 'prune_scene_models removed a different set')
    left = sorted(p.name for p in scene.iterdir())
    check(left == ['device_3', 'notes', 'object_1', 'object_x'], f'prune_scene_models left {left}')

# The generated assets, when imported: the clips the range fix brought back are in their .hscb, every
# scene model a .hscb names has its tris.md3 and an integer mdlscale, and no scene model directory
# lacks its tris.md3 (an orphan or a failed export the import would have pruned).
RESTORED = {   # (object, clip) pairs the 16-bit range used to drop
    'a10': {('lifeboat_x30_docked', 'takeoff'), ('lifeboat_x30_double', 'flight1'),
            ('lifeboat_x30_double', 'flight2'), ('lifeboat_burn', 'idle_fall')},
    'a30': {('intro_pod', 'vehicle airbrakes-off')},
    'a50': {(f'lift_marine_{n}', 'a50energylift') for n in range(1, 7)} | {('chief_lift', 'a50energylift')},
    'c10': {('chief', c) for c in ('x50_0710', 'x50_1210', 'x50_1410', 'x50_1910', 'x50_2010')}
           | {('mendoza_dead', c) for c in ('x50_1010mendoza', 'x50_1210mendoza', 'x50_1410mendoza')}
           | {('mendoza', 'x50_3310bisenti'), ('mendoza', 'x50_4110mendoza'), ('bisenti', 'x50_4110bisenti'),
              ('marine_2', 'x50_4110mar2'), ('infection_1', 'x50_4110flood')},
    'd20': {('chief', 'd20jumpdown'), ('chief', 'a50energylift')},
}
sys.path.insert(0, str(root / 'tests'))
from qa_common import read_hscb  # noqa: E402
scripts = sorted((root / 'packages/halo').glob('*.hscb'))
for script in scripts:
    cache = script.stem
    program = read_hscb(script)
    clips = {(program['objects'][obj].lower(), clip.lower()) for obj, _, clip, _, _ in program['animations']}
    missing = RESTORED.get(cache, set()) - clips
    check(not missing, f'{cache}.hscb: clips still left out: {sorted(missing)}')
    for _, _, clip, path, _ in program['animations']:
        model = root / 'packages/models' / path
        if not (model / 'tris.md3').is_file():
            check(False, f'{cache}.hscb: clip {clip} names {path}, which has no tris.md3')
            continue
        lines = [line for line in (model / 'md3.cfg').read_text().splitlines() if line.startswith('mdlscale ')]
        check(len(lines) == 1 and lines[0].split()[1].isdigit() and int(lines[0].split()[1]) >= 100,
              f'{path}: md3.cfg mdlscale {lines}')
    scene = root / 'packages/models/halo/scene' / cache
    if scene.is_dir():
        empty = sorted(d.name for d in scene.iterdir() if d.is_dir() and not (d / 'tris.md3').is_file())
        check(not empty, f'{cache}: scene model directories without tris.md3: {empty[:8]}')
if not scripts:
    print('No packages/halo/*.hscb: the generated-asset checks were skipped.')

if problems:
    raise SystemExit('\n'.join(problems))
print('MD3 exports pick their scale from the posed extent, failed exports and orphans are removed.')
