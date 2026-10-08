#pragma once
//
// THE AUTOPSY: PEELING A CORPSE ON THE DEATH SCREEN, A LAYER AT A TIME.
//
// The tuner's Models tab has had a PEEL since the anatomy landed
// (assets/editor/editor.js, "Peel: hide the outermost depth layers"): measure
// every voxel's depth from the surface, hide the ones shallower than N, and
// what is left on screen is the body N layers in. This is that, on the body
// that just died, so "what killed me" can be answered from the inside: a bite
// whose venom reached the bone, a burn that cooked the muscle under unbroken
// skin, a torso hollowed by rot under an intact face.
//
// ---- RENDER ONLY, AND IT NEVER TOUCHES THE BODY ------------------------------
//
// A peel is a COPY. Each drawn limb brick is cloned into the micro pool
// (MicroBodyClone), the peeled cells are cleared in the copy, and the portrait
// pass swaps the clone's model index into its own instance list for the one
// draw. The corpse's own bricks, its DebrisSystem voxels and everything the sim
// hashes are untouched — the micro pool is presentation state to begin with
// (sim/microbody.h) — so peeling to the bone and pressing respawn leaves no
// trace. The clones are freed when the peel goes back to 0, on respawn, and
// whenever the source brick they copy changes (a corpse that is still burning
// or being eaten re-peels, throttled, so the portrait stays the body as it is).
//
// ---- DEPTH IS PER BRICK, WITH THE JOINTS SHUT --------------------------------
//
// The tuner measures depth over the UNION of every limb, treating joint faces
// as interior. The engine has no union — each limb is its own brick, posed
// independently — but every brick already carries the answer to "is this
// boundary plane a joint" (MicroBodyModelGpu::cutFaces, measured at pack time
// for the smooth-normal gradient). Treating a cut face as solid beyond is what
// the union would have said, so a peeled forearm does not open a hollow ring
// at the elbow. Multi-source 6-connected BFS, depth 0 = a cell with an exposed
// face, exactly anatomy.js's convention, so "layer 2" means the same thing on
// both sides.
//
// ---- AND THE STRATA ARE THE READOUT ------------------------------------------
//
// The same depth fields give the autopsy's numbers: for every depth layer,
// what the body (and each part) is made of there. Read top-down that is the
// anatomy recipe — skin, flesh, muscle and blood, bone — and anything that is
// NOT the recipe stands out of it: rot two layers under the skin, char at the
// muscle. Those are the FINDINGS.

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sim/microbody.h"

struct UIState;
struct MaterialDef;

namespace autopsy {

// How a material reads in an autopsy. Classified by the caller (main.cpp knows
// the tissue / burn / infection tables the HUD already resolves) and handed in
// as a 256-entry table, because a micro voxel's material is 8 bits.
enum Kind : uint8_t {
  kOther = 0,      // neither tissue nor damage (metal on a robot, a shard)
  kTissue = 1,     // what the body is built from: skin, flesh, muscle, bone, blood
  kBurn = 2,       // cooked / burning / charred
  kInfection = 3,  // a material with an `infect` block: rot, venom
};

// Deepest layer reported. A human torso is ~16 skin voxels deep at its core;
// anything past this is lumped into the last layer.
constexpr int kMaxLayers = 32;

// The depth of every cell of one brick. kEmpty for an empty cell.
constexpr uint8_t kEmpty = 0xFF;
struct DepthField {
  int dx = 0, dy = 0, dz = 0;
  std::vector<uint8_t> depth;
  int maxDepth = -1;  // deepest solid cell; -1 = the brick is empty
  uint8_t At(int x, int y, int z) const {
    return depth[((size_t)z * dy + y) * dx + x];
  }
};

// Measure `model`'s depth field. The model's cut faces count as interior.
void MeasureDepth(const MicroBodySet& set, uint32_t model, DepthField& out);

// One part of the frozen body, as the portrait pass sees it.
struct Part {
  uint32_t model = 0;  // its micro model THIS frame (an owned clone or shared)
  int figSlot = -1;    // UIState body slot, -1 = gear / no figure slot
  bool isLimb = false; // part of the body (false = a garment, a held item)
};

// What one depth layer is made of.
struct Layer {
  uint32_t total = 0;
  uint32_t byMat[256] = {};
};

// Per-slot strata. Slot -1 parts (gear) are not counted.
struct Strata {
  int maxDepth = -1;
  Layer layers[kMaxLayers];
  void Clear() {
    maxDepth = -1;
    for (Layer& l : layers) l = Layer{};
  }
};

// THE PEEL CACHE. Owned by the frame loop for as long as a death screen is
// up; every clone it makes is freed by Release() (or by Update when it goes
// stale).
class Peeler {
 public:
  // Re-peel when anything changed: the peel level, a source brick's content
  // (editGen), the set itself (a materials reload replaces it), or the part
  // list. `now` is seconds; a source whose content keeps changing (a corpse
  // still burning) is re-peeled at most every `throttle` seconds.
  //
  // `slotCount` is how many figure slots the strata are kept for.
  // Returns true when the clones changed (the caller owes an upload).
  bool Update(MicroBodySet& set, const std::vector<Part>& parts, int peel,
              double now, int slotCount, double throttle = 0.25);

  // The model to draw in place of `src` at the current peel: `src` itself
  // when nothing is peeled, kHidden when the peel took every cell of it.
  static constexpr uint32_t kHidden = 0xFFFFFFFFu;
  uint32_t DrawModel(uint32_t src) const;

  // Free every clone. Safe to call with a set that has been replaced (the
  // clones of a dead set are simply forgotten, never freed into the new one).
  void Release(MicroBodySet& set);

  // Strata of the whole body (every limb part) and of each figure slot,
  // measured on the SOURCE bricks: the readout says what is there at each
  // depth whatever the current peel hides.
  const Strata& Body() const { return body_; }
  const Strata& Slot(int s) const { return slots_[(size_t)s]; }
  int SlotCount() const { return (int)slots_.size(); }
  int MaxDepth() const { return body_.maxDepth; }
  // Bumped whenever the strata were re-measured, so the UI mirror can skip a
  // copy that would say the same thing.
  uint32_t Generation() const { return gen_; }
  int ClonesLive() const { return (int)entries_.size(); }
  uint32_t Refusals() const { return refusals_; }

 private:
  struct Entry {
    uint32_t clone = kHidden;   // kHidden = every cell peeled (nothing drawn)
    uint32_t srcGen = 0;        // MicroBodyEditGen(src) when it was made
    uint32_t srcBase = 0;       // the source block, for reuse detection
    uint32_t srcDims = 0;
    int peel = 0;
    double madeAt = 0.0;
  };
  std::unordered_map<uint32_t, Entry> entries_;
  // The strata are keyed on the set of (model, editGen) they measured.
  std::vector<uint64_t> measuredKey_;
  double measuredAt_ = -1e9;
  uint64_t setEpoch_ = 0;
  Strata body_;
  std::vector<Strata> slots_;
  uint32_t gen_ = 0;
  uint32_t refusals_ = 0;
  DepthField scratch_;
};

// Mirror `p`'s strata into the death screen's readout (UIState::autopsy):
// the legend, the per-layer composition of the body and of every part, and the
// findings. `kinds` is the 256-entry Kind table. Skips the copy when the
// strata have not been re-measured since the last call.
void FillUI(const Peeler& p, const uint8_t* kinds,
            const std::vector<MaterialDef>& mats, UIState& ui);

}  // namespace autopsy
