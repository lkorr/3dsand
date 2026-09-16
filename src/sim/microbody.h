#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "sim/voxload.h"
#include "sim/world.h"

// Dynamic microvoxel BODIES (docs/PLAN_voxel_editor.md §C, DESIGN.md §9).
//
// A mob def may declare `"skinScale": 2|4|8: its limb .vox models are authored
// at that many voxels per WORLD voxel, so a limb whose .vox box is 6x2x2 at
// skinScale 2 occupies 3x1x1 world voxels. Physics builds the same limb at a
// pitch of 1/physScale — a SEPARATE, coarser resolution derived from the art
// (mob.h) — so the creature is the same physical size as a scale-1 one with a
// coarser skin: the extra resolution buys detail, not bulk, and does not drag
// the collider up with it.
//
// RENDERING is where they diverge. The cube path (debris.wgsl `vsBody`) emits
// one 36-vertex cube per voxel; at scale 4 that is 64x the instances for the
// same silhouette, which is exactly the wrong trade. Instead each micro body
// rasterizes ONE box — its oriented bounding box, 36 vertices total — and the
// fragment shader marches the limb's brick in object space. Cost then scales
// with SCREEN AREA rather than with voxel count, which is what makes 4x
// resolution affordable (rule 2: cost tracks activity, not content size).
//
// DETERMINISM (rule 1): everything here is render-only, exactly like the static
// micro pool. The pool and the tables are bound to the microbody render
// pipeline and to nothing else; the sim never sees them. Damage edits micro
// payloads (see the COW section below), but only ever as presentation state —
// a body's authoritative voxels live in DebrisSystem, and nothing here feeds
// back into the hashed grid.

// ---- GPU layout ------------------------------------------------------------

// One limb model, 16 bytes — must match struct MicroBodyModel in common.wgsl.
struct MicroBodyModelGpu {
  // Word index into the pool where this model's payload starts. The payload is
  // dims.x*dims.y*dims.z micro voxels, 2 packed 16-bit voxels per word,
  // x-major then y then z (idx = (z*dy + y)*dx + x), padded to a whole word.
  //
  // Each 16-bit voxel is:
  //   bits 0..7   material ID — what the voxel IS. Palette index == material ID
  //               (the project-wide .vox convention), so it shades through the
  //               ordinary material table. 8 bits caps a micro body at material
  //               ids 1..255, checked at load.
  //   bits 8..15  ART COLOUR — a 1-BASED MERGED art index, 0 meaning "use the
  //               material's own colour". A creature is one material all over
  //               and painted per voxel, so colour cannot share the material's
  //               channel. Resolved as materials[ART_PALETTE_BASE + (art - 1)]
  //               (kArtPaletteSlotsGpu in world.h); render-only, never hashed.
  //
  //               NOT a .vox palette slot. It held one until 2026-08-30, which
  //               capped the merged palette at 128 because the slot was stored
  //               as 128 + index in this same byte. The conversion now happens
  //               once on the CPU at load (MicroBodyMergeArt), so the byte
  //               carries the full 1..255 range and this encoding matches the
  //               cube path's (debris.wgsl) exactly.
  uint32_t base;
  // bits 0..9 dims.x, bits 10..19 dims.y, bits 20..29 dims.z (micro voxels).
  // 10 bits each is 1023 per axis, far past the +-127 DebrisVoxel bound that
  // limits a limb anyway; packing them keeps the record at 16 bytes.
  //
  // bit 30 (kMicroBodyDimsStainBit): THIS BLOCK CARRIES A STAIN LATTICE. One
  // byte per micro voxel, 4 per word, laid out after the payload at
  // `base + WordsFor(cells)` in the same idx order, holding `slot << 4 | amt`
  // -- the world's 3-bit stain PALETTE SLOT and the 0..15 amount. This is the
  // one place the 16-bit body coat word (voxload.h BodyStain*, which names a
  // MATERIAL) is narrowed to what the shader needs, through
  // MicroBodySet::stainSlotOfMat; the shader is unchanged by that. Only OWNED
  // blocks have one -- a shared def model is clean by definition, and a body
  // becomes owned the first time anything marks it -- so the pool pays the
  // extra 50% only for bodies that have actually been bloodied. The shader
  // reads the flag off this word and derives the lattice offset from dims;
  // nothing else in the record changes, which is what keeps it 16 bytes.
  uint32_t dims;
  uint32_t scale;  // micro voxels per world voxel: 2 or 4
  // ---- THE CUT-FACE MASK (2026-09-11) --------------------------------------
  // Six bits, `axis * 2 + positive` — the same face encoding shadowFaceOf uses
  // — saying which of the brick's six boundary planes is a JOINT rather than
  // the end of the model. Consumed only by microbody.wgsl's smooth-normal
  // gradient (BODY_SMOOTH_N), which differentiates the occupancy field and
  // needs to know what lies past the brick.
  //
  // WHY IT CANNOT BE DECIDED IN THE SHADER. A limb is its own brick, tightly
  // bounded, and the shader can see no other limb. Past the SIDE of an arm is
  // air, so treating outside-the-brick as empty is what rounds the silhouette;
  // past the END of an upper arm is the FOREARM, and treating that as empty
  // rounds the cap too — which put a hard light/dark ring at every shoulder,
  // elbow, hip and knee, swinging with the joint as it animated (owner report:
  // "the conjoining seams of limb nodes pulse and flash"). The two cases are
  // locally identical: in both, the boundary cell is solid and the cell outside
  // is unknown. What tells them apart is GLOBAL — a cut plane is a whole
  // cross-section of the limb, a tangent row on a cylinder is a line — so it is
  // measured once here at pack time and carried on the model.
  //
  // It rides the word that was padding. MicroBodyModelGpu must stay 16 bytes
  // (the static_assert below, and the hand-written mirror in common.wgsl that
  // nothing checks), so a new field is not available; this one was already
  // there, uploaded, and documented as carrying nothing.
  uint32_t cutFaces;
};
static_assert(sizeof(MicroBodyModelGpu) == 16,
              "must match common.wgsl MicroBodyModel");

// One micro body to draw this frame, 16 bytes — must match MicroBodyInst in
// microbody.wgsl. COMPACTED: the draw's instance count is exactly the number
// of micro bodies on screen, so a world with none dispatches no vertices at
// all (rule 2). Slot indexes the shared bodyXforms buffer, which both the cube
// pass and this one read — the two passes partition the slots, never duplicate.
struct MicroBodyInstGpu {
  uint32_t slot;
  uint32_t model;
  // HIT FLASH, as the BIT PATTERN of a float (WGSL reads it with
  // `bitcast<f32>`). Additive, in linear HDR, applied at shade time on top of
  // the material's own emission — game/mob.h MobLimb::hitFlash is where the
  // value comes from and what decays it.
  //
  // A REUSED PADDING WORD, not a wider struct, and the two static_asserts below
  // are why that mattered: the instance buffer is sized by `sizeof` in two
  // places (simulation.cpp) but the WGSL side is a hand-written mirror that
  // nothing checks — check_invariants.py's check_gpu_structs only reads
  // sim/world.h and only matches structs whose names are IDENTICAL, so
  // MicroBodyInst/MicroBodyInstGpu are invisible to it. Staying at 16 bytes
  // keeps the one guard that does exist meaningful.
  //
  // A BIT PATTERN rather than a fixed-point integer on purpose: a scale factor
  // would be a magic number that has to agree in two files with no checker
  // between them, which is exactly the class of bug the note above describes.
  // 0 bitcasts to 0.0f, so the default is "no flash" for free — which is what
  // debris (phys/debris.cpp) keeps passing.
  uint32_t flashBits = 0;
  // THE DYE, packed (game/dye.h): bit 24 set = dyed, low 24 bits the colour in
  // `unpackColor`'s own byte order. 0 = undyed, which is every body limb, every
  // piece of debris and every garment nobody has coloured — so the default
  // costs nothing and the shader's test is one bit.
  //
  // The LAST padding word this struct had, taken for the same reason
  // `flashBits` took the one before it: the struct must stay 16 bytes (the
  // static_assert below is the only mechanical guard the CPU/WGSL pair has,
  // because check_invariants.py cannot see a hand-written mirror), so a new
  // per-instance value either fits in a spare word or does not exist. There
  // are now no spare words. The next one needs a second buffer.
  uint32_t dye = 0;
};
static_assert(sizeof(MicroBodyInstGpu) == 16,
              "must match microbody.wgsl MicroBodyInst");

// ---- CPU side --------------------------------------------------------------

// The pool plus the per-model records.
//
// Two populations share one pool. **Shared** models are packed at mob-def load
// (or on first sphere spawn), indexed by every instance of that def, and never
// freed. **Owned** models are copy-on-write clones created the first time a
// particular BODY is damaged — from then on that body has its own payload and
// edits are local to it (MicroBodyOwn / MicroBodyEdit below).
//
// Freed owned blocks return to `freeList` keyed by word count and are reused
// verbatim, which is what keeps "shoot the same sphere for ten minutes" from
// walking the pool's high-water mark to the ceiling. Blocks are never
// coalesced or compacted: a compaction would have to rewrite every live
// model's `base` while the GPU may still be reading last frame's upload, and
// exact-size reuse already handles the dominant case (a body re-editing its
// own block in place, which does not reallocate at all).
struct MicroBodySet {
  std::vector<MicroBodyModelGpu> models;
  std::vector<uint32_t> pool;
  // model index -> true when this record is an owned COW clone (freeable).
  // Shared models must never be freed: other instances still point at them.
  std::vector<uint8_t> owned;
  // model index -> words actually reserved at `base`. This is NOT derivable
  // from dims: an edited body reuses its block in place while shrinking, so
  // the block stays the size it was allocated at and must be freed at that
  // size or the surplus leaks out of the pool permanently.
  std::vector<uint32_t> blockWords;
  // word count -> list of free block bases of exactly that size.
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> freeList;
  // Retired owned model records, reusable so a long fight does not exhaust
  // kMaxMicroBodyModels even though the pool words are recycled.
  std::vector<uint32_t> freeModels;

  // ---- WHEN A CEILING IS HIT --------------------------------------------
  //
  // Every allocator in this file degrades by REFUSING: the call returns -1, the
  // caller leaves the skin alone, and the body's authoritative voxels change
  // with nothing on screen to show it. That degradation is correct — losing
  // detail under memory pressure beats refusing to be destructible — and it was
  // completely SILENT, which is the whole reason a 256-record table shipped as
  // a mystery instead of as a line of stderr. Owner report 2026-09-15: "after
  // killing a bunch of zombies, new zombies spawn with 0 gore and won't get any
  // when I hit them; it fixes itself when I leave the area with the corpses."
  // Every word of that is this counter's story — the corpses were holding the
  // records — and nothing in the engine said so.
  //
  // COUNTED rather than logged at the call site, because a full table refuses
  // once per burning voxel per tick: the report has to be rate limited, and the
  // count is itself the number worth reporting.
  uint32_t refusals = 0;
  // Set by any mutation; the caller re-uploads and clears. Batching one upload
  // per tick rather than one per edit is rule 2 applied to PCIe traffic.
  bool dirty = false;

  // ---- incremental pool upload ---------------------------------------------
  //
  // `dirty` above means "something changed, re-upload" and the MODEL TABLE
  // takes it literally: the table is kMaxMicroBodyModels * 16 bytes, small
  // enough that sending all of it is the right answer.
  //
  // The POOL is not. It is 4 MiB, and Simulation::UploadMicroBodies used to
  // write ALL of it on any dirty. That was free while the only thing that
  // dirtied it was a carve — a rare event — and it becomes a 4 MiB/tick PCIe
  // write the moment a body burns per voxel, against a documented budget of
  // < 1 MB/tick (DESIGN.md §11). So every pool write records the WORD RANGE it
  // touched and the upload sends only those ranges.
  //
  // Ranges are coalesced greedily (a burning limb's pokes all land inside one
  // model's block, so they merge into one span) and CAPPED: past
  // kMaxDirtyRanges the set gives up and asks for a whole-pool write. That
  // fallback is what makes correctness independent of how well the merge went
  // — a missed range would be a stale brick on screen, and the failure mode of
  // a coalescing heuristic must never be "wrong", only "slower".
  static constexpr size_t kMaxDirtyRanges = 24;
  // Words this far apart are merged into one range rather than kept separate:
  // one 2 KiB write beats two writes plus a range slot, and the slack is only
  // ever re-sending words that did not change.
  static constexpr uint32_t kDirtyMergeGap = 512;
  std::vector<std::pair<uint32_t, uint32_t>> dirtyRanges;  // [lo, hi) words
  // Starts TRUE so a freshly constructed set publishes its whole pool once.
  // Every hot reload replaces the set wholesale (`mbSet = MicroBodySet{}`), so
  // this is also what makes a reload re-send everything without the reload path
  // having to know the range machinery exists.
  bool poolDirtyAll = true;

  // Record that pool words [lo, hi) were written. Also sets `dirty`.
  void MarkPool(uint32_t lo, uint32_t hi);
  // Called by the uploader once the GPU copy has been issued.
  void ClearDirty();

  // ---- art palette ----
  // Skin colours from every loaded prefab, merged and deduplicated by RGB. It
  // rides here rather than on any one Prefab because a frame draws limbs from
  // several defs at once and they all resolve against ONE reserved run of the
  // material table — so the merge has to happen where the models are pooled,
  // which is here. Render-only, exactly like the pool itself.
  //
  // Packed 0x00RRGGBB. Entry i is stored in a voxel's art byte as i + 1 and
  // read back by the shaders as materials[ART_PALETTE_BASE + i]. Bounded by
  // kArtPaletteSlotsGpu (world.h) = 255, the 1-based ceiling of that byte.
  std::vector<uint32_t> artColors;

  // ---- material id -> stain PALETTE SLOT (1..7, 0 = does not stain) --------
  //
  // The body coat word names a MATERIAL (sim/voxload.h) because everything
  // except the renderer needs to know WHICH substance is on a voxel. The
  // renderer needs three bits. This table is the conversion, and it lives here
  // rather than at the call sites so that WriteBrick / MicroBodyPack /
  // MicroBodyPokeStain all narrow the same way and no caller has to hold a
  // material table to poke a stain.
  //
  // Deliberately a bare vector of bytes and not a `const std::vector<
  // MaterialDef>*`: this header is included by the render path and must not
  // grow a dependency on sim/materials.h. Filled through
  // MicroBodySetStainSlots at every materials load; an empty table (or a
  // material past its end) means "no slot", i.e. amount 0, i.e. nothing drawn
  // -- which is the right failure for a set that was never told.
  std::vector<uint8_t> stainSlotOfMat;
};

// Publish material id -> stain palette slot into `set`. Called wherever the
// material table is (re)built: LoadMobDefs, which already clears the set, and
// MobSystem::OnMaterialsReloaded, because an R reload can renumber the slots
// under bricks that are already packed.
void MicroBodySetStainSlots(MicroBodySet& set, std::vector<uint8_t> slotOfMat);

// Merge one prefab's art palette into `set`, remapping its slots if needed.
// Returns a 256-entry table mapping the prefab's .vox palette SLOT (128..255)
// -> a 1-BASED MERGED INDEX, so the caller can rewrite its voxels' `color`
// before packing. The two sides speak different numbering systems on purpose:
// applying this table IS the one conversion from file space to engine space,
// and after it no `color` anywhere is a .vox slot again.
//
// Colours beyond kArtPaletteSlotsGpu are dropped to 0 (unpainted) and reported.
std::vector<uint8_t> MicroBodyMergeArt(MicroBodySet& set,
                                       const std::vector<uint32_t>& artColors,
                                       const std::string& label,
                                       std::string& log);

// Packs one limb's voxel model into `set` and returns its model index, or -1 on
// failure (pool full, out-of-range material, empty model). `voxels` are in
// MICRO units with a min corner already at 0 (PrefabModel-local coords).
//
// `log` collects diagnostics. A limb that fails to pack does NOT fall back to
// the cube path — cube instances are one WORLD voxel each, so a scale-2 limb
// drawn that way would be twice its real size — it simply does not render,
// which the loader says out loud. A broken limb must not stop the mob from
// loading (DESIGN.md §6).
//
// `cutFaces` is the 6-bit joint mask described on MicroBodyModelGpu::cutFaces.
// It defaults to 0 — "every boundary plane of this brick is the edge of the
// model" — which is the right answer for a prefab that is ONE model: an item's
// blade, a debris chunk, a carved COW clone. Anything packed out of a
// MULTI-model prefab — a mob's limbs, a garment's panels — should pass
// MicroBodyCutFaces below.
int MicroBodyPack(MicroBodySet& set, const std::vector<PrefabVoxel>& voxels,
                  IVec3 dims, uint32_t scale, const std::string& label,
                  std::string& log, uint32_t cutFaces = 0);

// ---- WHICH OF A BRICK'S SIX FACES IS A JOINT --------------------------------
//
// The `cutFaces` value for model `self` of `pf`: the 6-bit mask, `axis * 2 +
// positive`, of the boundary planes that ANOTHER model of the same prefab is
// pressed against. See MicroBodyModelGpu::cutFaces for what the shader does
// with it and why the shader cannot work it out for itself.
//
// A FRACTION, NOT A PREDICATE, because parts meet imperfectly: art with a
// one-voxel taper at the shoulder leaves part of the cap uncovered, and a held
// prop can graze a face it is not jointed to. Half the solid boundary cells
// covered is the line, which no single stray voxel can cross and no ordinary
// joint fails.
//
// Returns 0 for a single-model prefab, which is the pre-2026-09-11 behaviour.
//
// NOT MOB-SPECIFIC, and it lived in mob.cpp for one day before that cost
// something: a WORN ITEM is shaped exactly like a mob — one named model per
// covered limb, in one shared frame — so a robe packed with cutFaces = 0 grew
// a rounded end cap on every sleeve, yoke and hem, and the caps' shading swung
// with the joint they straddled (owner report 2026-09-12: "the overlapping
// parts are pulsing like crazy"). The rule is about MULTI-MODEL PREFABS, so it
// belongs beside the packer every multi-model prefab goes through.
uint32_t MicroBodyCutFaces(const Prefab& pf, int self);

// ---- copy-on-write: destructible micro bodies -------------------------------
//
// A shared model cannot be edited — every instance of the def points at it, so
// carving a crater in one sphere would crater all of them. Damage therefore
// goes through MicroBodyOwn first: it clones the model into a fresh block and
// returns a NEW model index that exactly one body holds. Calling it on a model
// that is already owned is a no-op returning the same index, so the damage path
// can call it unconditionally and only the first hit pays the copy.
//
// Returns -1 if the pool or the model table is full, in which case the caller
// must fall back to leaving the body's rendering alone (the body's authoritative
// voxels still change; only the skin stops keeping up). Losing detail under
// memory pressure beats refusing to be destructible.
int MicroBodyOwn(MicroBodySet& set, uint32_t model);

// ALWAYS produces a fresh private clone, even of a model that is already owned.
//
// THE DIFFERENCE FROM MicroBodyOwn, AND WHY BOTH EXIST. Own answers "may THIS
// body edit its brick?", so its no-op on an already-owned model is exactly
// right for the damage path: the second hit on a body must not clone again.
// It is exactly WRONG for a NEW body that INHERITED a model index from an old
// one — a shatter fragment taking `nb.micro = b.micro` — because there the
// question is "does this body have a brick OF ITS OWN?" and the honest answer
// for an owned parent is no.
//
// Getting that wrong is not a leak, it is aliasing: the fragment's re-skin
// rewrites the PARENT's payload, both records' teardowns call MicroBodyFree on
// one block, and the survivor is left drawing a model whose dims the first free
// zeroed — an invisible limb. Worse, the freed record goes back on `freeModels`
// while a live body still points at it, so the NEXT MicroBodyOwn anywhere in
// the world (a different creature's first carve) hands that record out again
// and the stale holder's eventual free takes the newcomer's brick with it.
// That is how a fragment of a burning corpse makes a living mob's torso vanish.
//
// Returns -1 on a full pool or model table, exactly like MicroBodyOwn.
int MicroBodyClone(MicroBodySet& set, uint32_t model);

// Rewrites an OWNED model's payload from `voxels` (micro units, coordinates
// relative to the body origin, i.e. the same frame DebrisSystem stores). The
// model's dims/base are re-derived: a body that lost its top half gets a
// smaller brick, so the OBB the raster pass draws shrinks with it and the march
// does not wade through empty space.
//
// `originShift` receives the min corner the new brick was rebased to, in MICRO
// units. The caller must shift the body transform by that (rotated into world
// space) or the art will drift off the collider — the brick march runs [0..dims)
// from the body origin, so moving the min corner moves the art.
//
// Returns false and leaves the model untouched when `voxels` is empty or the
// re-pack does not fit; `model` must be owned (MicroBodyOwn first).
bool MicroBodyEdit(MicroBodySet& set, uint32_t model,
                   const std::vector<PrefabVoxel>& voxels, IVec3& originShift);

// Rewrites ONE micro voxel of an OWNED model in place: material id + art slot,
// nothing else. Coordinates are BRICK-LOCAL, i.e. [0, dims) in the frame the
// fragment shader marches — the frame MicroBodyEdit last rebased the payload
// to, NOT the caller's original lattice.
//
// This is the sibling MicroBodyEdit cannot be. Edit re-derives the tight box,
// rebases the origin, re-packs the whole payload and may reallocate the block,
// which is right for a CARVE (the shape changed) and catastrophic for a STATE
// change (the shape did not change — one voxel's material did). A per-voxel
// burn does the latter every tick, and doing it through Edit would re-pack a
// 15.7k-word torso to char a single voxel.
//
// Removal goes through here too — poke material 0 — so the shrinking re-pack
// can be deferred and batched exactly the way the collider rebuild already is.
// Returns false if the model is not owned or the coordinate is outside dims.
bool MicroBodyPoke(MicroBodySet& set, uint32_t model, int x, int y, int z,
                   uint8_t mat, uint8_t art);

// Rewrites ONE micro voxel's STAIN BYTE in an OWNED model, brick-local
// coordinates exactly as MicroBodyPoke. Takes the 16-bit BODY COAT word
// (voxload.h BodyStain*) and narrows it here through `stainSlotOfMat` -- a
// material with no palette slot writes amount 0, i.e. clean, so a coat the
// renderer has no colour for is invisible rather than mis-coloured. The stain
// lattice is allocated the first time an owned model is asked for one (the
// block is reallocated at payload + stain size, so a model that MicroBodyPack
// made owned without a stain grows one here). Returns false if the model is
// not owned, the coordinate is outside dims, or the pool cannot grow the
// block.
bool MicroBodyPokeStain(MicroBodySet& set, uint32_t model, int x, int y, int z,
                        uint16_t stain);

// A model's brick dimensions in micro voxels; {0,0,0} for an invalid index.
IVec3 MicroBodyDims(const MicroBodySet& set, uint32_t model);

// dims-word flag: the block holds a stain lattice after its payload.
constexpr uint32_t kMicroBodyDimsStainBit = 1u << 30;
constexpr uint32_t kMicroBodyDimsMask = 0x3FFFFFFFu;

// Returns an owned model's words to the free list and retires its record.
// Safe (no-op) on shared models and on kMicroBodyNoModel, so body teardown can
// call it blindly.
void MicroBodyFree(MicroBodySet& set, uint32_t model);

// ---- WHO HOLDS A BRICK RECORD (the aliasing audit) --------------------------
//
// A record has exactly ONE holder. Every lifecycle rule in this file assumes
// it: an edit rewrites the block in place, a free zeroes the record's dims and
// puts it back on `freeModels`. Two holders therefore means one of them is
// about to go invisible and the record is about to be handed to a THIRD body
// that is still alive — which is how a corpse's limb ends up drawn on a living
// creature (owner report 2026-09-16: "I kill a few zombies and then my torso
// swaps with one of theirs; my foot becomes a zombie torso").
//
// Detecting that from the DRAW LIST alone is not enough, and that is the whole
// reason this exists as a separate walk. A stale holder is very often one that
// does not draw — a limb whose `body` went to zero at a sever, a slot in the
// hold beat after a cut, a first-person-suppressed avatar part — and those are
// exactly the holders whose eventual free takes a live body's brick with it.
// So every system enumerates EVERY index it holds, drawn or not, and says what
// it is in words: the report has to name two entities, or it is just a number.
struct MicroHolder {
  uint32_t model = kMicroBodyNoModel;
  // "debris body 12 (handle 0x…, serial 940)", "zombie#7 limb head
  // (body, carved)", "avatar limb l_foot (SEVERED, no body)". Built by the
  // holding system, because only it can name its own parts.
  std::string what;
};

// A body's micro rendering, passed along when ownership of the body moves.
//
// This travels as an explicit argument (MobSystem hands it to
// DebrisSystem::AdoptBody) rather than living in a side table keyed by physics
// handle. A side table would have to be kept in sync at every spawn, sever,
// death, cull and reset — five agreements enforced by nothing — and a recycled
// Jolt BodyID could then paint unrelated debris as somebody's leg. As a
// parameter the invariant "this description belongs to that body" is not
// merely maintained, it is unrepresentable-if-violated.
//
// The routing key is still the BODY, not "is a mob limb": that is what makes a
// severed micro limb keep its detail with no special case at the adoption site.
//
// RENDER ONLY. This struct describes the SKIN and nothing else. It used to
// carry a single `scale` that also defined the units of the body's collider
// voxels, which is what capped micro bodies at 4: the collider lives in
// DebrisVoxel (int8, +-120), so a finer skin bought a proportionally smaller
// creature. The two now move independently — `skinScale` here, `physScale` on
// the body — because their costs are unrelated. The brick march is a fragment
// shader over one OBB, so skin cost tracks SCREEN AREA; the collider is Jolt
// boxes, so physics cost tracks VOXEL COUNT. Coupling them held the cheap axis
// hostage to the expensive one.
struct MicroBodyRef {
  // kMicroBodyNoModel = ordinary cube path (plain debris, scale-1 limbs).
  uint32_t model = kMicroBodyNoModel;
  // Micro voxels per world voxel in the BRICK. The body's `skinVoxels` are in
  // these units; its collider `voxels` are in `Body::physScale` units, which
  // may be coarser. Equal values are the ordinary case and mean the two
  // lattices coincide exactly as they did before the split.
  uint32_t skinScale = 1;
  // The DYE this body renders in (game/dye.h), packed; 0 = undyed, which is
  // everything except a coloured garment.
  //
  // IT BELONGS HERE BECAUSE A MicroBodyRef IS A RENDER DESCRIPTION — the note
  // three lines up already says so, and "what colour is it" is the same kind
  // of fact as "which brick" and "at what pitch". Putting it here is also what
  // makes the severed-garment case free: DebrisSystem::AdoptBody already takes
  // a ref, both of Mob's hand-off sites already build one from the limb, so a
  // sleeve cut off a red shirt arrives in the debris system red with no new
  // argument threaded through either call.
  uint32_t dye = 0;
  bool Valid() const { return model != kMicroBodyNoModel; }
};
