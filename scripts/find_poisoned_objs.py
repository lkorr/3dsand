"""List (and optionally delete) project objects whose ninja depfile names a
SIBLING worktree's headers: sccache served the .obj AND its depfile from a
build in another worktree, so ninja watches the wrong files and never rebuilds
them when this tree's headers change (a MaterialDef stride mismatch crashed
LoadMicroVox that way, 2026-09-04)."""
import os, re, subprocess, sys
os.chdir(os.path.dirname(os.path.abspath(__file__)))
objs = [l.split(':')[0] for l in subprocess.run(['ninja', '-t', 'targets', 'all'], capture_output=True, text=True).stdout.splitlines()
        if re.search(r'sandvox(_core)?\.dir.*\.obj', l)]
# multi-config: the targets list names the default config; probe every config
objs = sorted(set(o.replace('/Debug/', '/Release/') for o in objs) | set(objs))
poisoned = []
pat = re.compile(r'^    \.\./\.\./[^/]+/src/')
for obj in objs:
    out = subprocess.run(['ninja', '-t', 'deps', obj], capture_output=True, text=True).stdout
    if any(pat.match(l) for l in out.splitlines()):
        poisoned.append(obj)
print('%d project objects, %d poisoned' % (len(objs), len(poisoned)))
for p in poisoned:
    print('  ' + p)
if '--delete' in sys.argv:
    for p in poisoned:
        if os.path.exists(p):
            os.remove(p)
    print('deleted %d' % len(poisoned))
