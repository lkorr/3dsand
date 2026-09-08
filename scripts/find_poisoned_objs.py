"""List (and optionally delete) project objects that sccache served from a
SIBLING WORKTREE's build.

WHY THIS MATTERS: the shared object cache at C:/sv-deps/sccache is keyed on
preprocessed source, so two worktrees whose headers agree share objects — which
is the whole point and is normally correct. It stops being correct the moment a
header DISAGREES: a rebase that adds an enum entry, a struct field or a
constant leaves one tree's .obj compiled against the old layout and the other's
against the new, and the linker is happy to combine them. The symptom is a
wandering access violation in code nobody touched.

MEASURED, twice:
  * 2026-09-04, a MaterialDef stride mismatch crashed LoadMicroVox.
  * 2026-09-08 (W2-A), a rebase onto a main that had added pass::Pipe entries
    produced a 100%-sccache-hit build that crashed in PageTable::ResetIdentity
    on World::Init, writing through a null vector. The give-away is in
    crash.log: the stack frames name SOURCE PATHS FROM OTHER WORKTREES.
        [2] PageTable::ResetIdentity  .../worktrees/agent-a8757.../pagetable.cpp
        [4] World::Init               .../worktrees/agent-abf31.../world.cpp
        [5] main                      .../worktrees/agent-a646b.../main.cpp   <- mine
    25 of 116 objects were foreign. Read the paths in a crash trace before
    diagnosing anything else.

TWO DETECTORS, and the second is the one that works:

  --deps    the original: ask ninja whose HEADERS each object depends on. It
            needs ninja, a build.ninja, and a depfile that came across with the
            cached object. It found 0 of the 25 above, because this script used
            to chdir into scripts/ — where there is no build.ninja, so `ninja -t
            targets` printed nothing and it reported "0 project objects, 0
            poisoned", which reads exactly like "your tree is clean".

  default   grep the OBJECT BYTES for a worktree path that is not this one. The
            debug info records the absolute path of every source and header the
            compiler saw, so a foreign object says so in plain ASCII. No ninja,
            no depfile, no generator assumptions — and it is what actually
            caught both incidents.

Usage:
  python scripts/find_poisoned_objs.py            # report
  python scripts/find_poisoned_objs.py --delete   # report + remove
  python scripts/find_poisoned_objs.py --deps     # the old depfile probe too

AFTER DELETING, REBUILD WITH SCCACHE_RECACHE=1. Removing the .obj alone just
makes the next build fetch the same poisoned object out of the shared cache
again; the env var forces a real compile that also repairs the cache entry.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

# Which worktree is THIS one? Everything else named inside an object is foreign.
# A checkout that is not a worktree at all has no marker, so every worktree path
# found in an object is foreign to it — which is the correct answer for main.
m = re.search(r'[\\/]worktrees[\\/]([^\\/]+)', ROOT)
MINE = m.group(1).encode() if m else b'\x00\x00no-worktree\x00\x00'
WORKTREE = re.compile(rb'worktrees[\\/]([A-Za-z0-9_.-]+)')

BUILD = os.path.join(ROOT, 'build')
objs = []
for root, _dirs, files in os.walk(BUILD):
    # Only the project's own objects: the dependency builds (Dawn, Jolt) live
    # under C:/sv-deps and are not worktree-specific.
    if 'sandvox' not in root:
        continue
    for f in files:
        if f.endswith('.obj'):
            objs.append(os.path.join(root, f))

poisoned = {}
for obj in objs:
    try:
        with open(obj, 'rb') as fh:
            blob = fh.read()
    except OSError:
        continue
    foreign = sorted({m.group(1).decode('latin-1')
                      for m in WORKTREE.finditer(blob)
                      if m.group(1) != MINE})
    if foreign:
        poisoned[obj] = foreign

print('%d project objects, %d built in another worktree'
      % (len(objs), len(poisoned)))
for obj in sorted(poisoned):
    print('  %-58s <- %s' % (os.path.relpath(obj, BUILD), ', '.join(poisoned[obj])))

if '--deps' in sys.argv:
    # The original probe, kept because a depfile can be wrong in ways the bytes
    # are not (ninja watching a sibling's headers means it will never rebuild
    # this object when YOUR headers change, even if the object itself is fine).
    # Run from the build dir, which is the bug that made it silent.
    os.chdir(BUILD)
    listed = [l.split(':')[0]
              for l in subprocess.run(['ninja', '-t', 'targets', 'all'],
                                      capture_output=True, text=True).stdout.splitlines()
              if re.search(r'sandvox(_core)?\.dir.*\.obj', l)]
    listed = sorted(set(o.replace('/Debug/', '/Release/') for o in listed) | set(listed))
    pat = re.compile(r'^    \.\./\.\./[^/]+/src/')
    stale = [o for o in listed
             if any(pat.match(l) for l in
                    subprocess.run(['ninja', '-t', 'deps', o],
                                   capture_output=True, text=True).stdout.splitlines())]
    print('--deps: %d targets listed, %d with a sibling depfile' % (len(listed), len(stale)))
    for s in stale:
        print('  ' + s)
    os.chdir(ROOT)

if '--delete' in sys.argv:
    for obj in poisoned:
        if os.path.exists(obj):
            os.remove(obj)
    print('deleted %d -- now rebuild with SCCACHE_RECACHE=1, or the cache serves '
          'them straight back' % len(poisoned))
elif poisoned:
    print('re-run with --delete, then: SCCACHE_RECACHE=1 bash scripts/build.sh')
