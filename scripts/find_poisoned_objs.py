"""List (and optionally delete) project objects that sccache served from a
SIBLING WORKTREE's build.

WHY THIS MATTERS: the shared object cache at C:/sv-deps/sccache is keyed on
preprocessed source, so two worktrees whose headers agree share objects — which
is the whole point and is normally correct. It stops being correct the moment a
header DISAGREES: a rebase that adds an enum entry, a struct field or a
constant leaves one tree's .obj compiled against the old layout and the other's
against the new, and the linker is happy to combine them. The symptom is a
wandering access violation in code nobody touched.

MEASURED, four times:
  * 2026-09-04, a MaterialDef stride mismatch crashed LoadMicroVox.
  * 2026-09-07 (env-pg-pi), the reverse direction: 73 of a fresh worktree's 767
    objects came from the MAIN CHECKOUT, treeatlas.cpp.obj kept the old
    BiomeDef layout and the exe died in LoadTreeAtlas. Main's paths have no
    `worktrees/` segment, which is why the second probe below exists.
  * 2026-09-08 (W2-A), a rebase onto a main that had added pass::Pipe entries
    produced a 100%-sccache-hit build that crashed in PageTable::ResetIdentity
    on World::Init, writing through a null vector. The give-away is in
    crash.log: the stack frames name SOURCE PATHS FROM OTHER WORKTREES.
        [2] PageTable::ResetIdentity  .../worktrees/agent-a8757.../pagetable.cpp
        [4] World::Init               .../worktrees/agent-abf31.../world.cpp
        [5] main                      .../worktrees/agent-a646b.../main.cpp   <- mine
    25 of 116 objects were foreign. Read the paths in a crash trace before
    diagnosing anything else.
  * 2026-09-09, `Tuning` grew `render.gasBlendStart` in the gas-stage-1b commit.
    80 of main's 156 objects were the gas-stage1 worktree's, from one commit
    earlier; ninja rebuilt only the 4 TUs it watched through its OWN depfiles,
    so main.cpp read `tune.warnings` four bytes short and the game died on the
    first std::string of a startup loop that normally prints nothing. This is
    the one that put the check into build.sh instead of into a habit.

THREE DETECTORS. Run by hand you get all of them; build.sh runs the two that
work, on every build:

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
            caught three of the four incidents.

  default   ...and, for objects the first probe finds nothing in, any absolute
  (cont.)   path naming one of our own files BY ITS PATH RELATIVE TO src/ from
            a root that is neither this tree nor the shared dependency cache.
            That is the 2026-09-07 direction. It has to be the relative path and
            not the basename: on the basename alone the Windows SDK's
            `ucrt/sys/types.h` matches src/audio/xyzpan/.../types.h and 307 of
            310 objects in a freshly-rebuilt tree read as poisoned.

Usage:
  python scripts/find_poisoned_objs.py            # report
  python scripts/find_poisoned_objs.py --delete   # report + remove
  python scripts/find_poisoned_objs.py --deps     # the old depfile probe too

  --config Release       only objects under a .../Release/... path
  --newer-than FILE      only objects modified after FILE's mtime
  --exit-code            exit 1 if anything is still poisoned when we finish

The last two are what make this affordable to run on EVERY build, which is how
scripts/build.sh uses it (between the compile phase and the link). A poisoned
object is always one sccache has JUST written, so a stamp file touched after
each clean check narrows the steady-state scan from ~1.5 GB of object bytes to
whatever this build actually compiled -- usually nothing.

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

# ---- second probe: an object built in the MAIN CHECKOUT ---------------------
# The regex above cannot see one, because main's path has no `worktrees/`
# segment at all -- and that is a real face of this bug, not a hypothetical
# (2026-09-07, env-pg-pi: 73 of a worktree's 767 objects came from main,
# treeatlas.cpp.obj kept the old BiomeDef layout, the exe died in
# LoadTreeAtlas). So also flag any absolute path that names one of OUR OWN
# source basenames from somewhere that is not this tree.
#
# The shared dependency cache is exempt and MUST be: Dawn, Tint and Jolt live at
# one path for every worktree, which is the entire point of
# FETCHCONTENT_BASE_DIR, and they contribute 66 basename collisions with src/
# (main.cpp x40, types.h, context.h, stream.h, support.h, crash.cpp, ...).
# Without the exemption every dependency object reads as foreign.
DEPS = 'c:/sv-deps'
_cache = os.path.join(ROOT, 'build', 'CMakeCache.txt')
if os.path.exists(_cache):
    with open(_cache, encoding='utf-8', errors='replace') as fh:
        for line in fh:
            if line.startswith('FETCHCONTENT_BASE_DIR:'):
                DEPS = line.split('=', 1)[1].strip().replace('\\', '/').lower()
                break
#
# A BASENAME is not enough to identify one of our files, and testing that cost a
# run: the Windows SDK's `ucrt/sys/types.h` matches src/audio/xyzpan/.../types.h,
# which condemned 307 of 310 objects in a tree that had just been rebuilt clean.
# The test is the whole path RELATIVE TO src/ -- `.../src/sim/world.h` is ours,
# `.../ucrt/sys/types.h` is not, and neither is the CRT's `.../src/vctools/crt/
# .../exe_common.inl` that every object carries from __scrt_common_main_seh.
ROOT_SLASH = ROOT.replace('\\', '/').lower()
SRC = os.path.join(ROOT, 'src')
MY_RELS = {}
for _r, _d, _fs in os.walk(SRC):
    for _n in _fs:
        if _n.lower().endswith(('.cpp', '.h', '.hpp', '.inl', '.def')):
            _rel = os.path.relpath(os.path.join(_r, _n), SRC)
            MY_RELS.setdefault(_n.lower(), set()).add(
                '/src/' + _rel.replace('\\', '/').lower())
# Drive-rooted, ending in a source extension. Debug info keeps these in a string
# table, so excluding the control bytes and ':' is what stops one match from
# running into the next entry.
ABSPATH = re.compile(
    rb'[A-Za-z]:[\\/][^\x00-\x1f:<>|*?"]{1,300}?\.(?:cpp|h|hpp|inl|def)(?![A-Za-z0-9_])')


def foreign_sources(blob):
    """Absolute paths naming one of our own sources from outside this tree."""
    out = set()
    for mm in ABSPATH.finditer(blob):
        path = mm.group(0).decode('latin-1').replace('\\', '/').lower()
        if path.startswith(ROOT_SLASH) or DEPS in path:
            continue
        for tail in MY_RELS.get(path.rsplit('/', 1)[-1], ()):
            if path.endswith(tail):
                out.add(path[:-len(tail)])
                break
    return out


def _argval(flag, default=None):
    return sys.argv[sys.argv.index(flag) + 1] if flag in sys.argv else default

BUILD = os.path.join(ROOT, 'build')
# --config Release: the other configs' objects cannot reach the exe we are
# about to link, so scanning them is pure IO.
CONFIG = _argval('--config')
# --newer-than STAMP: everything older was scanned by an earlier run and has
# not been rewritten since, so its verdict still stands.
SINCE = 0.0
stamp = _argval('--newer-than')
if stamp and os.path.exists(stamp):
    SINCE = os.path.getmtime(stamp)

objs = []
for root, _dirs, files in os.walk(BUILD):
    # Only the project's own objects: the dependency builds (Dawn, Jolt) live
    # under C:/sv-deps and are not worktree-specific.
    if 'sandvox' not in root:
        continue
    if CONFIG and (os.sep + CONFIG + os.sep) not in root + os.sep:
        continue
    for f in files:
        if f.endswith('.obj'):
            path = os.path.join(root, f)
            if SINCE and os.path.getmtime(path) <= SINCE:
                continue
            objs.append(path)

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
    if not foreign:
        # Only worth the second, more expensive probe when the cheap one is
        # quiet: an object already tagged with a sibling worktree is condemned.
        elsewhere = foreign_sources(blob)
        if elsewhere:
            foreign = sorted(t + '/src/...' for t in elsewhere)[:2]
    if foreign:
        poisoned[obj] = foreign

scope = []
if CONFIG:
    scope.append(CONFIG)
if SINCE:
    scope.append('newer than the last clean check')
print('poison check: %d project object%s scanned%s, %d built in another worktree'
      % (len(objs), '' if len(objs) == 1 else 's',
         ' (%s)' % ', '.join(scope) if scope else '', len(poisoned)))
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

if '--delete' in sys.argv and poisoned:
    for obj in poisoned:
        if os.path.exists(obj):
            os.remove(obj)
    print('deleted %d -- now rebuild with SCCACHE_RECACHE=1, or the cache serves '
          'them straight back' % len(poisoned))
elif poisoned:
    print('re-run with --delete, then: SCCACHE_RECACHE=1 bash scripts/build.sh')

# --exit-code says "something was wrong", NOT "something is still wrong": with
# --delete the objects are gone but the build they belonged to is still not
# linkable, so the caller has to recompile either way.
if '--exit-code' in sys.argv:
    sys.exit(1 if poisoned else 0)
