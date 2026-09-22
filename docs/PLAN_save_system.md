# PLAN: save system for an open-world RPG (2026-09-22)

Orchestrated plan. Each `## S<n>` section below is one worktree package and is
handed to its agent verbatim. Read the **Context** and **Shared rules** first —
they apply to every package.

## Context (the assessment this plan answers)

The game is headed for a finite, Elder-Scrolls-shaped open world with NPCs,
quests, movable objects, buildable structures, and players who travel
independently (multiplayer). What exists today:

- `world.svd/` = `meta.svm` (SVM5: constants, material NAME table, tick, seed;
  written LAST) + `r_x_y_z.svr` region files (16³ chunks each, `ChunkStore`,
  LRU 64 regions in RAM, tmp+rename writes) + `entities.sve` (TLV sections
  `DBRS MOBS AVTR PLYR ITMS WTRB`, per-section versions, unknown skipped, all
  content by NAME) + `manifest.svt` (M9.5 tick tags). `src/sim/worldio.*`,
  `src/sim/chunkstore.*`, `src/game/persist.*`.
- **The world is a delta from the seed.** `Stream::EvictSlots` skips chunks
  whose sticky `modified_` bit is clear (`stream.cpp:425`) because `genChunk`
  reproduces them. A pristine chunk costs zero bytes. This is the property the
  whole plan protects: a 20 km finite map at 0.1 m voxels is ~10¹² voxels and
  only the delta is storable.

Problems, in priority order (each maps to a package):

1. **The save path defeats the delta** (S1). `FlushResident()` evicts with
   `filter=false`, so every one of the 32,768 resident chunks is stored,
   pristine included, and they are never dropped afterwards. JITTER and mixed
   surface chunks RLE-expand to ~22–32 KiB each (palette jitter makes nearly
   every cell its own run).
2. **No worldgen identity in meta** (S1). A save records the seed but not the
   generator. Any `map.json` / tree re-bake / worldgen WGSL change silently
   regenerates every pristine chunk under a save differently, seaming against
   the stored ones.
3. **Settle-after-generation dirties pristine terrain** (S2). Measured in
   `stream.cpp`: 9% of the real-page slots on a leaving plane under
   `--autofly-surface` were `modified_` without any player edit (CA settling,
   water levelling, plants). Save size then scales with area EXPLORED, not
   with what the player did.
4. **On-disk encoding has no entropy coding** (S3). `(u32 run, u32 word)`
   pairs, 8 bytes per run.
5. **Entity persistence is one monolithic file, single-player shaped** (S4).
   `entities.sve` is rewritten and loaded whole; `AVTR`/`PLYR` (the player)
   live inside the world file; time of day / weather are not persisted.
6. **NPCs cease to exist out of window** (S5). `MobSystem` despawns
   out-of-window mobs (`mob.h` ~4213). `Mob::SaveOne` writes every limb's full
   `voxels` + `skinVoxels` lists even for an undamaged body.

## Shared rules (every package)

- Work ONLY in your worktree. Read the repo `CLAUDE.md` first; it is binding
  (build via `scripts/build.sh`, run every exe via `scripts/run.sh`,
  `export SANDVOX_NO_CRASH_DIALOG=1`, board claims via `scripts/board.sh`).
- C++ package: **build once** with `bash scripts/build.sh` in your worktree
  (sccache makes deps free). Do not rebuild "to be sure".
- **Verification budget:** iterate with `--selftest --gate <name>` on the
  gates you touch (`save-load`, `region-store`, plus any gate you add). Do
  NOT run `--suite acceptance` or the full `--selftest` — the orchestrator
  runs acceptance once at the end on the merged tree. Do NOT rebaseline the
  determinism hash; none of these packages should move it (saves are outside
  the hashed domain). If `--gate determinism` moves, that IS a finding —
  report it.
- Thresholds and expected values go in `tests/baseline.json`, not C++.
- Save-format changes: bump the magic/version, keep the **previous version
  loadable** (the SVM4→SVM5 append precedent in `worldio.h`), refuse
  anything you cannot read correctly rather than half-applying it.
- Update `DESIGN.md` §3 (Streaming / Save-format paragraphs) in the same
  commit if your change contradicts it. Also fix the stale "16³ voxels = 8 KB
  per chunk" line (it is 16 KiB since the word went to 32 bits) — S1 owns
  that one line.
- `assets/tuner.html` ARCH_NODES: add/update the save-system node's `desc`
  for what you landed (ASCII `'` delimiters only).
- **Commit on your worktree branch** with a descriptive message (repo style:
  a sentence stating the new truth). Do not merge to main; the orchestrator
  merges. Stage by path, never `git add -A`.
- Final report: what landed, the commit sha, gates run + results, measured
  numbers (bytes before/after where applicable), anything deferred, and any
  file outside your list you had to touch and why.

---

## S1 — the save stores the delta, and meta names its generator

**Files:** `src/sim/stream.{h,cpp}` (FlushResident / EvictSlots),
`src/sim/worldio.{h,cpp}`, `src/sim/chunkstore.{h,cpp}` (only if BindSave
semantics need it), `src/test/selftest_worldio.cpp`, `tests/baseline.json`,
`DESIGN.md` (§3 save paragraphs + the 8 KB line), `assets/tuner.html` (node).

**A. Save only modified chunks.** Make the save path apply the same
"unmodified is reproducible" rule eviction uses. Things to get right:
- A chunk that was decoded FROM the store and not modified since is still in
  the store (Get does not consume) — skipping it is correct.
- `BindSave` on an unbound store wipes old region files and marks all RAM
  regions dirty ("this RAM store is the whole world") — confirm that stays
  correct once pristine chunks are no longer written.
- Pristine chunks that OLD saves (or the current save path) already wrote are
  dead weight forever. Add a prune: when a chunk is evicted/flushed and is
  unmodified AND byte-identical to what `genChunk` would produce… you cannot
  cheaply know that on CPU for real pages. Acceptable simpler rule: a chunk
  entering the window FROM the store that is then evicted unmodified stays
  (it may hold real edits). Only prune what you can prove. Document the
  choice; do not invent an expensive GPU comparison unless it is cheap.
- The save must still round-trip: the `save-load` gate compares the world
  after load — make sure pristine chunks regenerate identically (they should:
  same seed, same generator).
- Measure: bytes written by one save of a freshly generated, untouched world,
  before and after (`ChunkStore::Flush` reports `bytesOut`). Put the number
  in the commit message and DESIGN.md. Add a `save-load` (or new
  `save-size`) check that an untouched world's save stays under a
  `tests/baseline.json` threshold, so this cannot regress silently.

**B. Worldgen fingerprint (SVM6).** Append a `u64 worldgenFingerprint` to
meta (append-only, SVM5 still loads with fingerprint UNKNOWN, like SVM4's
tick/seed). It must change when pristine chunk CONTENT would change, and
ideally not when only a comment changes. Preferred: hash of `genChunk`
output for a small fixed set of probe chunks (surface, cave depth, water,
a tree site — pick from the map's site table / existing worldgen gates) if
that costs < ~100 ms at save and load; otherwise a hash of the generator's
INPUTS (assembled worldgen WGSL source, `assets/worldmap/<name>/*`,
`assets/trees/*.svtree`, biomes, material name table). State which you chose
and why in `worldio.h`.
Policy on mismatch at load: **load, with a loud log line and a
`worldgenMismatch` flag** surfaced in `build/last_run.json` and to the caller
(`WorldStamp` gains the field). Do NOT refuse: dev iteration changes worldgen
daily and refusing would kill every dev save. The shipped-game policy
(frozen generator versions vs new-gen-only-in-unvisited-chunks) is deferred
and must be written as an open question in `worldio.h` and DESIGN.md.
Gate: save, perturb the fingerprint input (or fake the stored value), load,
assert the flag is set; unperturbed round-trip asserts it is clear.

## S2 — terrain does not change just because someone looked at it

**Files:** a new gate in the appropriate `src/test/selftest_*.cpp` (register
in `selftest.cpp` `kOrder` — read the ordering note in `selftest.h`),
`src/sim/stream.h` (a read-only accessor for the modified set / counters if
needed — S1 also edits stream.*, keep your stream.* change to an additive
accessor), `tests/baseline.json`, `DESIGN.md` (a short paragraph).

**Goal:** turn "9% of surface chunks come out modified with no player input"
from an anecdote into an attributed, gated number, per CLAUDE.md rule 6
("record at the point of failure; attribution beats elimination").

- Gate `gen-settle` (name free to adjust): generate a window, run N ticks with
  NO player/brush/spell input, then count chunks whose `modified_` bit is set
  and ATTRIBUTE them: which material changed / which field (material,
  fullness, stain, state nibble), depth below surface, and the writer class
  if knowable (CA powder move, liquid level, plant growth, reaction). The
  existing `terrain` gate's pass D already diffs awake chunks by field —
  reuse that machinery, do not duplicate it.
- Also measure the traversal case: a short fixed `--autofly`-style camera
  path over untouched terrain with no edits, reporting modified chunks per
  plane evicted, attributed the same way.
- Assert against thresholds in `tests/baseline.json` set from what you
  measure (so it cannot get WORSE); record the attributed breakdown in the
  gate output and `build/last_run.json`.
- **Fixing the sources is in scope only if a cause is trivially fixable and
  data-only** (e.g. one worldgen rule birthing powder off its angle of
  repose). Anything bigger: write it up as a ranked list in DESIGN.md with
  the numbers and leave it. Do NOT touch `worldgen.wgsl`/`common.wgsl`
  without saying so in the report (and remember `common.wgsl` edits cost a
  full shader-cache miss). A worldgen fix WILL move the determinism hash —
  that is expected; say so, do not rebaseline.

## S3 — region files are compressed

**Files:** `src/sim/chunkstore.{h,cpp}` (on-disk format only),
`CMakeLists.txt` (zstd via FetchContent into the shared `C:/sv-deps` cache —
read the Build gotchas in CLAUDE.md; `FETCHCONTENT_BASE_DIR` is set before
`include(FetchContent)`), `src/test/selftest_worldio.cpp`,
`tests/baseline.json`, `DESIGN.md`.

**Scope boundary — keep it:** the IN-MEMORY representation stays the current
RLE (`std::vector<uint32_t>` pairs). `RleEncodeChunk`/`RleDecodeChunk`,
`ForEachStored`, the M9.5 chunk exchange and `FarEdits::RebuildFromStore` all
consume it and must not change. Only `WriteRegion` / `EnsureLoaded` /
`ForEachStored`'s disk branch change: encode at write, decode at read.

- New region magic `SVR3`; `SVR2` still loads.
- Per-chunk record (keeps per-chunk random access within a region):
  split the decoded 4096 words into planes — material (12 bits → u16),
  state nibble, stain byte (bits 24..31) — and **XOR the state nibble with
  the positional palette jitter worldgen would have produced** for that cell
  (`JitterStateInRow` / `SynthWord` machinery in `world.h`; the store needs
  the seed — thread it in at bind). Untouched cells become 0; moved grains
  carry a nonzero residue, which is fine. Verify the jitter function you use
  is exactly the one worldgen/JITTER uses (there is a CPU mirror — find it,
  do not write a second copy). Then zstd the planes (level ~3; measure 1/3/9
  and pick). Lossless: decode must reproduce the RLE words bit-exactly under
  `kPersistMask`.
- Measure on real data: bytes for a set of modified chunks (brush craters,
  a flooded area, burnt forest — use existing selftest fixtures) SVR2 vs
  SVR3, and encode/decode µs per chunk. Encode/decode happen on LRU spill,
  so the per-chunk cost must stay small (report it; flag if > ~200 µs).
- Gate: round-trip bit-exactness across a varied chunk set, plus SVR2 file
  loads under the new build, plus a compression-ratio floor in
  `tests/baseline.json`.

---

## Wave 2 (not started until S1–S3 merge AND the board shows `src/game/mob.*` released)

## S4 — entities split by owner: region, world, player

Split `entities.sve` into:
- **per-region** `r_x_y_z.sve` beside each `.svr`: world-anchored entities
  (`DBRS`, `ITMS`, `MOBS`) bucketed by position, loaded/written with the
  region so they stream with the terrain;
- **global** `world.sve`: `WTRB`, time of day / celestial state, weather,
  and the future homes of quest state, world flags, factions;
- **per-player** `players/<id>.svp`: `AVTR` + `PLYR`, keyed by a stable
  player/account id (single player uses a fixed id). Multiplayer peers save
  their own player files; the host saves the world.
Old `entities.sve` still loads (read and distributed into the new layout).
Persist time of day + weather (new section) since they are absent today.

## S5 — NPCs outlive the window, and a whole body saves as its name

- Mob limb delta: a limb that was never carved saves as a flag, not its
  voxel lists; only carved limbs store lattices (bump `MOBS` version, keep
  the previous version loadable).
- Dormant records: a mob leaving the window is serialized (`SaveOne`) into
  its region's entity bucket instead of despawned, and re-instantiated
  (`LoadOne`) when its chunk re-enters. Spawn caps count only live mobs.
  Minimal AI brain state (target, mode) travels with it.
- Design note (not implemented): authored NPCs need stable content ids
  (`npc:<name>`) distinct from runtime `nextId_`, so quest state can name
  them.
