"""The C++ mission table matches the importer, so maps and runtime agree.

source/src/halo_campaign.h and tools/import_halo_campaign.py both list the ten
missions. This runs without assets or a Mac.
"""
import re
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from qa_common import *  # noqa: E402,F403
sys.path.insert(0, str(root / 'tools'))
from import_halo_campaign import MISSIONS  # noqa: E402

header = (root / 'source/src/halo_campaign.h').read_text()
rows = re.findall(r'\{"(\w+)", "([^"]+)", (\d+), "[^"]*"\}', header)
assert len(rows) == len(MISSIONS), (len(rows), len(MISSIONS))
# Halo's first section is the standalone Crash Site map from import_halo_a30.py.
extra = {'a30': 1}
for (cache, title, sections), mission in zip(rows, MISSIONS):
    expected = (mission.cache, mission.title, len(mission.sections) + extra.get(mission.cache, 0))
    assert (cache, title, int(sections)) == expected, ((cache, title, sections), expected)

# Cinematic cues name zero-based sections, or -1 (any section) for the escape timeout and for
# scenes a script calls; an exit variant adds its vehicle, volume and site flag.
script = (root / 'source/src/halo_script.cpp').read_text()
cues = re.findall(r'\{(\d+), (-?\d+), (\d+), "(\w+)", SceneTrigger::(\w+)(?:, "\w+", "\w+", "\w+")?\}', script)
assert cues, 'cue table not found'
for mission, section, bsp, name, trigger in cues:
    m, s = int(mission), int(section)
    assert m < len(MISSIONS), (name, m)
    assert trigger in ('Entry', 'Exit', 'Timeout', 'Cartographer', 'Call'), (name, trigger)
    if trigger == 'Timeout':
        assert s == -1, (name, m, s)
    else:
        assert (s == -1 and trigger == 'Call') or 0 <= s < int(rows[m][2]), (name, m, s)
print(f'{len(rows)} missions and {len(cues)} cinematic cues agree with the importer.')
