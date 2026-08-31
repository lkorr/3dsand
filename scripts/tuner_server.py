#!/usr/bin/env python3
"""Local dev server for assets/tuner.html.

Opened as a file:// page the tuner is sandboxed: it cannot find the assets
folder on its own (the File System Access API only hands out a directory after
an explicit user gesture), and it certainly cannot build or launch the engine.
Serving it from here lifts all three limits, because the privileged work
happens in this process rather than in the page:

  GET  /                      the tuner, served from assets/
  GET  /api/files             materials.json + reactions.json + tuning.json
                              + items/items.json
                              (+ spells/glyphs.json, READ-ONLY, for the Wiki)
  POST /api/save              write those files back
  GET  /api/models            list .vox/.json under
                              assets/{models,mobs,microvox,items,limbs}
  GET  /api/model?path=...    read one of those files (bytes for .vox)
  POST /api/model?path=...    write one of those files
  GET  /api/shaders           list assets/shaders/*.wgsl with their text
  GET  /api/sounds            the whole assets/sounds/ tree, grouped into sets
  GET  /api/sound?path=...    stream one audio file (audition in the page)
  POST /api/sound/import      write a dropped file into a set folder
  POST /api/sound/rename      rename one variant within its set
  POST /api/sound/move        move a variant to another set
  POST /api/sound/delete      delete one variant (to assets/sounds/.trash/)
  POST /api/sound/set         create an empty set folder
  GET  /api/notes             list note pages under notes/
  GET  /api/note?name=...     read one note page
  POST /api/note?name=...     write one note page (autosave)
  POST /api/note/delete       delete one note page
  POST /api/build             cmake --build ... --target sandvox
  POST /api/play              launch build/Release/sandvox.exe (body {mode,scene}:
                              "game", or "lab" = fluid testing world --lab <scene>)
  GET  /api/heightmap?...     terrain map from `sandvox --heightmap` (binary)
  GET  /api/voxregion?...     a BOX OF REAL VOXELS from `sandvox --voxserve`
  GET  /api/voxpalette        the compiled material table the viewer colours with
  GET  /api/tuning-defaults   worldgen's COMPILED-IN defaults, from the engine
  POST /api/voxreload         re-read tuning.json + shaders in the voxel server
  GET  /api/worldedits        list assets/worldedits/*.svedit
  GET  /api/worldedit?name=   read one edit layer (binary 'SVED')
  POST /api/worldedit?name=   write one edit layer
  POST /api/worldedit/delete  delete one edit layer
  GET  /api/status            build state + whether the exe is running

Usage:
    python scripts/tuner_server.py          # serves on 127.0.0.1:8777, opens a browser
    python scripts/tuner_server.py --port N --no-open

SCOPE / SAFETY. This is a developer tool for one machine, not a service:
  - It binds 127.0.0.1 only, so nothing off this box can reach it.
  - The JSON writable paths are a fixed allowlist (materials/reactions/tuning
    under assets/materials, plus items/items.json). The page cannot name a
    path, so a bad or malicious request cannot write anywhere else. A per-item
    sidecar is NOT here: it is named by the request, so it goes through the
    model routes and their containment check. glyphs.json is served for
    the Wiki but is NOT in that allowlist, so /api/save cannot reach it however
    the request is spelled — read-only is structural, not a convention.
  - The model routes DO take a path from the request (the editor must be able
    to name the file it is editing), so every one goes through _model_path(),
    which resolves the path and requires the result to sit inside one of the
    fixed MODEL_DIRS with an allowed extension. Traversal, absolute paths,
    symlinks and odd extensions are all rejected there.
  - The note routes take a NAME, not a path, and _note_path() rejects anything
    that is not a plain filename of safe characters before joining it to
    notes/. Shaders are read-only and served from a fixed directory listing.
  - The sound routes take a set name + a bare filename, never a path. Both go
    through _sound_path(), which validates each path SEGMENT against the same
    character allowlist the notes use, so there is no traversal to defend
    against; containment under assets/sounds is then asserted anyway. Deletes
    move the file to assets/sounds/.trash/ rather than unlinking it — a
    recorded take is not reproducible, so the destructive path is the one place
    here that must not be trusted to a click.
  - /api/build runs one hardcoded command; /api/play picks its argv from a
    hardcoded whitelist keyed by the request's {mode, scene} (game, or the
    --lab fluid scenes). Both shell=False. No request TEXT ever reaches a
    command line — the body selects between fixed argument lists.
  - /api/heightmap is the one route that puts request NUMBERS on a command
    line, and they are integers parsed with int(float(...)) and clamped before
    they get there, so nothing but digits can reach argv. shell=False as well.
Anyone exposing this beyond localhost is opting into arbitrary local builds.
"""
import argparse
import gzip
import hashlib
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")
MATDIR = os.path.join(ASSETS, "materials")

# The only files the page may write, by logical name. Values are absolute paths
# resolved once here, so a request can never introduce a path of its own.
WRITABLE = {
    "materials": os.path.join(MATDIR, "materials.json"),
    "reactions": os.path.join(MATDIR, "reactions.json"),
    "tuning": os.path.join(MATDIR, "tuning.json"),
    # Item BEHAVIOUR (damage, reach, kind). An item's art and its grip live in
    # per-item sidecars under assets/items/, which ride the /api/model routes
    # instead — this is the one fixed file, so it belongs here.
    "items": os.path.join(ASSETS, "items", "items.json"),
}

def readable():
    """Read-only sources the Wiki assembles pages from, for the CURRENT ASSETS.

    Served by /api/files alongside the writable three, but deliberately NOT in
    WRITABLE: the Wiki EXPLAINS the magic system, it does not edit it. A glyph
    holds a RESOLVED 12-bit material id at runtime, so authoring one from a page
    that cannot re-run the engine's name resolution is how you get a spell that
    silently conjures air. Edit assets/spells/glyphs.json directly.

    A function rather than a module constant for the same reason model_dirs()
    is: tuner_app.py re-points ASSETS after import, and a dict frozen here would
    keep pointing at the pre-import path.
    """
    return {"glyphs": os.path.join(ASSETS, "spells", "glyphs.json")}

# Directories the editor may read and write models in, as paths RELATIVE to
# assets/. They are resolved against ASSETS at request time rather than frozen
# here, because tuner_app.py re-points the module's ASSETS global after import
# (it runs from a PyInstaller bundle whose __file__ is not the checkout).
#
# `items` is here for the same reason `mobs` is: an item is a .vox plus a .json
# sidecar the tuner edits in place (grip, edge, durability). It rides the two
# /api/model routes rather than /api/save, because those already carry the
# path-containment check a per-file asset needs — items.json itself, being one
# fixed file, goes through /api/save with the other three.
#
# `limbs` is the limb LIBRARY: one saved body part (a single limb, or a limb
# with its descendants) as a .vox of the geometry plus a .json of the rig
# metadata that makes it a limb rather than a lump — hp, joint, tag, anchor.
# Exactly the same .vox+.json shape as a mob, one rung down the hierarchy, so
# it rides these routes unchanged. See assets/editor/limblib.js for the format.
# `trees` is the algorithmic tree editor's directory: <species>.json is the
# authored parameter set (the source of truth) and <species>.svtree is the baked
# voxel atlas the engine loads. Same .json+per-file-binary shape as a mob, one
# rung further from geometry, so it rides these two routes unchanged.
#
# `prefabs` is here so the Trees tab can export a .vox of a generated tree for
# hand-placement with the in-game prefab tool. Read-write like the rest -- the
# containment check in _model_path() is what keeps that safe.
MODEL_DIRS = ("models", "mobs", "microvox", "items", "limbs", "trees", "prefabs")
MODEL_EXTS = (".vox", ".json", ".svtree")
# The one MODEL_DIRS entry with a delete route (/api/limb/delete). Named here
# so that route cannot be pointed at another directory by editing one string.
LIMB_DIR_NAME = "limbs"


def model_dirs():
    """Absolute, normalised model directories for the CURRENT ASSETS value."""
    return [os.path.abspath(os.path.join(ASSETS, d)) for d in MODEL_DIRS]


def _model_path(rel):
    """Resolve a request-supplied model path, or return None if it is not allowed.

    `rel` is relative to assets/, e.g. "models/goblin.vox". Everything about
    this function is the security boundary for the two /api/model routes:
    the path is resolved (so "..", "." and symlinks are collapsed) and the
    result must live directly inside one of MODEL_DIRS with an allowed
    extension. An absolute path in the request cannot escape either, because
    os.path.join() would adopt it and the containment check then fails.
    """
    if not isinstance(rel, str) or not rel:
        return None
    rel = rel.replace("\\", "/")
    if "\x00" in rel:
        return None
    # os.path.join would silently take over on an absolute or drive-qualified
    # path; reject those outright rather than relying on the check below.
    if rel.startswith("/") or os.path.isabs(rel) or (len(rel) > 1 and rel[1] == ":"):
        return None
    path = os.path.realpath(os.path.join(ASSETS, rel))
    if os.path.splitext(path)[1].lower() not in MODEL_EXTS:
        return None
    for d in model_dirs():
        root = os.path.realpath(d)
        if path.startswith(root + os.sep):
            return path
    return None


def shader_dir():
    """assets/shaders for the CURRENT ASSETS value (see model_dirs)."""
    return os.path.abspath(os.path.join(ASSETS, "shaders"))


def notes_dir():
    """Where note pages live: notes/ beside assets/, NOT inside it.

    Deliberately outside assets/ so notes are ordinary project documents —
    openable in Obsidian, diffable in git — rather than engine assets the
    loader might try to parse. Resolved per call for the same reason
    model_dirs() is: tuner_app.py re-points ROOT after import.
    """
    return os.path.abspath(os.path.join(ROOT, "notes"))


# A note is named, not pathed. Anything outside this alphabet cannot name a
# file, so there is no traversal to defend against in the first place.
_NOTE_OK = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"
               "0123456789-_. ()")
_WIN_RESERVED = {"CON", "PRN", "AUX", "NUL"} | {
    "%s%d" % (p, i) for p in ("COM", "LPT") for i in range(1, 10)}
NOTE_EXT = ".md"


def _note_path(name):
    """Resolve a note NAME to an absolute .md path, or None if not allowed.

    The name is a bare filename with no directory part: the page title as the
    user typed it. Rejecting on the character allowlist (plus the explicit
    dot-segment and length checks) happens BEFORE any join, so unlike the
    model routes this never depends on a containment test to be safe. The
    containment test is still applied afterwards as a second line of defence.
    """
    if not isinstance(name, str):
        return None
    name = name.strip()
    if not name or len(name) > 120:
        return None
    if name.lower().endswith(NOTE_EXT):
        name = name[:-len(NOTE_EXT)]
    if not name or name in (".", "..") or name != name.strip("."):
        return None
    if any(c not in _NOTE_OK for c in name):
        return None
    # Windows device names are not openable as files whatever the extension,
    # so "con.md" would fail at write time with a confusing error. Reject the
    # name up front instead.
    if name.split(".")[0].upper() in _WIN_RESERVED:
        return None
    root = notes_dir()
    path = os.path.abspath(os.path.join(root, name + NOTE_EXT))
    if os.path.dirname(path) != root:
        return None
    return path


# ---------------------------------------------------------------- sounds ----
# assets/sounds/ mirrors src/audio/library.h exactly: a SET is a FOLDER and its
# files are the variants. The server therefore never invents structure — it
# lists what the engine's own scanner would see, so a set that appears here is
# a set the game can play. Two directories are skipped for the same reasons the
# C++ loader skips them (raw/) or the loader must never see them (.trash/).
SOUND_EXTS = (".wav", ".flac", ".mp3", ".ogg")
SOUND_SKIP_DIRS = ("raw", ".trash")
TRASH_DIR = ".trash"


def sounds_dir():
    """assets/sounds for the CURRENT ASSETS value (see model_dirs)."""
    return os.path.abspath(os.path.join(ASSETS, "sounds"))


def _seg_ok(seg):
    """One path segment of a set name or filename, validated by allowlist.

    Reusing the note alphabet (minus space, which library.h rewrites to '_'
    and which would therefore make the set name on disk differ from the name
    the engine reports). Rejecting per SEGMENT is what makes the sound routes
    traversal-proof by construction: '..' fails the dot check, a separator
    cannot appear inside a segment at all, and no segment may be empty.
    """
    if not seg or len(seg) > 80:
        return False
    if seg in (".", "..") or seg != seg.strip("."):
        return False
    if seg.split(".")[0].upper() in _WIN_RESERVED:
        return False
    return all(c in _NOTE_OK and c != " " for c in seg)


def _sound_path(setname, filename=None, allow_trash=False):
    """Resolve set[+file] under assets/sounds/, or None if not allowed.

    `setname` is a '/'-joined set name as the engine spells it
    ("footsteps/leaf"); `filename` is a bare name with an audio extension.
    With no filename the result is the set DIRECTORY, which is what the
    create-set and move routes need.
    """
    if not isinstance(setname, str):
        return None
    segs = [s for s in setname.replace("\\", "/").split("/") if s != ""]
    if not segs or len(segs) > 6:
        return None
    if not all(_seg_ok(s) for s in segs):
        return None
    if not allow_trash and segs[0] in SOUND_SKIP_DIRS:
        return None
    root = sounds_dir()
    parts = [root] + segs
    if filename is not None:
        if not isinstance(filename, str) or not _seg_ok(filename):
            return None
        if os.path.splitext(filename)[1].lower() not in SOUND_EXTS:
            return None
        parts.append(filename)
    path = os.path.abspath(os.path.join(*parts))
    # Second line of defence, exactly as the model route does it.
    if not path.startswith(root + os.sep):
        return None
    return path


def _wav_info(path):
    """(seconds, channels, rate) for a WAV, or (None, None, None).

    Only WAV is inspected, and only via the stdlib: the tuner shows duration
    and flags stereo files because MONO is a hard requirement of the
    spatializer (library.h), and a stereo asset is a real authoring bug that
    is otherwise invisible until you hear the image collapse. Other formats
    report nothing rather than pulling in a decoder.
    """
    if not path.lower().endswith(".wav"):
        return (None, None, None)
    try:
        import wave
        with wave.open(path, "rb") as w:
            n, rate, ch = w.getnframes(), w.getframerate(), w.getnchannels()
            return (n / float(rate) if rate else None, ch, rate)
    except Exception:
        return (None, None, None)


def scan_sounds():
    """Every set under assets/sounds/, as the engine's scanner would see it.

    Set names are lowercased and space-to-underscore mapped to match
    SetNameFor() in src/audio/library.cpp — if the two ever disagree, the
    tuner would happily bind a material to a set name the engine cannot
    resolve.
    """
    root = sounds_dir()
    sets = {}
    if not os.path.isdir(root):
        return {"dir": root, "sets": [], "trash": 0}
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(d for d in dirnames if d not in SOUND_SKIP_DIRS)
        rel = os.path.relpath(dirpath, root).replace("\\", "/")
        if rel == ".":
            rel = ""
        files = [f for f in sorted(filenames)
                 if os.path.splitext(f)[1].lower() in SOUND_EXTS]
        if not files:
            continue
        for f in files:
            # A file directly in the root is a set named after the FILE; a file
            # in a subfolder makes that whole folder one set. Same two-case rule
            # as library.cpp.
            name = (rel if rel else os.path.splitext(f)[0])
            name = name.lower().replace(" ", "_")
            full = os.path.join(dirpath, f)
            secs, ch, rate = _wav_info(full)
            sets.setdefault(name, []).append({
                "file": f,
                "path": (rel + "/" + f) if rel else f,
                "size": os.path.getsize(full),
                "mtime": int(os.path.getmtime(full)),
                "seconds": secs, "channels": ch, "rate": rate,
            })
    trash = 0
    tdir = os.path.join(root, TRASH_DIR)
    if os.path.isdir(tdir):
        trash = sum(len(f) for _, _, f in os.walk(tdir))
    return {
        "dir": root, "trash": trash,
        "sets": [{"name": k, "variants": v} for k, v in sorted(sets.items())],
    }


def _next_variant_name(setdir, setname, ext):
    """A free `<leaf>_NN<ext>` in setdir.

    Imported files are RENAMED into the set's own numbering rather than
    keeping whatever the recorder called them, because the variant list is
    sorted by filename and the engine's deterministic variant order depends on
    that sort (library.cpp sorts before decoding). "take 3 FINAL.wav" landing
    between leaf_02 and leaf_03 would silently reorder every variant id.
    """
    leaf = setname.rstrip("/").split("/")[-1] or "sound"
    existing = set()
    try:
        existing = {n.lower() for n in os.listdir(setdir)}
    except OSError:
        pass
    for i in range(1, 1000):
        cand = "%s_%02d%s" % (leaf, i, ext)
        if cand.lower() not in existing:
            return cand
    return None


BUILD_CMD = ["cmake", "--build", "build", "--config", "Release",
             "--target", "sandvox"]
EXE = os.path.join(ROOT, "build", "Release", "sandvox.exe")

# Build state, shared with the poller. Guarded because ThreadingHTTPServer
# handles the build POST and the status GETs on different threads.
_lock = threading.Lock()
# Performance-suite state. Its own lock: a perf run is minutes long and holds
# the machine-global run mutex, so "is one already going" has to be answerable
# without waiting on the build lock.
_perf_lock = threading.Lock()
_perf = {"running": False, "ok": False, "log": "", "error": ""}
_build = {"running": False, "ok": None, "log": "", "returncode": None}
# Serialises /api/heightmap: a slider drag queues a dozen requests and there is
# no point running a dozen copies of the exporter.
_hm_lock = threading.Lock()
_play_procs = []


def _text(s):
    return s if isinstance(s, str) else s.decode("utf-8", "replace")


# ===========================================================================
# THE VOXEL SERVER — real voxels for the Worldgen tab's terrain view.
# ===========================================================================
#
# /api/heightmap runs a fresh process per map and that is fine, because
# --heightmap is a GPU-free early exit that costs ~150 ms. --voxserve is the
# opposite: it needs a Vulkan device, the SPIR-V compile and the material table
# (genCell is WGSL), which is ~3 s of boot for ~8 ms of work. A viewer that
# streams regions as the camera moves would spend its entire life booting.
#
# So ONE process, kept alive, driven over stdin. See src/tools/voxregion.h for
# the protocol and why the payload goes to a file rather than down the pipe.
VOXCACHE = os.path.join(ROOT, "build", "voxcache")
VOXTMP = os.path.join(ROOT, "build", "voxtmp")
# Bound on the on-disk cache. A region is ~100 KB gzipped, so this is ~200 MB
# worst case and typically far less.
VOXCACHE_MAX = 2000

WORLDEDIT_DIR = os.path.join(ASSETS, "worldedits")
WORLDEDIT_EXT = ".svedit"

# ---- the machine-global run mutex, in Python -------------------------------
#
# CLAUDE.md: every sandvox.exe launch goes through scripts/run.sh, which is a
# `mkdir C:/sv-build-lock` mutex shared with build.sh — concurrent GPU
# processes throttle the machine and poison every measured number.
#
# A persistent server cannot hold that lock for its whole life: it would block
# every build in every worktree for the length of a tuner session. But it also
# must not generate a region WHILE another agent is measuring. So the lock is
# taken around the ~10 ms of work and released, which is the same exclusion
# run.sh provides with none of the starvation. The boot is covered too — that
# is the expensive part.
#
# Mirrors run.sh exactly, including the 10-minute stale sweep, because two
# implementations of one mutex that disagree about staleness is not a mutex.
RUNLOCK_DIR = "C:/sv-build-lock"
RUNLOCK_STALE_SEC = 600


class RunLock:
    """Context manager over the same directory mutex scripts/run.sh uses."""

    def __init__(self, who, timeout=180.0):
        self.who = who
        self.timeout = timeout

    def __enter__(self):
        deadline = time.time() + self.timeout
        while True:
            try:
                ts = 0
                try:
                    with open(os.path.join(RUNLOCK_DIR, "ts")) as f:
                        ts = int(f.read().strip() or 0)
                except OSError:
                    ts = None
                if ts is not None and ts and time.time() - ts > RUNLOCK_STALE_SEC:
                    shutil.rmtree(RUNLOCK_DIR, ignore_errors=True)
            except OSError:
                pass
            try:
                os.mkdir(RUNLOCK_DIR)
            except FileExistsError:
                if time.time() > deadline:
                    raise TimeoutError("build/run lock held for >%ds" % self.timeout)
                time.sleep(0.05)
                continue
            except OSError as e:
                raise
            for name, val in (("pid", str(os.getpid())),
                              ("who", "tuner:" + self.who),
                              ("ts", str(int(time.time())))):
                try:
                    with open(os.path.join(RUNLOCK_DIR, name), "w") as f:
                        f.write(val)
                except OSError:
                    pass
            return self

    def __exit__(self, *exc):
        shutil.rmtree(RUNLOCK_DIR, ignore_errors=True)
        return False


class VoxServe:
    """The one --voxserve subprocess, and the request lock in front of it.

    Single-threaded by construction: the protocol is one line in, one ack line
    out, so two concurrent requests would interleave and each would read the
    other's ack. ThreadingHTTPServer means that is not hypothetical — a viewer
    streaming a ring of regions issues them in parallel.
    """

    def __init__(self):
        self.proc = None
        self.lock = threading.Lock()
        self.err = None

    def _spawn(self):
        if not os.path.isfile(EXE):
            raise RuntimeError("build/Release/sandvox.exe not built yet — press Build")
        os.makedirs(VOXTMP, exist_ok=True)
        # The boot is the expensive, GPU-heavy part, so it holds the run mutex.
        with RunLock("voxserve-boot"):
            p = subprocess.Popen(
                [EXE, "--voxserve"], cwd=ROOT,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL, text=True, bufsize=1,
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
            # Skip the boot log; the protocol starts at the READY line.
            while True:
                line = p.stdout.readline()
                if not line:
                    p.kill()
                    raise RuntimeError("voxserve died during boot")
                if line.startswith("VOXSERVE READY"):
                    break
        self.proc = p

    def _ensure(self):
        if self.proc is not None and self.proc.poll() is None:
            return
        self.proc = None
        self._spawn()

    def command(self, line, hold_lock=True):
        """Send one command, return (ok, payload_bytes_or_message).

        `hold_lock=False` is for commands that touch no GPU (PING), so a
        liveness check does not queue behind somebody's build.
        """
        with self.lock:
            self._ensure()
            def _round_trip():
                self.proc.stdin.write(line + "\n")
                self.proc.stdin.flush()
                while True:
                    ack = self.proc.stdout.readline()
                    if not ack:
                        raise RuntimeError("voxserve died")
                    ack = ack.strip()
                    if ack.startswith("OK ") or ack.startswith("ERR "):
                        return ack
                    # Anything else is engine chatter; the protocol tolerates it
                    # rather than breaking, because a stray printf in a shader
                    # reload path should not take the viewer down.
            try:
                if hold_lock:
                    with RunLock("voxserve"):
                        ack = _round_trip()
                else:
                    ack = _round_trip()
            except Exception as e:
                # A dead server is recoverable: the next request respawns it.
                try:
                    if self.proc:
                        self.proc.kill()
                except Exception:
                    pass
                self.proc = None
                return False, str(e)
            if ack.startswith("ERR "):
                return False, ack[4:]
            return True, ack[3:]

    def stop(self):
        with self.lock:
            p, self.proc = self.proc, None
        if p and p.poll() is None:
            try:
                p.stdin.write("QUIT\n")
                p.stdin.flush()
                p.wait(timeout=3)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass


_voxserve = VoxServe()


def _world_signature():
    """What the generated world depends on, as a short hex string.

    The cache key has to move when the world does, and the world is a function
    of tuning.json (worldgen's octave ladder, pond/tree/cave parameters — most
    of them WGSL consts) and of materials.json (the ids the words carry). Hashed
    by CONTENT, not mtime: a tuner restart should not throw away a cache, and an
    edit that reverts a file should hit the entries it had before.
    """
    h = hashlib.sha1()
    for path in (WRITABLE["tuning"], WRITABLE["materials"]):
        try:
            with open(path, "rb") as f:
                h.update(f.read())
        except OSError:
            h.update(b"?")
    return h.hexdigest()[:16]


def _voxcache_sweep():
    """Trim the cache to VOXCACHE_MAX files, oldest-accessed first."""
    try:
        names = os.listdir(VOXCACHE)
    except OSError:
        return
    if len(names) <= VOXCACHE_MAX:
        return
    entries = []
    for n in names:
        p = os.path.join(VOXCACHE, n)
        try:
            entries.append((os.path.getmtime(p), p))
        except OSError:
            pass
    entries.sort()
    for _, p in entries[:len(entries) - VOXCACHE_MAX]:
        try:
            os.remove(p)
        except OSError:
            pass


def _worldedit_path(name):
    """assets/worldedits/<name>.svedit, or None if the name is not a bare name.

    Same discipline as _note_path: the request carries a NAME, never a path, and
    anything that is not a plain safe filename is refused before it is joined.
    """
    if not name or len(name) > 64:
        return None
    ok = set("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_ ")
    if any(c not in ok for c in name):
        return None
    return os.path.join(WORLDEDIT_DIR, name + WORLDEDIT_EXT)


def run_build():
    """Run the build, capturing output. Errors are reported, never raised."""
    global _build
    with _lock:
        _build = {"running": True, "ok": None, "log": "", "returncode": None}
    try:
        p = subprocess.run(BUILD_CMD, cwd=ROOT, capture_output=True,
                           shell=False, timeout=1800,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        out = _text(p.stdout) + _text(p.stderr)
        # The Dawn build log is enormous; keep the lines that name our own
        # sources plus anything that looks like a diagnostic, so the panel is
        # readable instead of 10k lines of third-party noise.
        keep = []
        for line in out.splitlines():
            low = line.lower()
            if ("error" in low or "warning c" in low
                    or "\\src\\" in low or "/src/" in low
                    or ".vcxproj ->" in low):
                if "_deps" in low or "third_party" in low:
                    continue
                keep.append(line.rstrip())
        tail = keep[-200:] if keep else out.splitlines()[-40:]
        with _lock:
            _build = {"running": False, "ok": p.returncode == 0,
                      "log": "\n".join(tail), "returncode": p.returncode}
    except FileNotFoundError:
        with _lock:
            _build = {"running": False, "ok": False, "returncode": -1,
                      "log": "cmake not found on PATH. Open a shell that has it "
                             "(or the VS developer prompt) and try there."}
    except subprocess.TimeoutExpired:
        with _lock:
            _build = {"running": False, "ok": False, "returncode": -1,
                      "log": "build timed out after 30 minutes"}
    except Exception as e:  # never let a build failure kill the server
        with _lock:
            _build = {"running": False, "ok": False, "returncode": -1,
                      "log": "build failed to start: %r" % (e,)}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):  # quiet; the console is for build output
        pass

    # ---- helpers ----
    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        # The tuner edits files; a cached copy is worse than a slow one.
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json.dumps(obj))

    def _query(self):
        q = self.path.split("?", 1)
        return urllib.parse.parse_qs(q[1]) if len(q) > 1 else {}

    def _raw(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n > 0 else b""

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception:
            return {}

    def _serve_asset(self, rel):
        # Static files come from assets/ only, and the resolved path must stay
        # inside it — otherwise a crafted URL could walk up into the repo.
        path = os.path.abspath(os.path.join(ASSETS, rel))
        if not path.startswith(ASSETS + os.sep) and path != ASSETS:
            return self._send(403, "forbidden", "text/plain")
        if not os.path.isfile(path):
            return self._send(404, "not found", "text/plain")
        ext = os.path.splitext(path)[1].lower()
        ctype = {".html": "text/html; charset=utf-8",
                 ".js": "text/javascript; charset=utf-8",
                 ".json": "application/json",
                 ".css": "text/css",
                 ".vox": "model/x-vox",
                 ".png": "image/png"}.get(ext, "application/octet-stream")
        with open(path, "rb") as f:
            self._send(200, f.read(), ctype)

    # ---- routes ----
    # ---- the terrain map ---------------------------------------------------
    #
    # Runs `sandvox --heightmap` and hands the bytes straight back. That mode is
    # a GPU-FREE EARLY EXIT — it answers before GpuContext exists, reads only
    # tuning.json, and takes ~150 ms for a 384x384 map — which is why this route
    # does NOT go through scripts/run.sh. The run mutex exists because
    # concurrent processes saturate the GPU and poison every measured number
    # (CLAUDE.md); a mode that never opens a device cannot do either, and taking
    # the machine-global build lock on every slider drag would serialise the
    # tuner against builds and make the map hang for minutes.
    #
    # ONE AT A TIME anyway, through the same lock the build uses for its state:
    # a drag can queue a dozen requests and there is no point running twelve
    # copies. The client debounces as well; this is the backstop.
    def _heightmap(self):
        q = self._query()
        def num(name, dflt):
            try:
                return int(float(q.get(name, [dflt])[0]))
            except (TypeError, ValueError):
                return dflt
        cx, cz = num("cx", 0), num("cz", 0)
        span, res = num("span", 8192), num("res", 384)
        seed = num("seed", 1337)
        # Clamped here as well as in the engine: this route is reachable from a
        # page, and the engine's clamp is the second line of defence, not the
        # first.
        res = max(8, min(res, 1024))
        span = max(res, min(span, 1 << 22))
        out = os.path.join(ROOT, "build", "tuner_heightmap.bin")
        cmd = [EXE, "--heightmap", "%d,%d,%d,%d,%d" % (cx, cz, span, res, seed),
               "--heightmap-out", out]
        if not os.path.isfile(EXE):
            return self._json(503, {"ok": False,
                                    "error": "build/Release/sandvox.exe not built yet "
                                             "— press Build"})
        with _hm_lock:
            try:
                r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True,
                                   timeout=120,
                                   creationflags=getattr(subprocess,
                                                         "CREATE_NO_WINDOW", 0))
            except Exception as e:
                return self._json(500, {"ok": False, "error": str(e)})
            if r.returncode != 0:
                return self._json(500, {"ok": False,
                                        "error": (r.stderr or r.stdout or "")[-2000:]})
            try:
                with open(out, "rb") as f:
                    blob = f.read()
            except OSError as e:
                return self._json(500, {"ok": False, "error": str(e)})
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(blob)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(blob)

    # ---- the voxel view ----------------------------------------------------
    def _send_blob(self, blob, gz=False):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        if gz:
            self.send_header("Content-Encoding", "gzip")
        self.send_header("Content-Length", str(len(blob)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(blob)

    def _voxregion(self):
        q = self._query()

        def num(name, dflt):
            try:
                return int(float(q.get(name, [dflt])[0]))
            except (TypeError, ValueError):
                return dflt

        ox, oy, oz = num("ox", 0), num("oy", 1008), num("oz", 0)
        nx, ny, nz = num("nx", 64), num("ny", 64), num("nz", 64)
        lod, seed = num("lod", 1), num("seed", 1337)
        # Refused rather than clamped, for the reason BuildVoxRegion refuses:
        # a viewer that silently got a different box would place the mesh at the
        # origin it asked for and the seam would look like a worldgen bug.
        if lod not in (1, 2, 4, 8, 16):
            return self._json(400, {"ok": False, "error": "lod must be 1/2/4/8/16"})
        for v in (nx, ny, nz):
            if v < 1 or v > 128:
                return self._json(400, {"ok": False,
                                        "error": "sample counts must be 1..128"})
        for v in (ox, oy, oz):
            if v % 16:
                return self._json(400, {"ok": False,
                                        "error": "box origin must be a multiple of 16"})

        key = hashlib.sha1(("%d,%d,%d,%d,%d,%d,%d,%d,%s" % (
            ox, oy, oz, nx, ny, nz, lod, seed, _world_signature())
        ).encode()).hexdigest()
        cached = os.path.join(VOXCACHE, key + ".gz")
        want_gz = "gzip" in (self.headers.get("Accept-Encoding") or "")
        if os.path.isfile(cached):
            try:
                with open(cached, "rb") as f:
                    blob = f.read()
                os.utime(cached, None)          # LRU: the sweep sorts on mtime
                if want_gz:
                    return self._send_blob(blob, gz=True)
                return self._send_blob(gzip.decompress(blob))
            except OSError:
                pass

        os.makedirs(VOXTMP, exist_ok=True)
        os.makedirs(VOXCACHE, exist_ok=True)
        raw = os.path.join(VOXTMP, key + ".bin")
        cmd = "REGION %d %d %d %d %d %d %d %d %s" % (
            ox, oy, oz, nx, ny, nz, lod, seed, raw.replace("\\", "/"))
        try:
            ok, msg = _voxserve.command(cmd)
        except Exception as e:
            return self._json(503, {"ok": False, "error": str(e)})
        if not ok:
            return self._json(500, {"ok": False, "error": msg})
        try:
            with open(raw, "rb") as f:
                blob = f.read()
            os.remove(raw)
        except OSError as e:
            return self._json(500, {"ok": False, "error": str(e)})
        # Gzip is worth it here and not a reflex: a 64^3 region is ~1.1 MB of
        # words and ~90 KB gzipped, because the state nibble's palette jitter
        # makes the run-length coding in the file itself nearly useless on
        # solid rock. Cached in the compressed form, since that is what almost
        # every response sends.
        packed = gzip.compress(blob, 6)
        try:
            with open(cached, "wb") as f:
                f.write(packed)
            _voxcache_sweep()
        except OSError:
            pass
        if want_gz:
            return self._send_blob(packed, gz=True)
        return self._send_blob(blob)

    # ---- worldgen defaults -------------------------------------------------
    #
    # Runs `sandvox --dump-tuning-defaults`, which is a GPU-free, asset-free,
    # tuning.json-free early exit — so like /api/heightmap it does NOT take the
    # run mutex. There is no device to contend for.
    #
    # Cached on the EXE's mtime: the defaults are compiled in, so they can only
    # change when the binary does, and the tuner asks for them every time the
    # Worldgen tab renders.
    _defaults_cache = {"mtime": None, "body": None}

    def _tuning_defaults(self):
        if not os.path.isfile(EXE):
            return self._json(503, {"ok": False,
                                    "error": "build/Release/sandvox.exe not built yet "
                                             "— press Build"})
        mtime = os.path.getmtime(EXE)
        c = Handler._defaults_cache
        if c["mtime"] == mtime and c["body"]:
            return self._send(200, c["body"], "application/json")
        out = os.path.join(ROOT, "build", "tuning_defaults.json")
        try:
            r = subprocess.run([EXE, "--dump-tuning-defaults", out],
                               cwd=ROOT, capture_output=True, text=True, timeout=60,
                               creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        except Exception as e:
            return self._json(500, {"ok": False, "error": str(e)})
        if r.returncode != 0:
            return self._json(500, {"ok": False,
                                    "error": (r.stderr or r.stdout or "")[-2000:]})
        try:
            with open(out, "rb") as f:
                body = f.read()
        except OSError as e:
            return self._json(500, {"ok": False, "error": str(e)})
        c["mtime"], c["body"] = mtime, body
        return self._send(200, body, "application/json")

    def _voxpalette(self):
        out = os.path.join(VOXTMP, "palette.json")
        os.makedirs(VOXTMP, exist_ok=True)
        # PALETTE touches no GPU, so it does not queue behind a build.
        ok, msg = _voxserve.command("PALETTE " + out.replace("\\", "/"),
                                    hold_lock=False)
        if not ok:
            return self._json(503, {"ok": False, "error": msg})
        try:
            with open(out, "rb") as f:
                return self._send(200, f.read(), "application/json")
        except OSError as e:
            return self._json(500, {"ok": False, "error": str(e)})

    def do_GET(self):
        p = self.path.split("?")[0]
        if p == "/":
            return self._serve_asset("tuner.html")
        if p == "/favicon.ico":
            # Browsers request this unprompted; answering keeps a spurious 404
            # out of the page's console.
            return self._send(204, b"", "image/x-icon")
        if p == "/api/files":
            out = {}
            # The writable three, then the read-only Wiki sources. Same shape on
            # the wire; the page decides what it may write back.
            for name, path in list(WRITABLE.items()) + list(readable().items()):
                try:
                    with open(path, encoding="utf-8") as f:
                        out[name] = f.read()
                except FileNotFoundError:
                    out[name] = None
            out["root"] = ROOT
            return self._json(200, out)
        if p == "/api/models":
            # assets/models/ is the editor's own output directory; create it on
            # first listing so a fresh checkout has somewhere to save to.
            try:
                os.makedirs(os.path.join(ASSETS, MODEL_DIRS[0]), exist_ok=True)
            except OSError:
                pass
            files = []
            for d, absdir in zip(MODEL_DIRS, model_dirs()):
                try:
                    names = sorted(os.listdir(absdir))
                except OSError:
                    continue
                for n in names:
                    if os.path.splitext(n)[1].lower() not in MODEL_EXTS:
                        continue
                    full = os.path.join(absdir, n)
                    if not os.path.isfile(full):
                        continue
                    files.append({"path": "%s/%s" % (d, n), "dir": d, "name": n,
                                  "size": os.path.getsize(full),
                                  "mtime": int(os.path.getmtime(full))})
            return self._json(200, {"ok": True, "dirs": list(MODEL_DIRS),
                                    "files": files})

        if p == "/api/model":
            rel = self._query().get("path", [""])[0]
            path = _model_path(rel)
            if not path:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            if not os.path.isfile(path):
                return self._json(404, {"ok": False, "error": "not found"})
            ctype = ("application/json" if path.lower().endswith(".json")
                     else "model/x-vox")
            with open(path, "rb") as f:
                return self._send(200, f.read(), ctype)

        if p == "/api/shaders":
            # Read-only: the wiki quotes shader source as context, it does not
            # edit it. Sent as one payload because the pages cross-reference
            # every file and the whole set is well under a megabyte.
            out = {}
            d = shader_dir()
            try:
                names = sorted(n for n in os.listdir(d)
                               if n.lower().endswith(".wgsl"))
            except OSError:
                names = []
            for n in names:
                try:
                    with open(os.path.join(d, n), encoding="utf-8") as f:
                        out[n] = f.read()
                except OSError:
                    continue
            return self._json(200, {"ok": True, "shaders": out})

        if p == "/api/sounds":
            return self._json(200, dict(scan_sounds(), ok=True))

        if p == "/api/sound":
            q = self._query()
            path = _sound_path(q.get("set", [""])[0], q.get("file", [""])[0])
            if not path:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            if not os.path.isfile(path):
                return self._json(404, {"ok": False, "error": "not found"})
            ctype = {".wav": "audio/wav", ".mp3": "audio/mpeg",
                     ".ogg": "audio/ogg", ".flac": "audio/flac"}.get(
                         os.path.splitext(path)[1].lower(),
                         "application/octet-stream")
            with open(path, "rb") as f:
                return self._send(200, f.read(), ctype)

        if p == "/api/notes":
            d = notes_dir()
            try:
                os.makedirs(d, exist_ok=True)
            except OSError:
                pass
            pages = []
            try:
                names = sorted(os.listdir(d))
            except OSError:
                names = []
            for n in names:
                if not n.lower().endswith(NOTE_EXT):
                    continue
                full = os.path.join(d, n)
                if not os.path.isfile(full):
                    continue
                pages.append({"name": n[:-len(NOTE_EXT)],
                              "size": os.path.getsize(full),
                              "mtime": int(os.path.getmtime(full))})
            return self._json(200, {"ok": True, "dir": d, "pages": pages})

        if p == "/api/note":
            name = self._query().get("name", [""])[0]
            path = _note_path(name)
            if not path:
                return self._json(400, {"ok": False, "error": "bad note name"})
            if not os.path.isfile(path):
                return self._json(404, {"ok": False, "error": "not found"})
            with open(path, encoding="utf-8") as f:
                return self._json(200, {"ok": True, "name": name,
                                        "text": f.read()})

        if p == "/api/heightmap":
            return self._heightmap()
        if p == "/api/voxregion":
            return self._voxregion()
        if p == "/api/voxpalette":
            return self._voxpalette()
        if p == "/api/tuning-defaults":
            return self._tuning_defaults()
        if p == "/api/worldedits":
            names = []
            try:
                for n in sorted(os.listdir(WORLDEDIT_DIR)):
                    if n.endswith(WORLDEDIT_EXT):
                        full = os.path.join(WORLDEDIT_DIR, n)
                        names.append({"name": n[:-len(WORLDEDIT_EXT)],
                                      "bytes": os.path.getsize(full),
                                      "mtime": int(os.path.getmtime(full))})
            except OSError:
                pass
            return self._json(200, {"ok": True, "layers": names})
        if p == "/api/worldedit":
            name = (self._query().get("name") or [""])[0]
            path = _worldedit_path(name)
            if not path:
                return self._json(400, {"ok": False, "error": "bad layer name"})
            if not os.path.isfile(path):
                return self._json(404, {"ok": False, "error": "no such layer"})
            with open(path, "rb") as f:
                return self._send(200, f.read(), "application/octet-stream")

        if p == "/perf.json":
            # The Performance tab reads build/perf.json, which lives OUTSIDE
            # assets/ (it is build output, not an authored asset, and it must
            # not be committed). Served explicitly rather than by widening the
            # static root, which would expose the whole build directory.
            path = os.path.join(ROOT, "build", "perf.json")
            if not os.path.isfile(path):
                return self._json(404, {"ok": False,
                                        "error": "no build/perf.json yet -- "
                                                 "run the performance suite"})
            with open(path, "rb") as f:
                return self._send(200, f.read(), "application/json")

        if p == "/api/perf/status":
            with _perf_lock:
                return self._json(200, dict(_perf))

        if p == "/api/status":
            with _lock:
                b = dict(_build)
            alive = [q for q in _play_procs if q.poll() is None]
            _play_procs[:] = alive
            b["exeExists"] = os.path.isfile(EXE)
            b["playing"] = len(alive)
            return self._json(200, b)
        return self._serve_asset(p.lstrip("/"))

    def do_POST(self):
        p = self.path.split("?")[0]
        if p == "/api/save":
            body = self._body()
            written = []
            for name, text in (body.get("files") or {}).items():
                path = WRITABLE.get(name)      # allowlist: unknown names ignored
                if not path or not isinstance(text, str):
                    continue
                try:
                    json.loads(text)           # never write a file that won't parse
                except Exception as e:
                    return self._json(400, {"ok": False,
                                            "error": "%s is not valid JSON: %s" % (name, e)})
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
                written.append(name)
            return self._json(200, {"ok": True, "written": written})

        if p == "/api/model":
            rel = self._query().get("path", [""])[0]
            path = _model_path(rel)
            if not path:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            data = self._raw()
            if not data:
                return self._json(400, {"ok": False, "error": "empty body"})
            is_json = path.lower().endswith(".json")
            if is_json:
                # Same rule as /api/save: never write a .json that will not
                # parse, because the engine would then fail to load the asset.
                try:
                    json.loads(data.decode("utf-8"))
                except Exception as e:
                    return self._json(400, {"ok": False,
                                            "error": "not valid JSON: %s" % e})
            elif path.lower().endswith(".svtree"):
                # Same rule as the .vox magic check below and for the same
                # reason: the engine ABORTS on a malformed atlas, so a
                # half-written or wrong-typed file here is a game that will not
                # start. src/sim/treeatlas.h kFileMagic.
                if data[:4] != b"SVTR":
                    return self._json(400, {"ok": False,
                                            "error": "not a .svtree file (bad magic)"})
            else:
                if data[:4] != b"VOX ":
                    return self._json(400, {"ok": False,
                                            "error": "not a .vox file (bad magic)"})
            try:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "wb") as f:
                    f.write(data)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "path": rel,
                                    "bytes": len(data)})

        if p == "/api/limb/delete":
            # Deleting a LIBRARY part only — the route refuses anything outside
            # assets/limbs/, so a mob or a prefab can never be removed through
            # it however the request is spelled. A saved limb is authored art,
            # so like a sound take it is MOVED to .trash/ rather than unlinked;
            # /api/models skips that folder because it lists files, not dirs.
            name = self._body().get("name")
            if not isinstance(name, str) or not _seg_ok(name):
                return self._json(400, {"ok": False, "error": "bad limb name"})
            moved = []
            try:
                trash = os.path.abspath(
                    os.path.join(ASSETS, LIMB_DIR_NAME, TRASH_DIR))
                for ext in (".vox", ".json"):
                    src = _model_path("%s/%s%s" % (LIMB_DIR_NAME, name, ext))
                    if not src or not os.path.isfile(src):
                        continue
                    os.makedirs(trash, exist_ok=True)
                    dst = os.path.join(trash, name + ext)
                    stem, e2 = os.path.splitext(dst)
                    n = 1
                    while os.path.exists(dst):
                        dst = "%s (%d)%s" % (stem, n, e2)
                        n += 1
                    os.replace(src, dst)
                    moved.append(os.path.basename(src))
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            if not moved:
                return self._json(404, {"ok": False, "error": "not found"})
            return self._json(200, {"ok": True, "trashed": moved})

        if p == "/api/note":
            name = self._query().get("name", [""])[0]
            path = _note_path(name)
            if not path:
                return self._json(400, {"ok": False, "error": "bad note name"})
            body = self._body()
            text = body.get("text")
            if not isinstance(text, str):
                return self._json(400, {"ok": False, "error": "no text"})
            # An old name means this save is also a rename: write the new file
            # first, then drop the old one, so a failure loses nothing.
            old = _note_path(body.get("rename")) if body.get("rename") else None
            try:
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with open(path, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
                if old and old != path and os.path.isfile(old):
                    os.remove(old)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "name": name,
                                    "bytes": len(text.encode("utf-8")),
                                    "mtime": int(os.path.getmtime(path))})

        # ---- sounds ----
        # The whole point of this group is that dropping a .wav on a slot in
        # the page is the ENTIRE authoring step: the file lands in the right
        # folder, under the right name, in a set the engine already knows how
        # to scan. Every route answers with the rescanned tree so the page
        # never has to guess what the folder now looks like.
        if p == "/api/sound/import":
            q = self._query()
            setname = q.get("set", [""])[0]
            setdir = _sound_path(setname)
            if not setdir:
                return self._json(400, {"ok": False, "error": "bad set name"})
            src = q.get("name", [""])[0]
            ext = os.path.splitext(src)[1].lower()
            if ext not in SOUND_EXTS:
                return self._json(400, {"ok": False,
                                        "error": "not an audio file: %r" % (src,)})
            data = self._raw()
            if not data:
                return self._json(400, {"ok": False, "error": "empty body"})
            if len(data) > 64 * 1024 * 1024:
                return self._json(400, {"ok": False, "error": "file too large"})
            if ext == ".wav" and data[:4] != b"RIFF":
                return self._json(400, {"ok": False,
                                        "error": "not a WAV file (bad magic)"})
            try:
                os.makedirs(setdir, exist_ok=True)
                name = _next_variant_name(setdir, setname, ext)
                if not name:
                    return self._json(400, {"ok": False, "error": "set is full"})
                with open(os.path.join(setdir, name), "wb") as f:
                    f.write(data)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            secs, ch, rate = _wav_info(os.path.join(setdir, name))
            return self._json(200, {"ok": True, "set": setname, "file": name,
                                    "bytes": len(data), "channels": ch,
                                    "seconds": secs, "rate": rate,
                                    "tree": scan_sounds()})

        if p == "/api/sound/rename":
            b = self._body()
            old = _sound_path(b.get("set"), b.get("file"))
            new = _sound_path(b.get("set"), b.get("to"))
            if not old or not new:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            if not os.path.isfile(old):
                return self._json(404, {"ok": False, "error": "not found"})
            if os.path.exists(new) and os.path.abspath(new) != os.path.abspath(old):
                return self._json(409, {"ok": False, "error": "name already taken"})
            try:
                os.replace(old, new)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "tree": scan_sounds()})

        if p == "/api/sound/move":
            b = self._body()
            src = _sound_path(b.get("set"), b.get("file"))
            dstdir = _sound_path(b.get("to"))
            if not src or not dstdir:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            if not os.path.isfile(src):
                return self._json(404, {"ok": False, "error": "not found"})
            try:
                os.makedirs(dstdir, exist_ok=True)
                ext = os.path.splitext(src)[1].lower()
                name = _next_variant_name(dstdir, b.get("to"), ext)
                if not name:
                    return self._json(400, {"ok": False, "error": "set is full"})
                os.replace(src, os.path.join(dstdir, name))
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "file": name,
                                    "tree": scan_sounds()})

        if p == "/api/sound/delete":
            b = self._body()
            src = _sound_path(b.get("set"), b.get("file"))
            if not src:
                return self._json(400, {"ok": False, "error": "path not allowed"})
            if not os.path.isfile(src):
                return self._json(404, {"ok": False, "error": "not found"})
            # MOVED, not unlinked. A take is a recording someone made once;
            # the loader already skips .trash/ by name, so a "deleted" variant
            # is inaudible to the engine and still on disk if it was a mistake.
            try:
                rel = os.path.relpath(src, sounds_dir()).replace("\\", "/")
                dst = os.path.join(sounds_dir(), TRASH_DIR,
                                   rel.replace("/", "__"))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                stem, ext = os.path.splitext(dst)
                n = 1
                while os.path.exists(dst):
                    dst = "%s (%d)%s" % (stem, n, ext)
                    n += 1
                os.replace(src, dst)
                # Drop the set folder if that was its last variant, so an
                # emptied set stops showing up as a set with no sounds.
                d = os.path.dirname(src)
                if d != sounds_dir() and not os.listdir(d):
                    os.rmdir(d)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "trashed": True,
                                    "tree": scan_sounds()})

        if p == "/api/sound/set":
            b = self._body()
            d = _sound_path(b.get("set"))
            if not d:
                return self._json(400, {"ok": False, "error": "bad set name"})
            try:
                os.makedirs(d, exist_ok=True)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True, "tree": scan_sounds()})

        if p == "/api/note/delete":
            path = _note_path(self._body().get("name"))
            if not path:
                return self._json(400, {"ok": False, "error": "bad note name"})
            try:
                if os.path.isfile(path):
                    os.remove(path)
            except OSError as e:
                return self._json(500, {"ok": False, "error": repr(e)})
            return self._json(200, {"ok": True})

        # ---- the voxel view ----
        if p == "/api/voxreload":
            # Worldgen's tuning values are WGSL consts baked at pipeline build,
            # so a live server keeps answering with the parameters it booted on
            # until this runs. The tab sends it after every save — the same
            # save-then-render order the 2D map has always used, for the same
            # reason.
            ok, msg = _voxserve.command("RELOAD")
            if not ok:
                return self._json(503, {"ok": False, "error": msg})
            return self._json(200, {"ok": True, "signature": _world_signature()})

        if p == "/api/worldedit":
            name = (self._query().get("name") or [""])[0]
            path = _worldedit_path(name)
            if not path:
                return self._json(400, {"ok": False, "error": "bad layer name"})
            blob = self._raw()
            if len(blob) < 32 or blob[:4] != b"SVED":
                return self._json(400, {"ok": False, "error": "not an SVED layer"})
            try:
                os.makedirs(WORLDEDIT_DIR, exist_ok=True)
                # Write-then-rename: a half-written edit layer is a world the
                # engine will refuse to load, and the tuner autosaves.
                tmp = path + ".tmp"
                with open(tmp, "wb") as f:
                    f.write(blob)
                os.replace(tmp, path)
            except OSError as e:
                return self._json(500, {"ok": False, "error": str(e)})
            return self._json(200, {"ok": True, "bytes": len(blob)})

        if p == "/api/worldedit/delete":
            name = (self._body().get("name") or "")
            path = _worldedit_path(name)
            if not path:
                return self._json(400, {"ok": False, "error": "bad layer name"})
            try:
                if os.path.isfile(path):
                    os.remove(path)
            except OSError as e:
                return self._json(500, {"ok": False, "error": str(e)})
            return self._json(200, {"ok": True})

        if p == "/api/build":
            with _lock:
                if _build["running"]:
                    return self._json(409, {"ok": False, "error": "build already running"})
            # The voxel server holds an open handle on sandvox.exe, and a link
            # step against a running image is the LNK1104 build.sh kills stale
            # instances to prevent. It respawns on the next region request, so
            # this costs a boot and buys a build that works.
            _voxserve.stop()
            threading.Thread(target=run_build, daemon=True).start()
            return self._json(200, {"ok": True, "started": True})

        if p == "/api/play":
            if not os.path.isfile(EXE):
                return self._json(400, {"ok": False,
                                        "error": "sandvox.exe not found — build first"})
            # Launch mode from the tuner's run dropdown. WHITELISTED, never
            # spliced from client text: the browser picks a mode, this table
            # owns the argv. "lab" is the fluid testing world
            # (docs/PLAN_fluid_overhaul.md §4, `--lab <scene>`).
            body = self._body()
            mode = body.get("mode", "game")
            scene = body.get("scene", "basin")
            if mode == "game":
                argv = [EXE, "--telemetry"]
            elif mode == "lab":
                if scene not in ("basin", "hill", "faucet", "pool", "slosh"):
                    return self._json(400, {"ok": False,
                                            "error": "unknown lab scene %r" % (scene,)})
                argv = [EXE, "--lab", scene, "--telemetry"]
            else:
                return self._json(400, {"ok": False,
                                        "error": "unknown mode %r" % (mode,)})
            try:
                q = subprocess.Popen(argv, cwd=ROOT, shell=False)
                _play_procs.append(q)
                return self._json(200, {"ok": True, "pid": q.pid})
            except Exception as e:
                return self._json(500, {"ok": False, "error": repr(e)})

        if p == "/api/perf/run":
            # `sandvox --perf`, THROUGH scripts/run.sh so it takes the machine-
            # global run mutex. That is not ceremony: concurrent sandvox
            # processes saturate the GPU and every number the suite records
            # becomes garbage (CLAUDE.md, "an unmutexed sandvox.exe poisons
            # every perf number"). run.sh blocks until the lock is free, so a
            # build or another session's run simply delays this one.
            if not os.path.isfile(EXE):
                return self._json(400, {"ok": False,
                                        "error": "sandvox.exe not found -- build first"})
            with _perf_lock:
                if _perf["running"]:
                    return self._json(409, {"ok": False,
                                            "error": "a perf run is already in flight"})
                _perf.update(running=True, ok=False, log="", error="")

            body = self._body()
            scenario = body.get("scenario") or ""
            argv = ["bash", os.path.join(ROOT, "scripts", "run.sh"), EXE, "--perf"]
            # Whitelist: the scenario id is never spliced from client text.
            if scenario in ("idle", "treeburn", "flythrough", "explosion", "water"):
                argv += ["--scenario", scenario]
            elif scenario:
                with _perf_lock:
                    _perf.update(running=False, error="unknown scenario %r" % (scenario,))
                return self._json(400, {"ok": False, "error": "unknown scenario"})

            def run_perf():
                try:
                    env = dict(os.environ, SANDVOX_NO_CRASH_DIALOG="1")
                    r = subprocess.run(argv, cwd=ROOT, capture_output=True,
                                       text=True, timeout=2400, env=env,
                                       creationflags=getattr(subprocess,
                                                             "CREATE_NO_WINDOW", 0))
                    with _perf_lock:
                        _perf.update(running=False, ok=(r.returncode == 0),
                                     log=(r.stdout or "")[-20000:],
                                     error=(r.stderr or "")[-4000:])
                except Exception as e:
                    with _perf_lock:
                        _perf.update(running=False, ok=False, error=repr(e))

            t = threading.Thread(target=run_perf, daemon=True)
            t.start()
            # Synchronous from the page's point of view: the button is disabled
            # for the duration anyway, and a run that finishes into a status
            # endpoint nobody polls is a run whose failure nobody sees.
            t.join(2400)
            with _perf_lock:
                st = dict(_perf)
            return self._json(200, {"ok": st["ok"], "log": st["log"],
                                    "error": st["error"]})

        if p == "/api/kill":
            n = 0
            for q in _play_procs:
                if q.poll() is None:
                    q.kill()
                    n += 1
            _play_procs.clear()
            return self._json(200, {"ok": True, "killed": n})

        return self._json(404, {"ok": False, "error": "no such endpoint"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8777)
    ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    url = "http://127.0.0.1:%d/" % a.port
    print("sandvox tuner -> %s" % url)
    print("   assets: %s" % ASSETS)
    print("   ctrl-c to stop")
    if not a.no_open:
        threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nbye")


if __name__ == "__main__":
    sys.exit(main())
