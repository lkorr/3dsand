#!/usr/bin/env python3
"""test_svlock_fair.py -- the GPU lock's waiters' queue (scripts/svlock.sh
"FAIRNESS", tuner_server.py RunLock), against PRIVATE lock directories.

No exe, no GPU, touches nothing machine-wide: SVLOCK_GPU_DIR /
SVLOCK_LEGACY_DIR point both implementations at a temp dir.

  python scripts/test_svlock_fair.py

  A  STARVATION: a Python thread re-takes the lock back to back (the tuner
     streaming regions, zero gap between requests); a bash svlock_acquire that
     starts waiting meanwhile gets the lock within a few polls.
  B  FIFO ACROSS IMPLEMENTATIONS: bash #1 holds; bash #2 queues; a Python
     request arriving after #2 gets the lock only after #2.
  C  A DEAD WAITER'S TICKET (untouched past the stale window) does not hold
     the queue: it is reaped and the lock is taken.
  D  THE OLD GUARANTEES: a pid-less (orphan) lock dir older than 30 s is
     reaped by both; a stale-ts lock is reaped; RunLock does not delete a
     lock that is no longer its own.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TMP = tempfile.mkdtemp(prefix="svlock_fair_").replace("\\", "/")
os.environ["SVLOCK_GPU_DIR"] = TMP + "/gpu-lock"
os.environ["SVLOCK_LEGACY_DIR"] = TMP + "/legacy-lock"
os.environ["SVLOCK_COMPILE_DIR"] = TMP + "/compile-lock"
sys.path.insert(0, HERE)
import tuner_server as ts  # noqa: E402  (reads SVLOCK_GPU_DIR at import)

LOCK = os.environ["SVLOCK_GPU_DIR"]
SVLOCK = os.path.join(HERE, "svlock.sh").replace("\\", "/")
# Git Bash, by PATH: a bare "bash" through CreateProcess finds System32 (WSL) first.
BASH = shutil.which("bash") or "bash"
fails = []


def check(ok, what):
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        fails.append(what)


def bash_acquire(hold, log, tag):
    """A bash shell that svlock_acquire's the GPU lock (+ legacy, as run.sh
    does), logs when it got it, holds `hold` s, releases."""
    script = ('source "%s"; t0=$(date +%%s%%3N); svlock_acquire_gpu "test:%s"; '
              'echo "%s got $(( $(date +%%s%%3N) - t0 ))" >> "%s"; sleep %s; svlock_release_all; trap - EXIT'
              % (SVLOCK, tag, tag, log, hold))
    return subprocess.Popen([BASH, "-c", script], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_log(log):
    try:
        with open(log) as f:
            return [l.split() for l in f.read().splitlines() if l.strip()]
    except OSError:
        return []


# ---- A ----------------------------------------------------------------------
stop = threading.Event()
takes = [0]


def hammer():
    while not stop.is_set():
        with ts.RunLock("hammer", timeout=60):
            takes[0] += 1
            time.sleep(0.02)


th = threading.Thread(target=hammer, daemon=True)
th.start()
time.sleep(0.5)
logA = TMP + "/a.log"
p = bash_acquire(0.2, logA, "A")
p.wait(timeout=60)
stop.set()
th.join(10)
rows = read_log(logA)
ms = int(rows[0][2]) if rows else -1
check(rows and ms < 3000, "A: bash acquired in %d ms against a back-to-back Python taker (%d Python takes)" % (ms, takes[0]))

# ---- B ----------------------------------------------------------------------
logB = TMP + "/b.log"
b1 = bash_acquire(2.0, logB, "B1")
time.sleep(1.0)                       # B1 holds
b2 = bash_acquire(0.5, logB, "B2")
time.sleep(1.0)                       # B2 is queued (ticket written)
qn = len([n for n in os.listdir(LOCK + ".queue") if not n.endswith(".tmp")]) if os.path.isdir(LOCK + ".queue") else 0
with ts.RunLock("B3", timeout=60):
    with open(logB, "a") as f:
        f.write("B3 got 0\n")
b1.wait(30)
b2.wait(30)
order = [r[0] for r in read_log(logB)]
check(qn >= 1 and order == ["B1", "B2", "B3"], "B: FIFO across bash and Python: %s (queue had %d ticket(s))" % (order, qn))

# ---- C ----------------------------------------------------------------------
os.makedirs(LOCK + ".queue", exist_ok=True)
dead = LOCK + ".queue/sh99999-1"
with open(dead, "w") as f:
    f.write("1 test:dead")
old = time.time() - 60
os.utime(dead, (old, old))
t0 = time.time()
with ts.RunLock("C", timeout=10):
    pass
check(time.time() - t0 < 1.0 and not os.path.exists(dead), "C: a dead waiter's ticket was reaped, Python took the lock in %.2f s" % (time.time() - t0))
with open(dead, "w") as f:
    f.write("1 test:dead")
os.utime(dead, (old, old))
logC = TMP + "/c.log"
bash_acquire(0, logC, "C").wait(30)
rows = read_log(logC)
check(rows and int(rows[0][2]) < 2000 and not os.path.exists(dead), "C: ...and by bash (%s ms)" % (rows[0][2] if rows else "?"))

# ---- D ----------------------------------------------------------------------
os.mkdir(LOCK)                        # an orphan: no pid
os.utime(LOCK, (old, old))
t0 = time.time()
with ts.RunLock("D", timeout=10):
    pass
check(time.time() - t0 < 1.0, "D: Python reaped a 60 s old pid-less lock")
os.mkdir(LOCK)
os.utime(LOCK, (old, old))
logD = TMP + "/d.log"
bash_acquire(0, logD, "D").wait(60)
check(bool(read_log(logD)), "D: bash reaped a 60 s old pid-less lock")
os.mkdir(LOCK)                        # a stale holder: ts 20 min old
for n, v in (("ts", str(int(time.time()) - 1200)), ("who", "x"), ("pid", "1")):
    with open(os.path.join(LOCK, n), "w") as f:
        f.write(v)
with ts.RunLock("D2", timeout=10):
    pass
check(not os.path.isdir(LOCK), "D: a 20-minute stale lock was reaped and the new one released")
rl = ts.RunLock("D3", timeout=10)
rl.__enter__()
with open(os.path.join(LOCK, "pid"), "w") as f:   # stolen meanwhile
    f.write("424242")
rl.__exit__(None, None, None)
check(os.path.isdir(LOCK), "D: RunLock does not release a lock that is no longer its own")
shutil.rmtree(LOCK, ignore_errors=True)
leftover = [n for n in os.listdir(LOCK + ".queue")] if os.path.isdir(LOCK + ".queue") else []
check(not leftover, "no tickets left behind (%s)" % leftover)

shutil.rmtree(TMP, ignore_errors=True)
print("test_svlock_fair: " + ("PASS" if not fails else "FAIL (%d)" % len(fails)))
sys.exit(1 if fails else 0)
