// sim_stream.wgsl — STREAMS BECOME HELD (docs/PLAN_running_water.md phase 2;
// DESIGN.md §5b.13).
//
// THE PROBLEM. Phase 1's held rule lets a map river, lake or sea rest however
// its surface falls, and makes it an infinite reservoir: a bank somebody digs
// through spills, and the river refills what it spilled at once. If the
// breach runs into a hollow, the hollow fills to the river's level and the
// whole thing sleeps on its own. If it runs DOWNHILL -- to the sea, to the
// river further down, to another lake -- it is a stream fed and drained
// forever: the conveyor phase 1 removed from the authored rivers, rebuilt by
// the player, every chunk along it awake for as long as it runs (rule 2).
//
// THE ANSWER is the authored rivers' own: a stream that has stopped changing
// is HELD where it is. A second record per window column, beside the authored
// one in the still table (worldgen.wgsl's STILL-WATER TABLE, words [2] and
// [3]), carries a DYNAMIC target that sim_step.wgsl's stillLevel8 reads when
// the column has no authored one. Once a column holds a dynamic target the
// held rule, the reservoir and the renderer treat it exactly like a river:
// at rest it pours nothing into a full held neighbour, under its target it is
// refilled, and its chunk sleeps. A stream is adopted column by column from
// its source down, and the last held column of it meets whatever it drains
// into (held: at rest; not held: it still spills, so the front keeps moving
// until it reaches something).
//
// ADOPTION, per column, by the two kernels below on every tick the CA ran:
//   * the column's SURFACE is its highest free-surface liquid cell (liquid,
//     non-liquid above) under the column's topmost awake chunk; s8 = 8 y + f;
//   * it is STEADY when its surface has stayed within one voxel (8 eighths)
//     of where it stood when the count began, for STREAM_ADOPT_TICKS visits
//     (a jump restarts the count from the new surface; a tick with no surface
//     halves it: a thin stream flickers). A trench still FILLING rises out of
//     the band and is not adopted part way up: it fills, rests at its
//     feeder's level and sleeps as plain water. (Adoption does not disturb
//     the column below it: a held cell under its target is refilled before
//     it moves -- sim_step.wgsl stage 0 -- so an adopted column hands on what
//     it did before);
//   * it is FED when a lateral neighbour holds a target T >= s8 -- the map's
//     own water (hop 0) or an adopted column (hop h) -- whose water stands at
//     its surface where it can step into this column (its top cell is that
//     liquid; this column's cell at that row is not a wall); the column takes
//     hop = the smallest such h + 1, so hops strictly fall toward the map's
//     water and support can never run in a circle;
//   * it has OUTFLOW: somewhere lower to run beside its surface cell, or a
//     held neighbour (the map's water or an adopted column) whose target is
//     at or under its surface -- the body it drains into (a pool with no way
//     out has neither, and sleeps as plain water);
//   * steady, fed and running: adopted at its current surface.
// RELEASE: an adopted column that is visited (something woke its chunk) and is
// no longer fed at its own target by a neighbour of SMALLER hop is released -- a plugged breach drains its
// stream from the top down: the released water is ordinary water, it flows
// into the next held column as water over its target, that wakes it, and it
// finds its own support gone.
// FLOODED (2026-10-16 audit): an adopted column that has stood OVER its
// target long enough is released too -- a leaky count, +1 a visit its water
// stands over the target, +STREAM_FLOOD_DEEP a visit it stands more than a
// voxel over, -1 a visit it does not, released at STREAM_FLOOD_COUNT. The
// steady test cannot tell a stream from a basin filling slowly (a big hollow
// a breach fills at under a voxel per STREAM_ADOPT_TICKS), so a column of
// such a basin is adopted part way up; the basin then rises past that target,
// and its water, fed for ever through the breach, ran into the column and
// back to the reservoir (GIVE) for ever -- a source and a sink with a chunk
// awake between them (rule 2). Released, the basin rises to its feeder's
// level and sleeps (or is adopted there, at the level it actually holds).
// WHY SO SLOW, measured: at a count of 120 the `rivers` S stream flapped --
// 22, 45, 27, 15, 69, 12, 112 ... of 153 columns adopted, instead of 6, 28,
// 46 ... 153 -- because a column of a stream that is STILL BEING ADOPTED
// stands over its target for as long as the unadopted water upstream of it
// still runs into it (GIVE drains it, the running water refills it), which
// is the source-and-sink pattern exactly, only transient; and one release
// cascades down the whole stream. The gate's reporter (S's floodMax) shows
// it: the largest count among the adopted columns climbs ONE A VISIT from
// tick 150, when the first six are adopted (the columns at the river's
// level, which the river feeds and the stream drains into), to 912 at tick
// 1050, when the last column adopts and the stream stops running, then
// falls. A stream of a thousand columns would take the mouth past 4000 and
// release it -- harmless: nothing upstream rests on the mouth's support, so
// it releases alone and is adopted again. So a shallow flood takes
// STREAM_FLOOD_COUNT visits (about a minute) and only a deep one -- a voxel
// or more over its target, which running water arriving at reach 1 did not
// do in S (the count never stepped by STREAM_FLOOD_DEEP) -- is fast. Either
// way the source and the sink are BOUNDED, which they were not.
//
// RULE 1. ONE WRITER PER COLUMN: only the topmost dirty chunk of a column's
// window stack acts for it (the stack above it is asleep by construction).
// Kernel `scan` reads neighbours' COMMITTED targets (word [2]) and writes only
// its own column's word [3] (observation, age, and a pending action); kernel
// `commit`, after a barrier, applies its own column's action to word [2]. The
// ONE word a scan thread reads that another may be writing is an adopted
// neighbour's [3], for its liquid -- and scan never changes an adopted [3]'s
// low 12 bits (only the flood count above them and the action), so whichever
// value the read sees, the liquid is the same. Every decision is a function
// of the voxels after the CA and the committed table -- the order threads run
// in cannot matter.
// RULE 2. A column is visited only while its chunk is dirty; adoption only
// ever ADDS held columns, release only removes ones whose support is gone or
// that a basin has drowned, and nothing here writes a voxel, so this pass
// cannot keep anything awake (commit's one wake per release, phase 3, happens
// once per adoption).
// Derived, not saved: a reloaded stream runs as water and is adopted again.
//
// ============ THE CONDUIT: A STREAM FROM A FINITE SOURCE (phase 3) ===========
// docs/PLAN_running_water.md phase 3; tickets 2026-10-16_11-16-43 / 11-14-41.
// Everything above roots a stream on the map's water, whose reservoir is
// infinite. A stream whose source is FINITE -- a player's pour, a pool, a
// broken tank -- has nothing held to root on, so every chunk along it stayed
// awake until the source was empty. Now such a stream is adopted too, as a
// POOL stream ([3] bit STREAM_POOL_BIT), and its accounting is a LEDGER
// instead of an infinite reservoir:
//   * ROOT: an unadopted column whose held feeders fail may be FED by an
//     UNHELD lateral neighbour of the same liquid -- the live source -- whose
//     surface stands at least STREAM_POUR eighths over this column's, that can
//     actually pour (sourcePours: a cell able to split, or water under its
//     top cell, or the same row), with an open way in, AND that is itself a
//     ROOT: no lateral neighbour of IT stands STREAM_POUR over it (isRoot).
//     Every column of a running stream is fed by the unheld column upstream
//     of it; only the one at the top of the slope (the pool's edge, whose
//     neighbours are the pool at its own level) is fed by a column nothing
//     higher feeds. So POOL adoption starts at the source and walks down the
//     hop chain exactly as a map stream does (hop 1 at the root, h + 1 after).
//     A column adopted from a POOL feeder is POOL (ties of hop go to the
//     map's: an infinite feeder is the better account).
//   * RELEASE, with HYSTERESIS: an adopted POOL column with no held feeder of
//     smaller hop stays held while ANY unheld neighbour still pours into it
//     (sourcePours, no root test). That is what lets the stream RETREAT from
//     the top as the source runs down instead of cascading: a released head's
//     water is unheld and still pours into the column below, which is now the
//     head; when the leftover film can no longer pour, that column goes too.
//   * THE LEDGER (sim_step.wgsl THE CONDUIT): what the source pours into a
//     POOL column over its target is DEPOSITED, what the mouth spills out is
//     WITHDRAWN to refill it, and a withdrawal needs the ledger as of the
//     start of the tick (the SNAPSHOT this kernel's `commit` copies) to be
//     positive. Mass is moved, never made: voxels + ledger is exact.
//   * STARVED: an adopted POOL column under its target while the snapshot is
//     empty counts on the flood count (+1 a visit), so a mouth waiting on a
//     source that has stopped pouring is released in bounded time.
// Rule 1: the snapshot is written by ONE thread (commit's workgroup 0) after
// the barrier that ends the CA's adds, and read by the next tick's CA and
// scan; nothing reads the live word in the pass that adds to it.
// Dense worldgen clears the whole table (pass_table.def fill_stillWater); a
// LOAD clears the dynamic words (`clearStreams`, pass_table.def
// lr_streamClear -- the load refills the window through the streamed path,
// and a window loaded where the old one stood has every tag unchanged); a
// streamed window column keeps its dynamic words only while its tag (the
// world column) is unchanged (worldgen.wgsl cols), which a vertical window
// shift, regenerating every column, must not cost the streams.

@group(0) @binding(0) var<storage, read_write> voxels   : array<u32>;
@group(0) @binding(1) var<storage, read>       dirtyIn  : array<u32>;
// `commit` wakes a released column's chunk (WAKE ON RELEASE, phase 3).
@group(0) @binding(2) var<storage, read_write> dirtyOut : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read>       materials : array<Material>;
@group(0) @binding(4) var<uniform> T : TickParams;
@group(0) @binding(12) var<storage, read_write> dirtyList : array<u32>;
@group(0) @binding(17) var<storage, read>       pageTable : array<u32>;
@group(0) @binding(18) var<storage, read_write> pageFaults : array<atomic<u32>>;
@group(0) @binding(58) var<storage, read_write> stillWater : array<u32>;
// This pass stores no voxel; the identity only satisfies common.wgsl.
const PT_KERNEL : u32 = PT_K_STEP;

// worldgen.wgsl's STILL_WORDS / stillIndex / stillTag (and sim_step.wgsl's).
const STILL_WORDS : u32 = 4u;
fn stillIndex(x : i32, z : i32) -> u32 {
  return (bitcast<u32>(z & WORLD_MASK) * WORLD_N + bitcast<u32>(x & WORLD_MASK)) * STILL_WORDS;
}
fn stillTag(x : i32, z : i32) -> u32 {
  let tx = bitcast<u32>(x >> firstTrailingBit(WORLD_N)) & 0x7FFFu;
  let tz = bitcast<u32>(z >> firstTrailingBit(WORLD_N)) & 0x7FFFu;
  return 1u | (tx << 1u) | (tz << 16u);
}

fn inBounds(c : vec3<i32>) -> bool { return inWindow(c, T.origin); }

// How long a surface must hold still before it is held, in ticks.
const STREAM_ADOPT_TICKS : u32 = 120u;
// How far down from the top of its chunk a column's surface is looked for.
const STREAM_SCAN : i32 = 32;
// THE TWO DYNAMIC WORDS.
//   [2] adopted: the target T8 (20 bits, eighths, signed) | HOP << 20 (12
//       bits, 1..STREAM_HOP_MAX): how many columns of stream lie between this
//       one and the map's water that feeds it. 0 = not adopted.
//   [3] not adopted: the observed surface (20 bits) | age << 20 (10 bits) |
//       the pending action << 30. Adopted: the liquid's material (12 bits) |
//       the flood count << 12 (12 bits) | the pending action << 30 --
//       sim_step.wgsl stillLevel8 reads the material from here (low 12 bits
//       only), so an adopted record never claims another liquid.
// A pending ADOPT carries the hop in the age bits (the commit writes [2]).
const STREAM_AGE_MAX : u32 = 1023u;
const STREAM_HOP_MAX : u32 = 1023u;
const STREAM_ACT_ADOPT : u32 = 1u;
const STREAM_ACT_RELEASE : u32 = 2u;
const STREAM_NONE : i32 = -1073741824;
// THE FLOOD COUNT (see FLOODED above), [3] bits 12..23 of an adopted column.
const STREAM_OVER_SHIFT : u32 = 12u;
const STREAM_OVER_MAX : u32 = 4095u;
const STREAM_FLOOD_COUNT : u32 = 4000u;
const STREAM_FLOOD_DEEP : u32 = 16u;
// THE CONDUIT (phase 3). An adopted [3]'s bit 24 says the stream is rooted on
// a FINITE source (sim_step.wgsl reads it: STILL_POOL_BIT there). A pending
// adopt of a POOL column is action 3 (the age bits carry the hop, as for 1).
const STREAM_POOL_BIT : u32 = 1u << 24u;
// THE SHORT WINDOW: a column fed by a held POOL column is adopted after this
// many steady visits, not STREAM_ADOPT_TICKS. The 120 is a guard against
// freezing a surface still on its way somewhere; below a held POOL column
// there is no such surface -- the ledger refills an adopted column to its
// target whatever it stood at -- and the long window is what left the thin,
// flickering tail of a finite stream unadopted for good (measured,
// `stream-pool`: the last ~15 of 80 columns never adopted, their gaps kept
// the middle awake). A ROOT (hop 1 from a live source) keeps the long window:
// the source must really be steady.
const STREAM_ADOPT_TICKS_POOL : u32 = 24u;
// A ROOT's steady window: twice the map's. A shallow channel fed by a lake has
// deep, flat water at its top that passes every root test, and a POOL root
// there raced the lake's own hop chain coming down the same channel (measured,
// `rivers` S: 78 of the first 87 adopted columns POOL). At twice the window
// the map chain, one column a tick once its first column is adopted, is there
// first.
const STREAM_ADOPT_TICKS_ROOT : u32 = 240u;
// A ROOT's width: cells of the 48 round it that hold the liquid (sourceWide).
const STREAM_ROOT_WIDE : i32 = 29;
// A ROOT's depth in cells (sourceDeep).
const STREAM_ROOT_DEPTH : i32 = 3;
// RETARGET: the most a POOL column's target is lowered in one step, eighths
// (a voxel). A source that fell further than that is let go instead.
const STREAM_RETARGET_MAX : i32 = 8;
const STREAM_ACT_ADOPT_POOL : u32 = 3u;
// "Pours": a source must stand this many eighths over a column's surface --
// the CA's own equalize threshold (TUNE_LIQUID_EQUALIZE is 2): less than that
// and no lateral move exists between them.
const STREAM_POUR : i32 = 2;
// sim_step.wgsl's LIQ_SPLIT_MIN: a cell this full can split into air.
const STREAM_SPLIT_MIN : u32 = 2u * clamp(TUNE_LIQUID_MIN_FILM, 1u, 4u);
// How far up a neighbour column is searched for its surface, in cells.
const STREAM_UP : i32 = 8;
// THE LEDGER: two words after the column records (sim_step.wgsl STILL_LEDGER
// must agree): [0] the live balance in eighths (i32, atomic adds in the CA),
// [1] its snapshot at the end of the last tick that ran (written by `commit`),
// [2] / [3] the eighths deposited / withdrawn so far (diagnostic only).
const STREAM_LEDGER : u32 = WORLD_N * WORLD_N * STILL_WORDS;
// sim_step.wgsl STILL_LEDGER_CAP: the ledger is FULL at this (THE CONDUIT).
const STREAM_LEDGER_CAP : i32 = 512;

fn sdec(v : u32) -> i32 { return bitcast<i32>(v << 12u) >> 12u; }
fn ledgerSnap() -> i32 { return bitcast<i32>(stillWater[STREAM_LEDGER + 1u]); }

// ================ STREAMS ADOPT ONLY WHEN NATURAL (2026-10-17) ================
// Tickets 2026-10-17_11-26-13 ("it solidifies into vertical waterfall column
// things ... the vertical columns rise in an unnatural way for a second or
// so, then collapse into a slightly more normal mound shape before building
// again") and 11-37-20 (a stream dug beside a river "slowly becomes a sheer
// frozen waterfall"). The owner's rule: a stream freezes only after a long
// time in which it has not been carrying water anywhere NEW, only if it is
// fed from the map's water, and frozen it must look and deliver like the live
// one. What phase 2 did instead, measured by `stream-natural`:
//   * STEADY was "within a voxel of where the count began for 120 visits".
//     A flood spreading over flat ground and a basin filling below a stream
//     rise slower than that, so their columns were held part way up -- each
//     at whatever height it stood at THAT visit, a splash crest or a trough;
//   * the reservoir then filled every held column to that height at once
//     (TAKE / GROW, and its held neighbours poured into it): the RISE;
//   * the basin rose on past those targets, the flood count released them,
//     the water slumped (the COLLAPSE), and 120 visits later they were
//     adopted again at new heights: the CYCLE, pillars and drop-offs each time;
//   * water in flight (over a step, off a ledge) was the "surface" too.
// THE NATURAL TEST, for a column a MAP-fed neighbour would feed (the conduit's
// POOL streams keep their own window, below):
//   * its RESTING surface -- the top of the liquid standing on its bed;
//     liquid with air under it is in flight and is not the surface -- is
//     observed in SUB-WINDOWS of STREAM_SUB visits: the window's MEAN against
//     an ANCHOR (the mean the run began at). Flicker averages out; a trend
//     does not. A window whose mean is more than STREAM_MEAN_TOL eighths off
//     the anchor re-anchors and restarts the column's own run;
//   * what it tells the columns ABOVE it is coarser: its level over a BLOCK
//     of STREAM_BLOCK windows (512 visits) against the block before -- a
//     change of STREAM_CHAIN_TOL (half a voxel) is a FINE event -- and its
//     FIRST wet window (a new area), which is FINE and COARSE. A level that
//     only wanders inside a few eighths -- a stream's own flicker and slosh,
//     a film over a lip coming and going, a sink settling, the pool under a
//     cascade -- is quiet to everything upstream; a flood spreading or a
//     basin filling half a voxel a block is not. (Measured, `stream-natural`:
//     a test window by window put an event somewhere in a steady through-flow
//     every 50-200 ticks, and nothing above it was ever still.)
//   * the run must last sim.streamSteadyTicks of windows in which most
//     visits found a resting surface (3 of 4) and few found water in flight
//     (a quarter at most);
//   * NOTHING DOWNSTREAM moved for as long: every column keeps the newest
//     fine and coarse event stamp among its own and its DOWNSTREAM
//     neighbours' (open beside its surface, no water standing over it there:
//     same level or lower), published by `commit` and read by the neighbours'
//     next `scan`, so an event anywhere down a stream reaches its top one
//     column a tick later. The map's water is a sink that never changes (it is
//     never scanned). A flood still spreading, a basin still filling, keep
//     every column above them live;
//   * it is held at its ANCHOR -- the mean surface its run held: what is
//     frozen is the shape it had on average, not a crest or a trough, and the
//     reservoir fills or takes back no more than the flicker. The HEAD (fed
//     by the map's own water) is taken at a visit within STREAM_AT_TOL of its
//     anchor; the FRONT after it (fed by a held stream column) at once -- see
//     THE FRONT DOES NOT WAIT at the test.
// RELEASE, besides the old rules (support gone, flooded): a map-fed held
// column whose own water stands more than a voxel under its target, or is
// gone (DRAINED) -- something below took more than the stream brings, or the
// stream above stopped feeding it. NOT on news from below: a held map-fed
// stream is not refilled from the reservoir (sim_step.wgsl stillWant, A
// MAP-FED STREAM IS NOT REFILLED), so it delivers into a dig or a draining
// basin as the live stream would, its water running down through it, and it
// rests again when nothing below takes water. (Released on the first new
// wet column below, as the first cut was, it let go whenever an established
// cascade's side lane got wet for the first time; held edges refilled from
// the reservoir delivered at 3.6 times the live rate. Measured,
// `stream-natural`.)
// A released column re-observes from scratch -- a whole window again -- and
// its release is a FINE event for the columns above it, which therefore wait
// the window too: no adopt/release cycle faster than the window.
// RULE 1: everything a scan reads of another column (its published stamps,
// its tag word) is written only by `commit`, after the barrier; a scan writes
// only its own column's words. RULE 2: unchanged -- this pass writes no voxel.

// THE STREAM WORD: TickParams' first padding word after soluteDrift
// (`_padSplash0`, support.cpp SubmitTick): sim.streamSteadyTicks in the low
// 16 bits, sim.streamPoolRoot in bit 16.
fn streamSteadyTicks() -> u32 { return max(T._padSplash0 & 0xFFFFu, 32u); }
fn streamPoolRoot() -> bool { return (T._padSplash0 & (1u << 16u)) != 0u; }
// The most open cells under a run of liquid for it to still be a step's
// spill rather than water in flight (restingSurface).
const STREAM_STEP_GAP : i32 = 1;
// Visits per observation window. 64: a running CA stream's surface flickers
// by several eighths visit to visit, and a 16-visit mean still wandered past
// the tolerance often enough that no channel column ever completed a run
// (measured, `stream-natural`'s fourth fixture: 6,000 ticks of a steady
// through-flow, 0 adoptions).
const STREAM_SUB : u32 = 64u;
// How far a window's mean may sit off the anchor and still be steady, eighths.
const STREAM_MEAN_TOL : i32 = 2;
// How far a column's block level must move to be news to the columns above
// (a FINE event), eighths: half a voxel.
const STREAM_CHAIN_TOL : i32 = 4;
// Windows per BLOCK: the level a column reports upstream is its mean over
// this many windows (8 x 64 = 512 visits), compared block to block.
const STREAM_BLOCK : u32 = 8u;
// How far the surface may stand off the anchor at the visit that adopts it.
const STREAM_AT_TOL : i32 = 4;
// THE REGIONS after the records (worldgen.wgsl STILL_MOUTH is the first: one
// word a column after the ledger's four).
const STREAM_MOUTH_END : u32 = WORLD_N * WORLD_N * STILL_WORDS + 4u + WORLD_N * WORLD_N;
// STILL_ECHO (render-only; raymarch.wgsl replays it): TWO words a column.
// Eight samples, byte k = sample k (word 0 bytes 0..3, word 1 bytes 4..7), each
// a SIGNED 8-bit offset in eighths of the column's resting surface (8 y + f)
// from the target it was adopted at (-127..127; -128 = no water that sample),
// taken every 4 ticks into slot (tick / 4) % 8 while the column is observed
// and not held, rebased onto the target at adoption and frozen from then on:
// an adopted column ([2] != 0) holds its last ~1 s before adoption. Zeroed on
// release, when an observation starts afresh, and by every table clear. Never
// read by the sim, not hashed.
const STILL_ECHO : u32 = STREAM_MOUTH_END;
// STREAM_OBS: EIGHT words a column, this kernel's own (and the gates'):
//   [0] the anchor (s8, 20 bits signed) | the steady run in windows << 20
//   [1] the window: sum of (s8 - anchor) (14 bits signed) | visits << 14 |
//       visits with no resting surface << 21 | wet (the column's class at the
//       last window end) bit 28 | anchored bit 29 | moved this window bit 30 |
//       moved in the last window bit 31
//   [2] the newest event stamps, own and downstream (fine | coarse << 16),
//       SCRATCH: written by `scan`
//   [3] the same, PUBLISHED: copied by `commit`; what neighbours read
//   [4] the still tag the words belong to (`commit` resets them when the slot
//       takes another world column)
//   [5] held: the tick the column was adopted. Observed: THE BLOCK -- the
//       sum of its windows' means (11 bits signed, against the anchor) |
//       windows in it << 11 (3 bits) | the previous block's mean << 14 (8 bits
//       signed, -128 = none) | ever wet bit 22 | this window's visits with
//       water in flight << 23 (7 bits) | the last window was a fall bit 30 |
//       the last window was wet through (7 of 8 visits) bit 31
//   [6] releases (bits 0..7) | adoptions (8..15) | the column's own fine
//       events (16..23) | why the last visit did not adopt it (24..31: 1 a
//       conduit action, 2 no resting surface, 3 in flight, 4 run short, 5
//       something below moved, 6 no outflow, 7 not running, 8 no held map-fed
//       feeder, 9 a POOL feeder, 10 not at its anchor, 15 adopted) --
//       diagnostic, saturating
//   [7] releases by reason, a byte each: support | flood | THE RUN'S DRIFT |
//       drained. Byte 2 (the retired event release's count) is the observer's
//       own while the column is not held: how far, signed eighths, its anchor
//       has moved since its steady run began (THE RUN IS A LEVEL, NOT A WINDOW
//       at the run's update).
// A stamp is (tick >> 4) & 0xFFFF, never 0 (0 = never).
const STREAM_OBS : u32 = STILL_ECHO + 2u * WORLD_N * WORLD_N;
// THE RUN IS A LEVEL, NOT A WINDOW: how far, eighths, a column's anchor may
// move from where its steady run began before the run starts over. Half a
// voxel: `stream-natural`'s own measure of "still filling" (a region rising
// more than half a voxel a column over the steady window).
const STREAM_RUN_DRIFT : i32 = 4;
const STREAM_REL_SUPPORT : u32 = 1u;
const STREAM_REL_FLOOD : u32 = 2u;
const STREAM_REL_EVENT : u32 = 3u;
const STREAM_REL_DRAINED : u32 = 4u;
// The pending release's reason rides in an adopted [3]'s spare bits 25..27
// between `scan` and `commit` of one tick (the action bits' neighbours).
const STREAM_REASON_SHIFT : u32 = 25u;

fn colOf(x : i32, z : i32) -> u32 { return stillIndex(x, z) / STILL_WORDS; }
fn obsAt(x : i32, z : i32) -> u32 { return STREAM_OBS + colOf(x, z) * 8u; }
fn echoAt(x : i32, z : i32) -> u32 { return STILL_ECHO + colOf(x, z) * 2u; }
fn stamp16(t : u32) -> u32 { return max((t >> 4u) & 0xFFFFu, 1u); }
// A stamp's age in 16-tick units (0xFFFF for "never").
fn stampAge(s : u32) -> u32 {
  if (s == 0u) { return 0xFFFFu; }
  return (stamp16(T.tick) - s) & 0xFFFFu;
}
fn newerStamp(a : u32, b : u32) -> u32 { return select(a, b, stampAge(b) < stampAge(a)); }
fn sdec14(v : u32) -> i32 { return bitcast<i32>(v << 18u) >> 18u; }

// The RESTING surface under this chunk's top (columnSurface's walk): the top
// cell of the first run of liquid, walking down, that stands on something
// solid or powder. A run with air or a gas under it is in FLIGHT (`flight`)
// and the walk goes on beneath it. A liquid cell above `top` is a surface in
// a chunk above, not this pass's business; a wall before any liquid is a cave.
struct Rest {
  ok     : bool,
  y      : i32,
  s8     : i32,
  mat    : u32,
  flight : bool,
  // The walk met liquid or a wall: what it says is about this column. A walk
  // that ran out in open air (the column's water lies under the look) says
  // nothing, and the observer skips the visit.
  known  : bool,
  // No resting water, but the walk met the column's FLOOR: the level of an
  // empty column there (8 x the cell over the floor), for the time-average.
  hasFloor : bool,
  floor8 : i32,
};
fn restingSurface(x : i32, top : i32, z : i32) -> Rest {
  var r : Rest;
  r.ok = false;
  r.flight = false;
  r.known = false;
  r.hasFloor = false;
  r.floor8 = 0;
  let above = vec3<i32>(x, top + 1, z);
  if (inBounds(above)) {
    let am = voxMat(voxWordAt(above));
    if (am != MAT_AIR && materials[am].klass == CLASS_LIQUID) { return r; }
  }
  // The walk: the first run of liquid's top is the surface unless that run
  // hangs over more than STREAM_STEP_GAP open cells -- water in FLIGHT, and
  // then the next run down is the candidate. A run over STREAM_STEP_GAP or
  // fewer open cells and then liquid or a floor is a STEP's spill (the lip of
  // a stream running down a staircase floor, water sliding off a one-voxel
  // riser): the stream's own surface, resting enough (measured,
  // `stream-natural`: the column after every riser of a channel falling 1 in
  // 4 read as a fall or as dry, never held, and the adoption front stopped at
  // the first of them).
  var inRun = false;
  var gap = -1;   // open cells under the last run that ended in the air (-1: none)
  for (var k = 0; k < STREAM_SCAN; k++) {
    let c = vec3<i32>(x, top - k, z);
    if (!inBounds(c)) { break; }
    let w = voxWordAt(c);
    let m = voxMat(w);
    let liquid = m != MAT_AIR && materials[m].klass == CLASS_LIQUID;
    if (liquid) {
      r.known = true;
      if (gap >= 0) {
        // Liquid again under a run that ended in the air.
        if (gap <= STREAM_STEP_GAP) { r.ok = true; return r; }   // a step: the upper run is the surface
        r.flight = true;
        gap = -1;
        inRun = false;
      }
      if (!inRun) {
        inRun = true;
        r.y = c.y;
        r.s8 = 8 * c.y + i32(voxState(w) + 1u);
        r.mat = m;
      }
      continue;
    }
    let open = m == MAT_AIR || materials[m].klass == CLASS_GAS;
    if (inRun) {
      if (open) { inRun = false; gap = 1; continue; }
      r.ok = true;   // a run standing on its bed
      return r;
    }
    if (open) {
      if (gap >= 0) { gap++; }
      continue;
    }
    // A floor (solid / powder).
    r.known = true;
    r.hasFloor = true;
    r.floor8 = 8 * (c.y + 1);
    if (gap >= 0 && gap <= STREAM_STEP_GAP) { r.ok = true; return r; }   // a step's spill onto a floor
    if (gap >= 0) { r.flight = true; }   // a fall landing on a dry ledge
    break;
  }
  if (gap >= 0) { r.flight = true; }
  // Liquid to the end of the walk: deeper than the look, resting.
  r.ok = inRun;
  return r;
}

// Rebase the column's eight echo bytes by `d` eighths (a "no water" byte
// stays). The column's own words only.
fn echoShift(ei : u32, d : i32) {
  if (d == 0) { return; }
  for (var w = 0u; w < 2u; w++) {
    let v = stillWater[ei + w];
    var o = 0u;
    for (var b = 0u; b < 4u; b++) {
      let s = bitcast<i32>((v >> (8u * b)) << 24u) >> 24;
      var n = s;
      if (s != -128) { n = clamp(s + d, -127, 127); }
      o |= (bitcast<u32>(n) & 0xFFu) << (8u * b);
    }
    if (o != v) { stillWater[ei + w] = o; }
  }
}
// One echo sample into slot (tick / 4) % 8, taken on the slot's FIRST tick
// only (a visit on the other three would only overwrite it: the recording is
// one sample every 4 ticks, 7.5 a second, as raymarch.wgsl plays it back).
// A column whose chunk slept through a slot's first tick keeps the sample
// that slot held 32 ticks earlier.
fn echoSample(ei : u32, v : i32) {
  if ((T.tick & 3u) != 0u) { return; }
  let slot = (T.tick >> 2u) & 7u;
  let wi = ei + (slot >> 2u);
  let sh = 8u * (slot & 3u);
  let old = stillWater[wi];
  let nw = (old & ~(0xFFu << sh)) | ((bitcast<u32>(v) & 0xFFu) << sh);
  if (nw != old) { stillWater[wi] = nw; }
}

// The newest event stamps among column (x, z)'s DOWNSTREAM neighbours, merged
// into `cur` (fine | coarse << 16): a lateral neighbour open at row `y` (air, a
// gas or liquid there) with no liquid at row y + 1 (nothing standing higher
// beside this surface), not the map's own water (a sink that never changes),
// whose published words belong to its current world column.
fn downstreamStamps(x : i32, y : i32, z : i32, cur : u32) -> u32 {
  var fine = cur & 0xFFFFu;
  var coarse = cur >> 16u;
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let nx = x + d.x;
    let nz = z + d.y;
    let nsi = stillIndex(nx, nz);
    let ntag = stillTag(nx, nz);
    if (stillWater[nsi] == ntag && stillWater[nsi + 1u] != 0u) { continue; }
    let a = vec3<i32>(nx, y, nz);
    if (!inBounds(a)) { continue; }
    let am = voxMat(voxWordAt(a));
    if (am != MAT_AIR && materials[am].klass != CLASS_GAS && materials[am].klass != CLASS_LIQUID) { continue; }
    let u = a + vec3<i32>(0, 1, 0);
    if (inBounds(u)) {
      let um = voxMat(voxWordAt(u));
      if (um != MAT_AIR && materials[um].klass == CLASS_LIQUID) { continue; }
    }
    let on = obsAt(nx, nz);
    if (stillWater[on + 4u] != ntag) { continue; }
    let p = stillWater[on + 3u];
    fine = newerStamp(fine, p & 0xFFFFu);
    coarse = newerStamp(coarse, p >> 16u);
  }
  return fine | (coarse << 16u);
}

struct Surf {
  ok   : bool,
  y    : i32,
  s8   : i32,
  mat  : u32,
};

// The column's surface under this chunk's top: the first liquid cell with a
// non-liquid cell above it, walking down at most STREAM_SCAN cells. `top` is
// the chunk's top cell y; a liquid cell ABOVE it means the column's surface is
// in a sleeping chunk above, which is not this pass's business.
fn columnSurface(x : i32, top : i32, z : i32) -> Surf {
  var s : Surf;
  s.ok = false;
  let above = vec3<i32>(x, top + 1, z);
  if (inBounds(above)) {
    let am = voxMat(voxWordAt(above));
    if (am != MAT_AIR && materials[am].klass == CLASS_LIQUID) { return s; }
  }
  for (var k = 0; k < STREAM_SCAN; k++) {
    let c = vec3<i32>(x, top - k, z);
    if (!inBounds(c)) { break; }
    let w = voxWordAt(c);
    let m = voxMat(w);
    if (m != MAT_AIR && materials[m].klass == CLASS_LIQUID) {
      s.ok = true;
      s.y = c.y;
      s.s8 = 8 * c.y + i32(voxState(w) + 1u);
      s.mat = m;
      return s;
    }
    // A wall over the column ends the look: whatever is under it is a cave.
    if (m != MAT_AIR && materials[m].klass != CLASS_GAS) { break; }
  }
  return s;
}

// The target a neighbour column holds for liquid `mat` and its hop (x = T8 or
// STREAM_NONE, y = hop: 0 for the map's own water, z = 1 for a POOL stream).
// The POOL bit of an adopted neighbour's [3] is constant for the whole scan
// (a pending release keeps it), so reading it while that neighbour's thread
// rewrites the word is as safe as reading its liquid.
fn neighbourTarget(x : i32, z : i32, mat : u32) -> vec3<i32> {
  let si = stillIndex(x, z);
  if (stillWater[si] != stillTag(x, z)) { return vec3<i32>(STREAM_NONE, 0, 0); }
  let a = stillWater[si + 1u];
  if (a != 0u) {
    if ((a >> 20u) == mat) { return vec3<i32>(sdec(a), 0, 0); }
    return vec3<i32>(STREAM_NONE, 0, 0);
  }
  let d = stillWater[si + 2u];
  let w3 = stillWater[si + 3u];
  if (d != 0u && (w3 & 0xFFFu) == mat) {
    return vec3<i32>(sdec(d), i32(d >> 20u), select(0, 1, (w3 & STREAM_POOL_BIT) != 0u));
  }
  return vec3<i32>(STREAM_NONE, 0, 0);
}

// A lateral neighbour column's SURFACE for liquid `mat`, searched from row
// `y` up at most STREAM_UP cells: the top cell of the first run of `mat`
// found (x = s8 = 8 y + f, or STREAM_NONE), y = that cell's row, z = 1 when
// it can POUR -- it can split into air (f >= STREAM_SPLIT_MIN) or has more of
// its liquid under it, so it is not a stranded one-eighth film. A wall above
// the start row ends the search (a cave over a ledge is not a source).
// Water deeper than the search (still `mat` at its last row) is a source
// standing at least that high, and pours.
fn sideSurface(x : i32, y : i32, z : i32, mat : u32) -> vec3<i32> {
  var top = -1;   // offset of the last `mat` cell of the run, -1 = none yet
  for (var k = 0; k < STREAM_UP; k++) {
    let c = vec3<i32>(x, y + k, z);
    if (!inBounds(c)) { break; }
    let m = voxMat(voxWordAt(c));
    if (m == mat) { top = k; continue; }
    if (top >= 0) { break; }
    if (m != MAT_AIR && materials[m].klass != CLASS_GAS) { return vec3<i32>(STREAM_NONE, 0, 0); }
  }
  if (top < 0) { return vec3<i32>(STREAM_NONE, 0, 0); }
  let t = vec3<i32>(x, y + top, z);
  let f = voxState(voxWordAt(t)) + 1u;
  var pours = f >= STREAM_SPLIT_MIN || top > 0;
  let b = t - vec3<i32>(0, 1, 0);
  if (!pours && inBounds(b) && voxMat(voxWordAt(b)) == mat) { pours = true; }
  return vec3<i32>(8 * t.y + i32(f), t.y, select(0, 1, pours));
}

// A LIVE SOURCE for a column whose surface is (or whose target is) `t8`:
// an UNHELD lateral neighbour (no target for `mat`) whose water stands
// STREAM_POUR or more over it and can pour, with an open way in (this
// column's cell at the source's top row is not a wall). `rootOnly` also asks
// that nothing stands STREAM_POUR over the SOURCE beside it (isRoot): the
// adoption test, which makes POOL adoption start at the top of the slope.
// Release asks without it (the hysteresis in the header).
fn liveSource(x : i32, y : i32, z : i32, t8 : i32, mat : u32, rootOnly : bool) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let nx = x + d.x;
    let nz = z + d.y;
    if (neighbourTarget(nx, nz, mat).x != STREAM_NONE) { continue; }
    let s = sideSurface(nx, y, nz, mat);
    if (s.x == STREAM_NONE || s.x < t8 + STREAM_POUR || s.z == 0) { continue; }
    let mc = vec3<i32>(x, s.y, z);
    if (!inBounds(mc)) { continue; }
    let mm = voxMat(voxWordAt(mc));
    if (mm != MAT_AIR && mm != mat && materials[mm].klass != CLASS_GAS) { continue; }
    if (rootOnly && (!isRoot(nx, s.y, nz, s.x, mat) || !sourceDeep(nx, s.y, nz, mat) ||
                     !sourceWide(nx, s.y, nz, mat))) { continue; }
    return true;
  }
  return false;
}

// A ROOT IS A POOL: the source column holds STREAM_ROOT_DEPTH cells of the
// liquid down from its top cell. A shallow stream has flat stretches -- a
// column with nothing STREAM_POUR over it beside it -- all along a gentle
// slope, and without this every one of them rooted a POOL stream (measured,
// `rivers` S: 131 of the lake-fed stream's 152 adopted columns came out POOL,
// so the map stream ran as a conduit and its plugged leftover was re-adopted
// and held). A pour, a pond, a tank are deep; a running film is not.
fn sourceDeep(nx : i32, ys : i32, nz : i32, mat : u32) -> bool {
  for (var k = 1; k < STREAM_ROOT_DEPTH; k++) {
    let c = vec3<i32>(nx, ys - k, nz);
    if (!inBounds(c) || voxMat(voxWordAt(c)) != mat) { return false; }
  }
  return true;
}

// ...and WIDE: at least STREAM_ROOT_WIDE of the 48 cells round it (7 x 7, its
// surface row) hold the liquid. Depth alone did not tell a pool from the deep,
// flat top of a channel a lake feeds through water nobody has adopted yet
// (measured, `rivers` S with the depth test and the 240-visit window: 56 of 120
// adopted columns POOL at tick 600, the POOL segment discharging into the map
// chain beside it, chunks awake and the flood count past 3,000). A channel 3
// wide fills 20 of the 48 at most; a pond, a tank, a pour's pool fill them.
// ...and NOT A SINK: no POOL stream's column stands among those 48. A basin a
// conduit's mouth falls into is deep and wide under the fall, and rooted
// there it became a second conduit fed by the first (measured, `stream-pool`:
// ~300 basin columns adopted, the ledger turning over ~3,000 eighths every 150
// ticks inside the basin and filling to the cap, which held the real head back
// until the source read as spent with 40% of it left).
fn sourceWide(nx : i32, ys : i32, nz : i32, mat : u32) -> bool {
  var n = 0;
  for (var dz = -3; dz <= 3; dz++) {
    for (var dx = -3; dx <= 3; dx++) {
      if (dx == 0 && dz == 0) { continue; }
      let c = vec3<i32>(nx + dx, ys, nz + dz);
      if (inBounds(c) && voxMat(voxWordAt(c)) == mat) { n++; }
      if (neighbourTarget(c.x, c.z, mat).z != 0) { return false; }
    }
  }
  return n >= STREAM_ROOT_WIDE;
}

// RETARGET (THE CONDUIT): the highest level an adopted POOL column with target
// t8 and hop `hop` is still FED at, below t8 -- a held feeder of smaller hop
// whose target has fallen under t8 (its own target, open way in), or an
// unheld source that still pours at STREAM_POUR over (its surface minus
// STREAM_POUR) and stands at or over this column's own water `curS` (so the
// water it spills into downstream is never mistaken for a feed; STREAM_NONE
// = no surface, held feeders only) -- or STREAM_NONE. Searched from a voxel
// under the target's top row (a retarget lowers by at most
// STREAM_RETARGET_MAX eighths a step).
fn lowerFeed(x : i32, z : i32, t8 : i32, mat : u32, hop : u32, curS : i32) -> i32 {
  var best = STREAM_NONE;
  let y0 = ((t8 - 1) >> 3u) - 1;
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let nx = x + d.x;
    let nz = z + d.y;
    let t = neighbourTarget(nx, nz, mat);
    var cand = STREAM_NONE;
    var row = 0;
    if (t.x != STREAM_NONE) {
      if (u32(t.y) >= hop || t.x >= t8) { continue; }
      cand = t.x;
      row = (t.x - 1) >> 3u;
    } else {
      if (curS == STREAM_NONE) { continue; }
      let sf = sideSurface(nx, y0, nz, mat);
      if (sf.x == STREAM_NONE || sf.z == 0 || sf.x < curS) { continue; }
      cand = sf.x - STREAM_POUR;
      row = sf.y;
    }
    if (cand <= best || cand >= t8 || cand <= t8 - STREAM_RETARGET_MAX) { continue; }
    let mc = vec3<i32>(x, row, z);
    if (!inBounds(mc)) { continue; }
    let mm = voxMat(voxWordAt(mc));
    if (mm != MAT_AIR && mm != mat && materials[mm].klass != CLASS_GAS) { continue; }
    best = cand;
  }
  return best;
}

// ROOT: no lateral neighbour of the source column (nx, nz), whose surface is
// sN at row ys, has `mat` standing STREAM_POUR or more over sN -- tested on
// the two rows that can hold such water next to a top cell (ys and ys + 1;
// water higher than that has water at ys + 1 too).
// NOR a held column (the map's water, an adopted stream) whose target stands
// at or over sN: a column a held body can feed is that body's, and its stream
// is adopted from the body (map-rooted, or POOL-fed), never rooted afresh.
// (A guard, not the fix for `rivers` S's POOL columns -- that was the flat
// stretches of a shallow stream; see sourceDeep.)
fn isRoot(nx : i32, ys : i32, nz : i32, sN : i32, mat : u32) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let ht = neighbourTarget(nx + d.x, nz + d.y, mat);
    if (ht.x != STREAM_NONE && ht.x >= sN) { return false; }
    for (var k = 0; k < 2; k++) {
      let c = vec3<i32>(nx + d.x, ys + k, nz + d.y);
      if (!inBounds(c)) { continue; }
      let w = voxWordAt(c);
      if (voxMat(w) == mat && 8 * c.y + i32(voxState(w) + 1u) >= sN + STREAM_POUR) { return false; }
    }
  }
  return true;
}

// SUPPORT: the smallest hop among the lateral neighbours that hold a target
// at or over `t8` (water can stand that high there) with a hop under
// `hopBelow`, and an OPEN way from that neighbour's surface into this column
// (this column's cell at that row is not a wall). Hops strictly fall toward
// the map's water, so support can never run in a circle. Returns
// STREAM_HOP_MAX + 1 when there is none. y = 1 when that best feeder is a
// POOL stream's (a tie of hop goes to the map-rooted one).
// Not "and the neighbour's water stands there now": an adopted thin film
// pours its top cell into the next, still unadopted column every substep, and
// the reservoir refills a cell only when it has nothing else to do, so that
// cell is mostly empty while the stream below it is still running -- the
// check stalled adoption a few columns in. A PLUGGED mouth is a wall in this
// column at the row (refused here), and the columns below it lose their
// support one tick later, when the plugged column's record is gone.
fn streamSupport(x : i32, z : i32, t8 : i32, mat : u32, hopBelow : u32) -> vec2<u32> {
  return streamSupportTo(x, z, t8, mat, hopBelow, STREAM_HOP_MAX);
}
// FED BY A HELD STREAM: some lateral neighbour is an adopted map-rooted
// stream column (hop >= 1, not POOL) whose target stands at or over `t8`,
// with an open way into this column at that row -- whatever else feeds it.
// THE FRONT's test (scan): the best hop alone (streamSupportTo) is 0 beside
// the map's water, and a column between a held stream and the river it runs
// into then had to pass the HEAD's tests -- a whole quiet window of its own --
// which the water running through it into the river never gave it (measured,
// `rivers` S: the held channel stopped 20 columns short of main, those
// columns' runs never past 5 windows, the 6 chunks awake for good).
fn fedByHeldStream(x : i32, z : i32, t8 : i32, mat : u32) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let t = neighbourTarget(x + d.x, z + d.y, mat);
    if (t.x == STREAM_NONE || t.x < t8 || t.y < 1 || u32(t.y) >= STREAM_HOP_MAX || t.z != 0) { continue; }
    let mc = vec3<i32>(x, (t.x - 1) >> 3u, z);
    if (!inBounds(mc)) { continue; }
    let mm = voxMat(voxWordAt(mc));
    if (mm != MAT_AIR && mm != mat && materials[mm].klass != CLASS_GAS) { continue; }
    return true;
  }
  return false;
}

// streamSupport with `poolStepMax`: a POOL feeder counts only if its target
// stands at most this many eighths over t8. A FALL ENDS A CONDUIT: the basin
// under a waterfall at a POOL stream's mouth is "fed" by a target ten voxels
// over it, and adopted (a bowl with a mound under the fall has local slopes,
// so it passes the strict outflow too) it held the mouth's water: the mouth
// emitted into its own sink and the conduit stalled at the cap (measured,
// `stream-pool`: ~105 basin columns POOL-adopted). Below a fall the water is
// plain water, as a falling sheet should be.
fn streamSupportTo(x : i32, z : i32, t8 : i32, mat : u32, hopBelow : u32, poolStepMax : u32) -> vec2<u32> {
  var best = STREAM_HOP_MAX + 1u;
  var pool = 0u;
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let nx = x + d.x;
    let nz = z + d.y;
    let t = neighbourTarget(nx, nz, mat);
    if (t.x == STREAM_NONE || t.x < t8) { continue; }
    let hop = u32(t.y);
    let np = u32(t.z);
    if (np != 0u && t.x > t8 + i32(poolStepMax)) { continue; }
    // Smaller hop wins; an equal hop replaces a POOL best with a map one.
    if (hop >= hopBelow || hop > best || (hop == best && (np != 0u || pool == 0u))) { continue; }
    let yt = (t.x - 1) >> 3u;
    let mc = vec3<i32>(x, yt, z);
    if (!inBounds(mc)) { continue; }
    let mm = voxMat(voxWordAt(mc));
    if (mm != MAT_AIR && mm != mat && materials[mm].klass != CLASS_GAS) { continue; }
    best = hop;
    pool = np;
  }
  return vec2<u32>(best, pool);
}

// OUTFLOW: this column's surface cell has somewhere LOWER to go beside it --
// a lateral neighbour that is air at the surface row, or liquid standing at
// least two eighths lower. A stream always has one (it runs downhill); a pool
// being filled has none (it is a bowl), so a filling pool is never frozen part
// way up: it fills to its feeder's level and sleeps as plain water.
// `strict` (a POOL candidate, THE CONDUIT): a held neighbour counts only if
// its target stands STREAM_POUR or more UNDER this surface. A flat pool beside
// a held column at its own level otherwise has both a feed (T >= s8) and an
// outflow (T <= s8), and the short POOL window flood-filled the SOURCE from
// the stream's head (measured, `stream-pool`: 319 reservoir columns adopted,
// the source frozen, the conduit stalled at the cap). A stream has somewhere
// lower to go; a pool has it only at its edge.
fn streamOutflow(x : i32, y : i32, z : i32, s8 : i32, mat : u32, strict : bool) -> bool {
  for (var i = 0u; i < 4u; i++) {
    var d = vec2<i32>(1, 0);
    if (i == 1u) { d = vec2<i32>(-1, 0); }
    else if (i == 2u) { d = vec2<i32>(0, 1); }
    else if (i == 3u) { d = vec2<i32>(0, -1); }
    let n = vec3<i32>(x + d.x, y, z + d.y);
    if (!inBounds(n)) { continue; }
    let w = voxWordAt(n);
    let m = voxMat(w);
    if (m == MAT_AIR) { return true; }
    if (m == mat && 8 * y + i32(voxState(w) + 1u) + 2 <= s8) { return true; }
    let t = neighbourTarget(n.x, n.z, mat);
    if (t.x != STREAM_NONE && t.x <= s8 - select(0, STREAM_POUR, strict)) { return true; }
  }
  return false;
}

// Is this workgroup's chunk the TOPMOST dirty chunk of its window stack?
// Thread k (1 <= k < NCHUNK) asks about the k-th window row above it, in
// world order (slot row (cy + k) & mask, up to the row that holds the
// window's top), and ORs the answer into the workgroup's word: one dirtyIn
// latency, not up to NCHUNK - 1 of them in a row on one thread while the
// other 255 wait. An OR is order-free, so the answer does not depend on which
// thread runs first. Called by EVERY thread of the workgroup, in uniform
// control flow (it holds two barriers). A ticket slot (>= NUM_CHUNKS: no
// window column) is never the top.
var<workgroup> wgAbove : atomic<u32>;
fn stackTop(centre : u32, li : u32) -> bool {
  if (li == 0u) { atomicStore(&wgAbove, 0u); }
  workgroupBarrier();
  if (centre < NUM_CHUNKS && li >= 1u) {
    let cy = (centre / NCHUNK) % NCHUNK;
    let cz = centre / (NCHUNK * NCHUNK);
    let cx = centre % NCHUNK;
    let oy = bitcast<u32>(T.origin.y & i32(NCHUNK_MASK));
    let rowsAbove = (oy + NCHUNK - 1u - cy) & u32(NCHUNK_MASK);
    if (li <= rowsAbove) {
      let sy = (cy + li) & u32(NCHUNK_MASK);
      if (dirtyIn[(cz * NCHUNK + sy) * NCHUNK + cx] != 0u) { atomicOr(&wgAbove, 1u); }
    }
  }
  workgroupBarrier();
  return centre < NUM_CHUNKS && atomicLoad(&wgAbove) == 0u;
}

@compute @workgroup_size(256)
fn scan(@builtin(workgroup_id) wg : vec3<u32>, @builtin(local_invocation_index) li : u32) {
  let centre = dirtyList[wg.x];
  // Every thread asks (the barriers in stackTop), then the not-top return.
  if (!stackTop(centre, li) || T.stillWaterRelax == 0u) { return; }
  let wc = slotWorldChunk(centre, T.origin);
  let x = wc.x * i32(CHUNK) + i32(li & 15u);
  let z = wc.z * i32(CHUNK) + i32(li >> 4u);
  let top = wc.y * i32(CHUNK) + i32(CHUNK) - 1;
  let si = stillIndex(x, z);
  // A column with no record at all (never generated into this slot) or one
  // the map already holds is not a stream.
  if (stillWater[si] != stillTag(x, z) || stillWater[si + 1u] != 0u) { return; }
  let dyn = stillWater[si + 2u];
  let old3 = stillWater[si + 3u];
  let w3 = old3 & 0x3FFFFFFFu;
  var word = 0u;
  // STREAMS ADOPT ONLY WHEN NATURAL: this column's observer words. A slot that
  // took another world column starts from nothing (`commit` writes the tag).
  let oi = obsAt(x, z);
  let ei = echoAt(x, z);
  let fresh = stillWater[oi + 4u] != stillTag(x, z);
  let old0 = select(stillWater[oi], 0u, fresh);
  let old1 = select(stillWater[oi + 1u], 0u, fresh);
  let old2 = select(stillWater[oi + 2u], 0u, fresh);
  let old5 = select(stillWater[oi + 5u], 0u, fresh);
  var o0 = old0;
  var o1 = old1;
  var o2 = old2;
  var o5 = old5;
  let r = restingSurface(x, top, z);
  if (dyn != 0u) {
    // ADOPTED: still supported at its own target by a neighbour nearer the
    // map's water (its surface may be under or over the target right now --
    // the reservoir's business), and not drowned (the flood count), or
    // released.
    let mat = w3 & 0xFFFu;
    let pool = w3 & STREAM_POOL_BIT;
    let t8 = sdec(dyn);
    var over = (w3 >> STREAM_OVER_SHIFT) & STREAM_OVER_MAX;
    let s = columnSurface(x, top, z);
    if (s.ok && s.mat == mat && s.s8 > t8) {
      // A CAPPED head (THE CONDUIT: a POOL column waiting over its target for
      // the mouth to draw the ledger down) counts +1, never the deep +16.
      let deep = s.s8 > t8 + 8 && !(pool != 0u && ledgerSnap() >= STREAM_LEDGER_CAP);
      over = min(over + select(1u, STREAM_FLOOD_DEEP, deep), STREAM_OVER_MAX);
    } else if (pool != 0u && ledgerSnap() <= 0 && (!s.ok || s.s8 < t8)) {
      // STARVED (the conduit's mouth waiting on an empty ledger): counted
      // like a flood, so a mouth whose source has stopped pouring is let go.
      over = min(over + 1u, STREAM_OVER_MAX);
    } else if (over > 0u) {
      over = over - 1u;
    }
    var held = over < STREAM_FLOOD_COUNT &&
               streamSupport(x, z, t8, mat, dyn >> 20u).x <= STREAM_HOP_MAX;
    // A POOL column with no held feeder nearer the source stays while unheld
    // water still pours into it -- it is the stream's head now (the
    // hysteresis: no root test here, see THE CONDUIT).
    if (!held && pool != 0u && over < STREAM_FLOOD_COUNT) {
      held = liveSource(x, (t8 - 1) >> 3u, z, t8, mat, false);
    }
    // Why it goes, if it goes (commit counts it, STREAM_OBS [7]).
    var reason = select(STREAM_REL_SUPPORT, STREAM_REL_FLOOD, over >= STREAM_FLOOD_COUNT);
    // A MAP-fed column (NATURAL, header): the newest events below it, and
    // let go when the place it delivers to changed after it was held (a
    // coarse event newer than its adoption) or its own water was drawn more
    // than a voxel under the target (DRAINED). Not a POOL column: the
    // conduit's basin fills by design, and its mouth has its own STARVED rule.
    if (held && pool == 0u) {
      let yt = (t8 - 1) >> 3u;
      // The stamps still pass THROUGH a held column (the columns above it
      // that are not held yet must hear what happens below it).
      o2 = downstreamStamps(x, yt, z, o2);
      // DRAINED: its water a voxel under the target -- or none, its floor a
      // voxel under it: something below took more than the stream brings (a
      // dig, a drain), or the stream above stopped feeding it (a step of a
      // chute the front reached between packets). From here the CA has it.
      // (No release on a mere event below: a held map-fed stream is not
      // refilled from the reservoir -- sim_step.wgsl stillWant, A MAP-FED
      // STREAM IS NOT REFILLED -- so it already delivers into a dig or a
      // draining basin exactly as the live stream would, by its water
      // running down through it. Released on the first new wet column below
      // it as the first cut was, a held stream let go whenever a cascade's
      // side lane got wet for the first time after thousands of ticks --
      // measured, `stream-natural`: 238 columns held, all released 100
      // ticks later, by two outlet columns of an established chute.)
      let drained = (r.ok && r.mat == mat && r.s8 < t8 - 8) || (r.known && !r.ok && r.hasFloor && r.floor8 < t8 - 8);
      if (drained && yt <= top) {
        held = false;
        reason = STREAM_REL_DRAINED;
      }
    }
    // ...and when what feeds it now stands LOWER -- the pool it drains has
    // fallen, or the column above was lowered -- it is RETARGETED to that
    // level rather than released (THE CONDUIT, RETARGET). Released, its
    // water drained into the next column and that one lost its feed too: the
    // whole stream let go from the top every time the source fell two
    // eighths, and re-adopted 120 visits later (measured, `stream-pool`: 209,
    // 134, 99, 15, 49, 115, 205 ... columns adopted, the middle awake through
    // every cycle). The pending word keeps the liquid and the POOL bit where
    // a neighbour reads them; the drop rides in the flood-count bits, which
    // nobody else reads, and `commit` lowers [2] by it.
    if (!held && pool != 0u && over < STREAM_FLOOD_COUNT) {
      let lf = lowerFeed(x, z, t8, mat, dyn >> 20u, select(STREAM_NONE, s.s8, s.ok && s.mat == mat));
      if (lf != STREAM_NONE) {
        word = mat | pool | (u32(t8 - lf) << STREAM_OVER_SHIFT) | (STREAM_ACT_ADOPT_POOL << 30u);
      } else {
        word = mat | pool | (over << STREAM_OVER_SHIFT) | (reason << STREAM_REASON_SHIFT) | (STREAM_ACT_RELEASE << 30u);
      }
    } else {
      word = mat | pool | (over << STREAM_OVER_SHIFT) |
             select((reason << STREAM_REASON_SHIFT) | (STREAM_ACT_RELEASE << 30u), 0u, held);
    }
    // WHERE ITS WATER STOOD WHEN IT WAS LET GO (diagnostic, STREAM_OBS [7]
    // byte 2, which is the observer's run drift only while the column is
    // observed -- a fresh anchor zeroes it): its resting surface minus its
    // target, eighths, clamped to +-63, bit 7 set when it had none (then the
    // floor's level minus the target; -64 = neither). `rivers` S prints the
    // lake-most released column's [7].
    if ((word >> 30u) == STREAM_ACT_RELEASE && pool == 0u) {
      var d = -64;
      if (r.ok) { d = clamp(r.s8 - t8, -63, 63); }
      else if (r.hasFloor) { d = clamp(r.floor8 - t8, -63, 63); }
      let b = (bitcast<u32>(d) & 0x7Fu) | select(0x80u, 0u, r.ok);
      let o7 = stillWater[oi + 7u];
      stillWater[oi + 7u] = (o7 & 0xFF00FFFFu) | (b << 16u);
    }
  } else {
    // THE CONDUIT's own observation ([3]: surface | age), for POOL streams
    // only -- with sim.streamPoolRoot off no POOL column can exist, and the
    // word stays 0 (the map's streams are observed in STREAM_OBS).
    var age = (w3 >> 20u) & STREAM_AGE_MAX;
    var s : Surf;
    s.ok = false;
    if (streamPoolRoot()) { s = columnSurface(x, top, z); }
    if (!streamPoolRoot()) {
      word = 0u;
    } else if (!s.ok) {
      word = (w3 & 0xFFFFFu) | ((age >> 1u) << 20u);
    } else {
      age = min(age + 1u, STREAM_AGE_MAX);   // this visit; reset below on a jump
      // The band's centre stays where the count began; a jump out of it
      // restarts the count from here.
      var obs = s.s8;
      if (age > 0u && abs(s.s8 - sdec(w3)) <= 8) { obs = sdec(w3); } else { age = 0u; }
      word = (bitcast<u32>(obs) & 0xFFFFFu) | (age << 20u);
      if (age >= STREAM_ADOPT_TICKS_POOL && streamOutflow(x, s.y, z, s.s8, s.mat, false)) {
        let sup = streamSupportTo(x, z, s.s8, s.mat, STREAM_HOP_MAX, u32(STREAM_RETARGET_MAX));
        var hop = sup.x + 1u;
        var act = select(STREAM_ACT_ADOPT, STREAM_ACT_ADOPT_POOL, sup.y != 0u);
        // A column fed by a held POOL column needs only the short window (and
        // a STRICT outflow); a MAP-fed one is adopted only by THE NATURAL
        // TEST below.
        if (hop <= STREAM_HOP_MAX && sup.y == 0u) { hop = STREAM_HOP_MAX + 1u; }
        else if (hop <= STREAM_HOP_MAX && sup.y != 0u && !streamOutflow(x, s.y, z, s.s8, s.mat, true)) {
          hop = STREAM_HOP_MAX + 1u;
        }
        // No held feeder: a live source at the top of a slope roots a POOL
        // stream (THE CONDUIT) -- after the ROOT window, so a map stream
        // being adopted down the same water always gets there first.
        else if (hop > STREAM_HOP_MAX && streamPoolRoot() && age >= STREAM_ADOPT_TICKS_ROOT &&
                 streamOutflow(x, s.y, z, s.s8, s.mat, true) && liveSource(x, s.y, z, s.s8, s.mat, true)) {
          hop = 1u;
          act = STREAM_ACT_ADOPT_POOL;
        }
        if (hop <= STREAM_HOP_MAX) {
          word = (bitcast<u32>(s.s8) & 0xFFFFFu) | (hop << 20u) | (act << 30u);
        }
      }
    }
    // ---- THE NATURAL TEST (header): observe the resting surface ----
    var anchor = sdec(o0);
    var run = o0 >> 20u;
    var acc = sdec14(o1 & 0x3FFFu);
    var k = (o1 >> 14u) & 127u;
    var dry = (o1 >> 21u) & 127u;
    var wet = (o1 >> 28u) & 1u;
    var anch = (o1 >> 29u) & 1u;
    var moved = (o1 >> 30u) & 1u;
    var movedLast = o1 >> 31u;
    // The BLOCK (o5, header): the sum of the last few windows' means and how
    // many, the previous block's mean (both relative to the anchor; -128 =
    // none yet), ever wet, this window's visits in flight.
    var bSum = bitcast<i32>(o5 << 21u) >> 21;
    var bN = (o5 >> 11u) & 7u;
    var bPrev = bitcast<i32>(o5 << 10u) >> 24;
    var ever = (o5 >> 22u) & 1u;
    var fl = (o5 >> 23u) & 127u;
    var fallLast = (o5 >> 30u) & 1u;
    var wetLast = o5 >> 31u;
    // THE RUN'S DRIFT (STREAM_OBS [7] byte 2; THE RUN IS A LEVEL, NOT A
    // WINDOW, below). A slot that took another world column starts from 0
    // (`commit` clears [7] after this scan).
    let old7 = select(stillWater[oi + 7u], 0u, fresh);
    var drift = bitcast<i32>(((old7 >> 16u) & 0xFFu) << 24u) >> 24;
    // A column that is wet, or was: a visit whose walk ran out in open air
    // says nothing about it.
    if (r.known && (r.ok || anch != 0u || wet != 0u)) {
      if (r.ok && anch == 0u) {
        // A fresh observation: anchored here, its echo started over.
        anchor = r.s8;
        anch = 1u;
        acc = 0;
        bSum = 0;
        bN = 0u;
        bPrev = -128;
        drift = 0;
        if (stillWater[ei] != 0u) { stillWater[ei] = 0u; }
        if (stillWater[ei + 1u] != 0u) { stillWater[ei + 1u] = 0u; }
      }
      k++;
      // The level this visit: the water's surface, or -- the column empty --
      // its floor. The window's mean is the TIME-AVERAGE level, dry visits
      // included: a stream's thin brink, a cascade's step, a film between
      // packets is held at the water it holds on average, not at the crest
      // of the packets that wet it (measured, `stream-natural`: the mean of
      // the wet visits alone held a chute's steps at their packets' height,
      // four times the water the live chute held, stood them up as pillars
      // and flooded them out by the hundred).
      if (r.ok) { acc += clamp(r.s8 - anchor, -127, 127); }
      else {
        dry++;
        if (r.hasFloor) { acc += clamp(r.floor8 - anchor, -127, 127); }
      }
      if (r.flight) { fl++; }
      if (!r.ok || r.flight || r.s8 != anchor) { moved = 1u; }
      var fine = o2 & 0xFFFFu;
      var coarse = o2 >> 16u;
      if (k >= STREAM_SUB) {
        // The window's class, by a 3/4 majority (a flickering edge keeps its
        // class), and its mean against the anchor.
        var nowWet = wet;
        if (dry * 4u <= STREAM_SUB) { nowWet = 1u; } else if (dry * 4u >= 3u * STREAM_SUB) { nowWet = 0u; }
        // NEW AREA is the column's FIRST wet window; after that its class
        // changes quietly -- the film over a basin's lip, the pool under a
        // cascade, a stream's thin edge come and go without the water going
        // anywhere new (measured, `stream-natural`: the outlet's sill column
        // alone kept a steady through-flow from ever being held, an event
        // every 500-1,000 ticks). A column that drains gets there by its
        // level falling, which the blocks below report.
        var ev = nowWet == 1u && wet == 0u && ever == 0u;
        var cev = ev;
        if (nowWet == 1u) { ever = 1u; }
        // Steady: wet, and resting most visits -- a quarter of them dry or in
        // flight is still the stream it was: a running stream's film breaks
        // now and then, and the BRINK column before every riser of a stepped
        // channel spills itself dry between packets (measured,
        // `stream-natural`: the brink columns of a 1-in-4 channel were dry an
        // eighth of their visits or more and never steady, and the adoption
        // front stopped at the first of them).
        let steady = nowWet == 1u && dry * 4u <= STREAM_SUB && fl * 4u <= STREAM_SUB;
        let wn = i32(STREAM_SUB);
        var moveOwn = false;
        var wMean = 0;
        if (nowWet == 1u && wn > 0) { wMean = acc / wn; }
        if (nowWet == 1u && wn > 0 && abs(acc) > STREAM_MEAN_TOL * wn) {
          // Re-anchored: the anchor follows the mean (it is what the column
          // is held at, and what the echo and the block are relative to); the
          // block's numbers are re-expressed against the new anchor, and the
          // run's drift counts the move (THE RUN IS A LEVEL, below).
          moveOwn = true;
          anchor += wMean;
          drift = clamp(drift + wMean, -127, 127);
          echoShift(ei, -wMean);
          bSum -= wMean * i32(bN);
          if (bPrev != -128) { bPrev = clamp(bPrev - wMean, -127, 127); }
          wMean = 0;
        }
        // THE BLOCK: what the column tells the columns above it is its level
        // over STREAM_BLOCK windows against the block before -- a stream's
        // own wander (several eighths over a few hundred ticks, measured:
        // single-window tests made an event somewhere in a steady
        // through-flow every 50-200 ticks, so nothing above it was ever
        // still) averages out; a basin filling or draining half a voxel a
        // block does not. A window with water in flight does not count (the
        // pool under a cascade rises and falls with every packet; a cascade
        // still RELAYS what happens under it, downstreamStamps).
        if (nowWet == 1u && fl == 0u) {
          bSum = clamp(bSum + wMean, -1023, 1023);
          bN++;
          if (bN >= STREAM_BLOCK) {
            let bMean = bSum / i32(bN);
            if (bPrev != -128) {
              if (abs(bMean - bPrev) >= STREAM_CHAIN_TOL) { ev = true; }
            }
            bPrev = clamp(bMean, -127, 127);
            bSum = 0;
            bN = 0u;
          }
        }
        if (ev) {
          fine = stamp16(T.tick);
          // the diagnostic count, STREAM_OBS [6] bits 16..23
          let o6e = stillWater[oi + 6u];
          stillWater[oi + 6u] = (o6e & 0xFF00FFFFu) | (min(((o6e >> 16u) & 0xFFu) + 1u, 255u) << 16u);
        }
        if (cev) { coarse = stamp16(T.tick); }
        // THE RUN IS A LEVEL, NOT A WINDOW (2026-10-18). The run went on while
        // no window's mean stood more than STREAM_MEAN_TOL off the anchor:
        // every re-anchor restarted it. A stream the MPM seam churns -- the
        // game's default, sim.fluidExciteMode 1: water spilling over every
        // riser excited, its particles waking the water round them -- has
        // window means a few eighths either side of where it stands, up and
        // down, for as long as it runs. MEASURED, `rivers` S (a channel dug
        // from a map lake to a river, MPM on): 30-38k eighths of the stream's
        // own water through the seam every 150 ticks, its volume 1,500 to
        // 2,700 eighths between samples, nothing downstream of its head moving
        // for 1,400 ticks at a time -- and the head's run 0 to 4 windows for
        // 3,000 ticks, never the 10 the window needs; 0 of 153 columns held.
        // Now a re-anchor ends the run only once the anchor has DRIFTED more
        // than STREAM_RUN_DRIFT from where the run began: a level wandering
        // either side of its place keeps its run, one that goes somewhere --
        // a basin rising half a voxel a column, `stream-natural`'s own measure
        // of still filling -- restarts it as before. (STREAM_MEAN_TOL still
        // moves the anchor: it is the target the column is held at.)
        let drifted = abs(drift) > STREAM_RUN_DRIFT;
        run = select(0u, min(run + 1u, 4095u), steady && !ev && !drifted);
        if (run == 0u) { drift = 0; }
        if (nowWet == 0u) { anch = 0u; }
        wet = nowWet;
        // WET THROUGH: an eighth of the window's visits dry at most
        // (diagnostic: the gate prints it). Before the window's counts reset.
        wetLast = select(0u, 1u, dry * 8u <= STREAM_SUB);
        k = 0u;
        acc = 0;
        dry = 0u;
        // A FALL: more than a quarter of the window's visits found water in
        // flight (a cascade, a chute). The front does not go into one.
        fallLast = select(0u, 1u, fl * 4u > STREAM_SUB);
        fl = 0u;
        movedLast = moved;
        moved = 0u;
      }
      if (anch != 0u) { echoSample(ei, select(-128, clamp(r.s8 - anchor, -127, 127), r.ok)); }
      o2 = fine | (coarse << 16u);
      if (r.ok) { o2 = downstreamStamps(x, r.y, z, o2); }
      o0 = (bitcast<u32>(anchor) & 0xFFFFFu) | (run << 20u);
      o1 = (bitcast<u32>(acc) & 0x3FFFu) | (k << 14u) | (dry << 21u) | (wet << 28u) | (anch << 29u) | (moved << 30u) |
           (movedLast << 31u);
      o5 = (bitcast<u32>(bSum) & 0x7FFu) | (bN << 11u) | ((bitcast<u32>(bPrev) & 0xFFu) << 14u) | (ever << 22u) |
           (fl << 23u) | (fallLast << 30u) | (wetLast << 31u);
      // ...and the run's drift, [7] byte 2 (only a changed word is stored).
      let n7 = (old7 & 0xFF00FFFFu) | ((bitcast<u32>(drift) & 0xFFu) << 16u);
      if (n7 != old7) { stillWater[oi + 7u] = n7; }
      // HELD when natural: steady for the window, resting, nothing below it
      // moved for as long, standing at its anchor now, running, and fed by
      // the map's water (hop 0) or a map-fed held column. RUNNING, not just
      // touching held water at its own level: a held feeder stands HIGHER, or
      // somewhere beside it is LOWER, or its surface moved in the last window
      // (water passing through a level stretch -- a stream's mouth at the
      // river's own level). A still pool beside the lake has none of the three
      // and sleeps as the plain water it is; phase 2 held it (fed by the lake
      // at its level, "draining" into the lake at its level).
      let steadyT = streamSteadyTicks();
      // THE TARGET is the surface the column stands at THIS visit -- taken
      // only at a visit within STREAM_AT_TOL (half a voxel) of the anchor, its
      // mean, so what is frozen is the shape it had on average and not a
      // crest, and the water is AT its target the moment it is held (nothing
      // to fill, nothing over). Held at the anchor itself instead, every
      // column stood a few eighths off its target: the refills and the excess
      // moved between held neighbours as a wave down the held stream that
      // never slept (measured, `stream-natural`: the fully held channel's 5
      // chunks awake, its top cells trading eighths with their neighbours).
      // A top cell of ONE eighth is held at TWO: a one-eighth film at rest
      // steps sideways into a held neighbour still filling to the same
      // target, that one fills, the film steps back -- the same dance
      // (measured: every move a one-eighth film between two columns held at
      // y74 + 1). Two eighths can split and does not step. (Not rounded DOWN:
      // a column whose only water was that film was then held at its floor --
      // dry -- and a held column at or over its target takes no water from
      // its neighbours: it dammed the stream, and the chute below it ran dry,
      // measured.)
      var tgt = select(anchor, r.s8, r.ok);
      if (tgt - 8 * ((tgt - 1) >> 3u) == 1) { tgt += 1; }
      let ty = (tgt - 1) >> 3u;
      // WHY NOT (STREAM_OBS [6] bits 24..31, diagnostic: the first test this
      // visit failed, 15 = adopted -- what `stream-natural`'s lane prints).
      var why = 0u;
      if ((word >> 30u) != 0u) { why = 1u; }
      else if (!r.ok) { why = 2u; }
      else if (r.flight) { why = 3u; }
      else {
        let sup = streamSupportTo(x, z, tgt, r.mat, STREAM_HOP_MAX, u32(STREAM_RETARGET_MAX));
        let quiet = stampAge(o2 & 0xFFFFu) * 16u >= steadyT;
        // THE FRONT DOES NOT WAIT. The HEAD of a stream (fed by the map's own
        // water, hop 0) must have held its own surface for the whole window,
        // run (somewhere lower beside it, a held feeder higher, or a moving
        // surface) and stand within STREAM_AT_TOL of its anchor at this visit.
        // A column fed by a HELD stream column (hop >= 1) -- the adoption
        // front going down water whose every column, and everything below,
        // was quiet for the window (the stamps above) -- is taken at once at
        // its anchor, and so is the still water the front reaches (a basin
        // the stream runs through). Measured, `stream-natural`: requiring the
        // front's columns to pass the head's tests stalled it at the first
        // BRINK of a stepped channel (a film spilling over a riser, its mean
        // wandering past 2 eighths every few windows), at every column just
        // below it (a held column stands at its mean all the time where the
        // live one only averaged it, so it hands the column below more than
        // the live stream did and resets that column's run) and at the edge
        // of the basin's level water (not "running"); and a held stream that
        // stops at an UNHELD pool over-feeds it -- a held column has no
        // resistance behind it, the live channel had 30 columns of it -- so
        // the pool rose, backed up into the held columns over their targets
        // and the flood count let them all go (87 + 26 held, then 0). Held
        // to its outlet, a stream hands its over-supply on to whatever runs
        // away below it (a cascade, the map's water) instead. NOT into a fall
        // (a window more than a quarter in flight): a cascade held at its
        // pools' mean stood its steps over each other as pillars, and its
        // held columns flooded and were let go by the hundred (measured: an
        // outlet chute, 585 releases).
        // ...and not a column that has been LET GO before (STREAM_OBS [6]'s
        // release count): whatever let it go -- a flood running through it,
        // a drain under it, the column above it released -- taken at once
        // again it would only go again, a tick later (measured,
        // `stream-natural`: once a dug extension drained the held basin, its
        // columns were released and re-taken by the front 28,000 times in 900
        // ticks; before that, an outlet's chute 438 adoptions and 168 releases
        // in 2,500). It can still be held as a head is, after a whole window
        // of quiet.
        let floodedBefore = (stillWater[oi + 6u] & 0xFFu) != 0u;
        // The front does not ask the stamps again: the head it came from was
        // held only once everything below it had been quiet for the window,
        // and what moved below since is the stream being held -- a held
        // stream delivers through its own water, not quite at the live rate,
        // and the cascade below a stream's last pool shifts with that.
        // Waiting on it, the front stopped at a basin's sill for good and the
        // held channel and basin above ran as a conveyor into the live chute
        // below for ever (measured, `stream-natural`: 5 channel chunks awake).
        // FED BY A HELD STREAM COLUMN, whatever else feeds it (2026-10-18;
        // fedByHeldStream): the smallest feeding hop is 0 for a column beside
        // the map's water as well, and as `sup.x >= 1` the front stopped there
        // -- the mouth of a stream at the river it runs into had to pass the
        // head's tests, a whole quiet window of its own that the water
        // running through it into the river never gave it (measured, `rivers`
        // S: the held channel stopped 20 columns short of main, those
        // columns' runs never past 5 windows, its chunks awake for good).
        let front = fedByHeldStream(x, z, tgt, r.mat) && sup.x < STREAM_HOP_MAX && sup.y == 0u && fallLast == 0u &&
                    !floodedBefore;
        if (front && abs(r.s8 - anchor) > STREAM_AT_TOL) { why = 10u; }
        else if (!front && !quiet) { why = 5u; }
        else if (front) {
          why = 15u;
          // Held at its anchor (the echo is relative to it; rebased onto the
          // target when that is the full cell under a one-eighth top).
          word = (bitcast<u32>(tgt) & 0xFFFFFu) | ((sup.x + 1u) << 20u) | (STREAM_ACT_ADOPT << 30u);
          echoShift(ei, anchor - tgt);
        } else if (run * STREAM_SUB < steadyT) { why = 4u; }
        else if (!streamOutflow(x, ty, z, tgt, r.mat, false)) { why = 6u; }
        else {
          let hi = streamSupportTo(x, z, tgt + 1, r.mat, STREAM_HOP_MAX, u32(STREAM_RETARGET_MAX));
          let runs = movedLast != 0u || streamOutflow(x, ty, z, tgt - 1, r.mat, false) ||
                     (hi.x <= STREAM_HOP_MAX && hi.y == 0u);
          if (!runs) { why = 7u; }
          else if (sup.x >= STREAM_HOP_MAX) { why = 8u; }
          else if (sup.y != 0u) { why = 9u; }
          else if (abs(r.s8 - anchor) > STREAM_AT_TOL) { why = 10u; }
          else {
            why = 15u;
            word = (bitcast<u32>(tgt) & 0xFFFFFu) | ((sup.x + 1u) << 20u) | (STREAM_ACT_ADOPT << 30u);
            echoShift(ei, anchor - tgt);
          }
        }
      }
      let o6w = stillWater[oi + 6u];
      let n6w = (o6w & 0x00FFFFFFu) | (why << 24u);
      if (n6w != o6w) { stillWater[oi + 6u] = n6w; }
    }
  }
  if (o0 != old0 || fresh) { stillWater[oi] = o0; }
  if (o1 != old1 || fresh) { stillWater[oi + 1u] = o1; }
  if (o2 != old2 || fresh) { stillWater[oi + 2u] = o2; }
  // [5] is the observer's only while the column is not held (`commit` writes
  // the adoption tick there).
  if (dyn == 0u && (o5 != old5 || fresh)) { stillWater[oi + 5u] = o5; }
  // Only a CHANGED word is stored: every dry column of every topmost dirty
  // chunk passes through here each tick, and almost all of them hold 0 and
  // stay 0. (No pending action is ever left standing in an unchanged word:
  // `commit` clears every action it reads.)
  if (word != old3) { stillWater[si + 3u] = word; }
}

@compute @workgroup_size(256)
fn commit(@builtin(workgroup_id) wg : vec3<u32>, @builtin(local_invocation_index) li : u32) {
  let centre = dirtyList[wg.x];
  // THE LEDGER'S SNAPSHOT (THE CONDUIT): the balance the CA's adds left this
  // tick, for the next tick's decisions. One thread, after the barrier that
  // ends the CA rows (pass_table.def streamScan / streamCommit); nothing in
  // this tick reads the snapshot after this (`scan` ran before it). Every
  // tick the CA ran has a dirty list, so this runs; a tick with none added
  // nothing, so the snapshot it keeps is still the balance.
  if (wg.x == 0u && li == 0u) { stillWater[STREAM_LEDGER + 1u] = stillWater[STREAM_LEDGER]; }
  if (!stackTop(centre, li) || T.stillWaterRelax == 0u) { return; }
  let wc = slotWorldChunk(centre, T.origin);
  let x = wc.x * i32(CHUNK) + i32(li & 15u);
  let z = wc.z * i32(CHUNK) + i32(li >> 4u);
  let si = stillIndex(x, z);
  if (stillWater[si] != stillTag(x, z) || stillWater[si + 1u] != 0u) { return; }
  let w3 = stillWater[si + 3u];
  let act = w3 >> 30u;
  // THE NATURAL TEST's words: the tag they belong to (a slot that took
  // another world column resets the column's adoption tick and counters; its
  // scan has already started the observation over), and the stamps `scan`
  // merged, PUBLISHED for the neighbours' next scan.
  let oi = obsAt(x, z);
  let ei = echoAt(x, z);
  let tag = stillTag(x, z);
  if (stillWater[oi + 4u] != tag) {
    stillWater[oi + 4u] = tag;
    stillWater[oi + 5u] = 0u;
    stillWater[oi + 6u] = 0u;
    stillWater[oi + 7u] = 0u;
  }
  if (act == STREAM_ACT_RELEASE) {
    // Counted (STREAM_OBS [6] / [7]); the observation starts over (a whole
    // window again before it can be held: no cycle faster than that), its
    // echo is gone, and the release is a FINE event for the columns above.
    let o6 = stillWater[oi + 6u];
    stillWater[oi + 6u] = (o6 & 0xFFFFFF00u) | min((o6 & 0xFFu) + 1u, 0xFFu);
    let reason = (w3 >> STREAM_REASON_SHIFT) & 7u;
    if (reason >= 1u && reason <= 4u) {
      let sh = 8u * (reason - 1u);
      let o7 = stillWater[oi + 7u];
      stillWater[oi + 7u] = (o7 & ~(0xFFu << sh)) | (min(((o7 >> sh) & 0xFFu) + 1u, 255u) << sh);
    }
    // ...as a WET column that has been wet (o1 wet bit, o5 ever-wet bit): its
    // own water running on is not a new area, so the release does not read as
    // a COARSE event to the held columns above it (they would all go too).
    stillWater[oi] = 0u;
    stillWater[oi + 1u] = 1u << 28u;
    stillWater[oi + 5u] = (0x80u << 14u) | (1u << 22u);
    stillWater[oi + 2u] = (stillWater[oi + 2u] & 0xFFFF0000u) | stamp16(T.tick);
    stillWater[ei] = 0u;
    stillWater[ei + 1u] = 0u;
    // WAKE ON RELEASE (phase 3): the chunk(s) at the released target's top
    // cell are marked for the next tick, so the columns that rested on this
    // one are looked at again even when the released water does not move
    // (a finite source's stream ends in films that stay put, and a held
    // column whose chunk never wakes keeps a stale target -- a point where
    // water would later be deposited into the ledger). One mark per release;
    // a release is once per adoption, so this cannot keep anything awake.
    let tc = vec3<i32>(x, (sdec(stillWater[si + 2u]) - 1) >> 3u, z);
    for (var k = 0u; k < 8u; k++) {
      let ns = dirtyFanSlot(tc, T.origin, k);
      if (ns != SLOT_NONE) { atomicOr(&dirtyOut[ns], DIRTY_R_WRITE); }
    }
    stillWater[si + 2u] = 0u;
    stillWater[si + 3u] = 0u;
  } else if (act == STREAM_ACT_ADOPT_POOL && stillWater[si + 2u] != 0u) {
    // RETARGET (scan's adopted branch): lower [2]'s target by the drop in the
    // flood-count bits, keep the hop, reset the count. Wake the column's
    // chunk(s) so the columns it feeds look at their own feed again.
    let d2 = stillWater[si + 2u];
    let nt = sdec(d2) - i32((w3 >> STREAM_OVER_SHIFT) & STREAM_OVER_MAX);
    stillWater[si + 2u] = (bitcast<u32>(nt) & 0xFFFFFu) | (d2 & 0xFFF00000u);
    stillWater[si + 3u] = (w3 & 0xFFFu) | (w3 & STREAM_POOL_BIT);
    let tc = vec3<i32>(x, (nt - 1) >> 3u, z);
    for (var k = 0u; k < 8u; k++) {
      let ns = dirtyFanSlot(tc, T.origin, k);
      if (ns != SLOT_NONE) { atomicOr(&dirtyOut[ns], DIRTY_R_WRITE); }
    }
  } else if (act == STREAM_ACT_ADOPT || act == STREAM_ACT_ADOPT_POOL) {
    // The surface `scan` saw is still this column's (nothing writes a voxel
    // between the two kernels); its material goes into [3].
    let s = columnSurface(x, wc.y * i32(CHUNK) + i32(CHUNK) - 1, z);
    if (s.ok) {
      stillWater[si + 2u] = (w3 & 0xFFFFFu) | (((w3 >> 20u) & STREAM_HOP_MAX) << 20u);
      stillWater[si + 3u] = (s.mat & 0xFFFu) | select(0u, STREAM_POOL_BIT, act == STREAM_ACT_ADOPT_POOL);
      stillWater[oi + 5u] = T.tick;
      let o6 = stillWater[oi + 6u];
      stillWater[oi + 6u] = (o6 & 0xFFFF00FFu) | (min(((o6 >> 8u) & 0xFFu) + 1u, 0xFFu) << 8u);
    } else {
      stillWater[si + 3u] = 0u;
    }
  }
  let published = stillWater[oi + 2u];
  if (stillWater[oi + 3u] != published) { stillWater[oi + 3u] = published; }
}

// THE LOAD RESET (pass_table.def lr_streamClear, PT_LOADRESET). A stream's
// words describe the world they were adopted in, and a LOAD replaces that
// world. Dense worldgen clears the whole table, but a load refills the window
// through the streamed path, whose `cols` keeps a column's dynamic words
// while its tag is unchanged -- and a save loaded where the window already
// stood changes no tag. So the old session's streams outlived the load: a
// stale target held whatever water the loaded world had in that column at
// the old level (refilling it from nothing), and the same save behaved
// differently loaded into a fresh process and into one that had been playing.
// One thread per window column, the dynamic words only: the authored [0] /
// [1], which the refill's `cols` has just written, stay. The load's streams
// run as water and are adopted again, as the header says they are.
@compute @workgroup_size(256)
fn clearStreams(@builtin(global_invocation_id) gid : vec3<u32>) {
  let i = gid.x;
  if (i >= WORLD_N * WORLD_N) { return; }
  // The conduit's ledger is water in a stream the load replaced: gone with it
  // (a bounded loss -- see sim_step.wgsl THE CONDUIT).
  if (i == 0u) {
    stillWater[STREAM_LEDGER] = 0u;
    stillWater[STREAM_LEDGER + 1u] = 0u;
    stillWater[STREAM_LEDGER + 2u] = 0u;   // the diagnostic deposit / withdrawal totals
    stillWater[STREAM_LEDGER + 3u] = 0u;
  }
  stillWater[i * STILL_WORDS + 2u] = 0u;
  stillWater[i * STILL_WORDS + 3u] = 0u;
  // ...and its echo and observer words (STREAMS ADOPT ONLY WHEN NATURAL).
  stillWater[STILL_ECHO + 2u * i] = 0u;
  stillWater[STILL_ECHO + 2u * i + 1u] = 0u;
  for (var w = 0u; w < 8u; w++) { stillWater[STREAM_OBS + 8u * i + w] = 0u; }
}
