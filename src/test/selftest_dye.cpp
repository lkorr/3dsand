// selftest_dye.cpp — THE COMMONER WARDROBE AND ITS COLOUR.
//
// The clothes in assets/items (tunic, smock, jerkin, trousers, breeches, hose,
// shoes, clogs, footwraps) are nine PATTERNS, painted in greyscale, and the
// colour arrives at runtime as one packed word carried on the worn instance
// (game/dye.h). Nine pieces of art times any colour anybody picks is the whole
// variety budget, and it only works if four separate things stay true. This
// gate is those four, and every one of them fails SILENTLY otherwise:
//
//   1. THE REFERENCE TONE AGREES ACROSS THE SEAM. `kDyeRef` in game/dye.h and
//      `DYE_REF` in assets/shaders/microbody.wgsl are the same number
//      written twice, on opposite sides of a boundary with no compiler between
//      them. Disagreement is not a crash: it is every dyed garment in the game
//      coming out uniformly too bright or too dark, and the cause two files
//      away. This is the check that makes the "stated in three places, checked
//      in two" claim in dye.h true rather than aspirational.
//
//   2. THE ART IS ACTUALLY GREY. A dye multiplies; multiplying a colour by a
//      colour is mud. The day somebody re-authors a shirt with a red stripe,
//      the stripe stops being a stripe and nothing else notices — the piece
//      still loads, still fits, still burns. So every painted cell of every
//      dyeable piece is checked to be a grey, against the MERGED art palette
//      the renderer will actually read (not against the .vox file, which is
//      not what the GPU sees).
//
//   3. THE DYE REACHES THE GPU INSTANCE. It travels
//      ItemStack -> WearItem -> MobLimb -> MicroBodyInstGpu's fourth word, a
//      chain whose only visible symptom of breaking is "my shirt is grey".
//      Checked at the END of it, through the real AppendMicroInsts, because
//      that is the buffer the shader reads.
//
//   4. A STACK IS ONE COLOUR, AND THE COLOUR SURVIVES A SAVE. Merging by def
//      alone would repaint a stack; dropping the word from the payload would
//      bleach the player's wardrobe on every reload. Both are data loss that
//      looks like a rendering bug.
//
// WHAT IT DOES NOT TEST is the PICTURE. That a red tunic renders red is the
// shader's business and no CPU assertion can see it; `--shot-mob` and a human
// are the instrument for that. What this pins is everything the picture
// depends on.

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "game/caster.h"
#include "game/dye.h"
#include "game/equipment.h"
#include "game/item.h"
#include "game/mob.h"
#include "phys/debris.h"
#include "game/persist.h"
#include "game/spell.h"
#include "sim/microbody.h"
#include "test/selftest.h"
#include "test/support.h"

using namespace sandvox;

namespace selftest {
namespace {

// Read a whole file, or "" if it is not there. The gate reports the miss
// itself rather than throwing, because a shader that has moved is a different
// failure from a shader that disagrees and the message has to say which.
std::string SlurpFile(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return {};
  std::string out;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

Status GateDye(Ctx& c, std::string& detail) {
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("dye: FAILED %s\n", what.c_str());
    }
  };

  // ---- 1. the reference tone, across the CPU/GPU seam ----------------------
  {
    const std::string src =
        SlurpFile(AssetDir() + "/shaders/microbody.wgsl");
    if (src.empty()) {
      check(false, "assets/shaders/microbody.wgsl is readable");
    } else {
      // Found by its declaration rather than by line number, so moving the
      // constant inside the file is not a failure — only deleting it or
      // changing its value is.
      const std::string key = "const DYE_REF : f32 = ";
      const size_t at = src.find(key);
      check(at != std::string::npos,
            "microbody.wgsl still declares DYE_REF");
      if (at != std::string::npos) {
        const double gpu = std::atof(src.c_str() + at + key.size());
        check(std::fabs(gpu - (double)kDyeRef) < 1e-6,
              Format("DYE_REF (%.4f) == game/dye.h kDyeRef (%.4f)", gpu,
                     (double)kDyeRef));
      }
    }
    // The tone curve itself, which is the one line of arithmetic the shader
    // and the UI both perform. A cell painted at the reference grey must come
    // out as exactly the dye, or the colour you pick is not the colour you get.
    check(std::fabs(DyeToneMul(kDyeRef) - 1.0f) < 1e-6f,
          "a cell at the reference grey renders as exactly the picked colour");
    check(DyeToneMul(kDyeRef * 0.5f) < 1.0f && DyeToneMul(kDyeRef * 2.0f) > 1.0f,
          "and darker art stays darker, brighter stays brighter");
  }

  // ---- 2. the packing, including the one case a flag exists for ------------
  {
    const uint32_t d = DyePack(0.8f, 0.2f, 0.4f);
    float back[3];
    DyeUnpack(d, back);
    check(DyeSet(d), "a packed dye reads as set");
    check(std::fabs(back[0] - 0.8f) < 0.01f &&
              std::fabs(back[1] - 0.2f) < 0.01f &&
              std::fabs(back[2] - 0.4f) < 0.01f,
          "and round-trips to within a palette step");
    // BLACK IS A COLOUR. Without the flag bit it is indistinguishable from the
    // default, and the symptom is a garment that silently refuses to be dyed
    // black — which reads as the dye system being broken rather than as this
    // one encoding decision.
    const uint32_t black = DyePack(0.0f, 0.0f, 0.0f);
    check(DyeSet(black) && black != 0,
          "black is a dye, not the absence of one");
    check(!DyeSet(0u), "and zero is still 'undyed'");
    // Out of range clamps rather than wraps: a picker that hands back 1.0001
    // must give white, not black.
    float hi[3];
    DyeUnpack(DyePack(1.5f, -0.5f, 1.0f), hi);
    check(hi[0] > 0.99f && hi[1] < 0.01f, "out-of-range components clamp");
    // The swatch conversion main.cpp performs for the inventory panel: ImGui
    // packs 0xAABBGGRR and the dye word is the same 24 bits plus a flag, which
    // is half the reason the byte order is what it is.
    check((0xFF000000u | (d & 0x00FFFFFFu)) ==
              (0xFF000000u | (uint32_t)(back[0] * 255.0f + 0.5f) |
               ((uint32_t)(back[1] * 255.0f + 0.5f) << 8) |
               ((uint32_t)(back[2] * 255.0f + 0.5f) << 16)),
          "the packed word IS an ImGui swatch once the alpha is added");
    check(DyeName(0u) == "undyed", "an undyed item says so");
    check(!DyeName(DyePack(0.85f, 0.15f, 0.12f)).empty(),
          "and a dyed one has a name to print");
  }

  // ---- 3. the shipped wardrobe --------------------------------------------
  //
  // THE MERGED PALETTE, not the .vox files. `PrefabVoxel::color` is a 1-based
  // index into MicroBodySet::artColors after MicroBodyMergeArt has rewritten
  // it (sim/microbody.h) — the renderer reads exactly that table, so that is
  // where "is this cell grey" has to be asked. Reading the .vox instead would
  // pass on a file whose colours never reached the GPU.
  MicroBodySet* mb = c.debris.MicroSet();
  int dyeableChest = 0, dyeableLegs = 0, dyeableFeet = 0;
  int greyCells = 0, colouredCells = 0, refCells = 0;
  std::string firstOffender;
  if (mb == nullptr) {
    check(false, "the micro pool is reachable (art palette lives there)");
  } else {
    // The grey the whole ramp is calibrated against: 179/255 == kDyeRef.
    const uint32_t refGrey = (uint32_t)(kDyeRef * 255.0f + 0.5f);
    for (const ItemDef& it : c.items.items) {
      if (!it.dyeable) continue;
      if (it.kind == ItemKind::ArmorChest) dyeableChest++;
      if (it.kind == ItemKind::ArmorLegs) dyeableLegs++;
      if (it.kind == ItemKind::ArmorBoots) dyeableFeet++;
      check(!it.cover.empty(),
            "'" + it.name + "' is a worn piece with shells to dye");
      for (const ItemCover& cv : it.cover) {
        for (const PrefabVoxel& v : cv.voxels) {
          if (v.color == 0) continue;   // unpainted: takes the material colour
          const size_t idx = (size_t)v.color - 1;
          if (idx >= mb->artColors.size()) continue;
          const uint32_t rgb = mb->artColors[idx];
          const uint32_t r = rgb & 0xFFu, g = (rgb >> 8) & 0xFFu,
                         b = (rgb >> 16) & 0xFFu;
          if (r == g && g == b) {
            greyCells++;
            if (r == refGrey) refCells++;
          } else {
            colouredCells++;
            if (firstOffender.empty())
              firstOffender = Format("%s/%s uses #%02X%02X%02X",
                                     it.name.c_str(), cv.part.c_str(), r, g, b);
          }
        }
      }
    }
    check(greyCells > 0, "the dyeable pieces are painted at all");
    check(colouredCells == 0,
          firstOffender.empty()
              ? std::string("every dyeable cell is a grey (a multiplier)")
              : "every dyeable cell is a grey (a multiplier) — " +
                    firstOffender);
    // AND THE RAMP IS THE ONE THE SHADER IS CALIBRATED FOR. Greyscale alone is
    // not enough: art painted entirely at 0.3 would pass the check above and
    // render every garment at 43% of its dye. At least some of the wardrobe has
    // to sit on the reference tone, or "the colour you pick is the colour you
    // get" is false for all of it.
    check(refCells > 0,
          Format("some cell is painted at the reference grey %u/255", refGrey));
  }
  // THE DELIVERABLE, AS A TEST. Three patterns per slot is what the wardrobe
  // is; a generator run that silently emitted two would leave the panel with a
  // short list and nothing else would notice.
  check(dyeableChest >= 3, Format("at least 3 dyeable shirts (%d)", dyeableChest));
  check(dyeableLegs >= 3, Format("at least 3 dyeable leg pieces (%d)", dyeableLegs));
  check(dyeableFeet >= 3, Format("at least 3 dyeable foot pieces (%d)", dyeableFeet));

  // ---- 4. a stack is one colour -------------------------------------------
  {
    const ItemDef* any = nullptr;
    for (const ItemDef& it : c.items.items)
      if (it.dyeable) any = &it;
    if (any == nullptr) {
      check(false, "the library ships something dyeable to stack");
    } else {
      const int di = c.items.Find(any->name);
      const uint32_t red = DyePack(0.8f, 0.1f, 0.1f);
      const uint32_t blue = DyePack(0.1f, 0.1f, 0.8f);
      Inventory hb;
      const int a = hb.Add(StackOf(c.items, di, 1, red));
      const int b = hb.Add(StackOf(c.items, di, 1, red));
      const int d = hb.Add(StackOf(c.items, di, 1, blue));
      check(a >= 0 && a == b && hb.slots[a].count == 2,
            "two of the same colour stack");
      check(d >= 0 && d != a && hb.slots[d].dye == blue,
            "a different colour takes its own slot instead of repainting one");
      Bag bag;
      check(bag.Add(StackOf(c.items, di, 1, red)) !=
                bag.Add(StackOf(c.items, di, 1, blue)),
            "and the pack obeys the same rule");
    }
  }

  // ---- 5. the dye reaches the GPU instance --------------------------------
  //
  // THROUGH THE REAL PATH: a real def, a real WearItem, and the real
  // AppendMicroInsts that fills the buffer the shader reads. Asserting on
  // MobLimb::dye instead would pass on a build whose AppendMicroInsts had
  // stopped packing the word, which is precisely the seam most likely to be
  // lost in a merge (it is one argument in a brace initialiser).
  MobSystem& mobs = c.mobs;
  int avDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
  const ItemDef* piece = nullptr;
  for (const ItemDef& it : c.items.items)
    if (it.dyeable && !it.cover.empty()) { piece = &it; break; }
  if (avDef < 0 || piece == nullptr) {
    detail = Format("%d checks, no rig or no dyeable piece to dress it in",
                    checks);
    std::printf("dye: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
    return ok ? Status::Pass : Status::Fail;
  }
  {
    mobs.Reset();
    // Window-anchored, for the reason armor-wear states at length: a gate that
    // spawns at absolute coordinates is spawning wherever the previous gate
    // left the residency window.
    const IVec3 wOrg = c.world.WindowOrigin();
    const int sx = wOrg.x * (int)kChunk + 150, sz = wOrg.z * (int)kChunk + 150;
    const int h = World::TerrainHeight(sx, sz, kDefaultSeed);
    const uint64_t id = mobs.Spawn(avDef, {sx, h + 1, sz});
    Mob* m = mobs.FindMobById(id);
    if (m == nullptr) {
      check(false, "the fixture rig spawns");
    } else {
      int slot = -1;
      for (int s = 0; s < kEquipSlotCount && slot < 0; s++)
        if (EquipSlotIsWorn(s) && EquipSlotAccepts(s, piece->kind)) slot = s;
      check(slot >= 0, "'" + piece->name + "' has a slot on the body");

      const uint32_t dye = DyePack(0.79f, 0.21f, 0.13f);
      auto countDyed = [&](uint32_t want) {
        std::vector<MicroBodyInstGpu> insts;
        mobs.AppendMicroInsts(insts, 0);
        int n = 0;
        for (const MicroBodyInstGpu& i : insts)
          if (i.dye == want) n++;
        return n;
      };
      auto wornShells = [&]() {
        int n = 0;
        for (int i = m->AppendedBase(); i < m->LimbCount(); i++)
          if (m->LimbDefAt(i).tag == "worn") n++;
        return n;
      };

      const int bare = countDyed(dye);
      check(bare == 0, "an undressed rig emits no dyed instance");

      if (slot >= 0 && m->WearItem(piece, slot, nullptr, dye)) {
        const int shells = wornShells();
        check(shells > 0, "the piece put shells on the rig");
        // EVERY shell of the garment, not merely one: the dye is handed to
        // each in turn (Mob::WearItem), and a sleeve that missed it would be a
        // red shirt with one grey arm.
        check(countDyed(dye) == shells,
              Format("every one of the %d shells carries the dye (%d did)",
                     shells, countDyed(dye)));
        // ...and nothing ELSE does. A body limb that picked the word up would
        // mean the wearer's own skin takes the colour of their shirt.
        std::vector<MicroBodyInstGpu> insts;
        mobs.AppendMicroInsts(insts, 0);
        int dyedTotal = 0;
        for (const MicroBodyInstGpu& i : insts)
          if (i.dye != 0) dyedTotal++;
        check(dyedTotal == shells,
              Format("and only the garment is dyed (%d of %d instances)",
                     dyedTotal, (int)insts.size()));

        // THE SAME PIECE, UNDYED, IS UNDYED. The default has to be the zero
        // word or every creature in the world would be wearing the last colour
        // anybody picked.
        m->UnwearItem(slot);
        check(m->WearItem(piece, slot), "and it goes on again with no dye");
        std::vector<MicroBodyInstGpu> plain;
        mobs.AppendMicroInsts(plain, 0);
        int stillDyed = 0;
        for (const MicroBodyInstGpu& i : plain)
          if (i.dye != 0) stillDyed++;
        check(stillDyed == 0, "leaving nothing dyed");
        m->UnwearItem(slot);
      } else if (slot >= 0) {
        check(false, "'" + piece->name + "' goes on the stock rig");
      }
    }
    mobs.Reset();
  }

  // ---- 6. the colour survives a save --------------------------------------
  //
  // Driven through MakeEntityIO for the reason selftest_playerkit's own
  // round-trip is: a field that is written but whose section is not registered
  // persists nothing, and the two failures look identical from the save side.
  {
    PlayerCaster caster;
    GlyphLibrary glyphs;
    Kit kit;
    Inventory& hb = kit.hotbar;
    const int di = c.items.Find(piece->name);
    const uint32_t red = DyePack(0.77f, 0.18f, 0.11f);
    const uint32_t black = DyePack(0.0f, 0.0f, 0.0f);
    hb.slots[1] = StackOf(c.items, di, 1, red);
    // the flag-bit case, end to end
    kit.bag.slots[3] = StackOf(c.items, di, 1, black);
    int wornSlot = -1;
    for (int s = 0; s < kEquipSlotCount && wornSlot < 0; s++)
      if (EquipSlotIsWorn(s) && EquipSlotAccepts(s, piece->kind)) wornSlot = s;
    if (wornSlot >= 0) kit.equip.slots[wornSlot] = StackOf(c.items, di, 1, red);

    PlayerKitRefs refs{&caster, &glyphs, &kit, &c.items};
    EntityIO io = MakeEntityIO(c.debris, c.mobs, nullptr, &refs);
    const EntitySection* plyr = nullptr;
    for (const EntitySection& s : io.sections)
      if (s.id ==
          (uint32_t)('P' | ('L' << 8) | ('Y' << 16) | ((uint32_t)'R' << 24)))
        plyr = &s;
    check(plyr != nullptr, "the PLYR section is registered");
    if (plyr != nullptr) {
      std::vector<uint8_t> bytes;
      plyr->save(bytes);
      plyr->reset();
      check(hb.slots[1].Empty(), "reset clears the hotbar");
      check(plyr->load(bytes.data(), bytes.size(), kPlayerKitSaveVersion),
            "and the payload loads back");
      check(hb.slots[1].dye == red, "the hotbar's colour came back");
      check(kit.bag.At(3).dye == black,
            "black came back as black rather than as undyed");
      if (wornSlot >= 0)
        check(kit.equip.At(wornSlot).dye == red,
              "and what you were wearing is still the colour you dyed it");
    }
  }

  detail = Format("%d checks, %d shirts/%d legs/%d feet, %d grey cells",
                  checks, dyeableChest, dyeableLegs, dyeableFeet, greyCells);
  std::printf("dye: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- hair-tuck: HAIR UNDER A HOOD (Mob::SyncHairTuck) -----------------------
//
// Lives beside `dye` because it dresses the same kind of rig in the same
// shipped wardrobe. A generated character's hair is its own limb, so a hood
// fitted to the head box used to have every long hairstyle poking through it.
// What is pinned, on a real pool body through the real WearItem:
//   - a hood hides some hair, and hides it ONLY from the brick: the lattice
//     the sim reads (burning, carving, saving) keeps every cell;
//   - a close helm (one eye slit) hides at least as much as an open cowl --
//     the face opening is the thing that keeps a fringe or a beard drawn;
//   - a second sync with nothing changed changes nothing;
//   - taking the headgear off draws every cell again.
// What it does not pin is the PICTURE; `--shot-mob pool/<stem>:+hood` is that.
Status GateHairTuck(Ctx& c, std::string& detail) {
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("hair-tuck: FAILED %s\n", what.c_str());
    }
  };
  MobSystem& mobs = c.mobs;
  // The first pool body whose def has a hair limb on its head. Pool bodies
  // are generated, so which one that is may change; that there is one is the
  // premise of the gate, and a pool without hair is reported, not failed.
  int def = -1;
  std::string stem;
  for (const std::string& name : mobs.PoolNames()) {
    const int d = mobs.PoolDef(name);
    if (d < 0) continue;
    for (const MobLimbDef& ld : mobs.Defs()[(size_t)d].limbs)
      if (ld.bloodless && ld.parent == "head") { def = d; stem = name; break; }
    if (def >= 0) break;
  }
  const int hoodIx = c.items.Find("hood"), helmIx = c.items.Find("iron_helm");
  if (def < 0 || hoodIx < 0 || helmIx < 0) {
    detail = "no pool body with head hair, or no hood/iron_helm item";
    std::printf("hair-tuck: PASS (%s)\n", detail.c_str());
    return Status::Pass;
  }
  const ItemDef* hood = &c.items.items[(size_t)hoodIx];
  const ItemDef* helm = &c.items.items[(size_t)helmIx];
  int slot = -1;
  for (int s = 0; s < kEquipSlotCount && slot < 0; s++)
    if (EquipSlotIsWorn(s) && EquipSlotAccepts(s, hood->kind)) slot = s;
  check(slot >= 0, "the hood has a slot on the body");

  mobs.Reset();
  const IVec3 wOrg = c.world.WindowOrigin();
  const int sx = wOrg.x * (int)kChunk + 150, sz = wOrg.z * (int)kChunk + 150;
  const int h = World::TerrainHeight(sx, sz, kDefaultSeed);
  const uint64_t id = mobs.Spawn(def, {sx, h + 1, sz});
  Mob* m = mobs.FindMobById(id);
  int hiddenHood = -1, hiddenHelm = -1, lattice = 0;
  if (m == nullptr || slot < 0) {
    check(m != nullptr, "the pool body '" + stem + "' spawns");
  } else {
    const Mob::HairTuckProbe bare = m->ProbeHairTuck();
    lattice = bare.latticeCells;
    check(bare.hairLimbs > 0 && bare.latticeCells > 0, "the body has hair");
    check(bare.drawnCells == bare.latticeCells && bare.tuckedLimbs == 0,
          Format("bare, every hair cell is drawn (%d of %d)", bare.drawnCells,
                 bare.latticeCells));

    check(m->WearItem(hood, slot), "the hood goes on");
    const Mob::HairTuckProbe hooded = m->ProbeHairTuck();
    hiddenHood = hooded.latticeCells - hooded.drawnCells;
    check(hooded.latticeCells == bare.latticeCells,
          "the hood hides hair from the brick only, not from the lattice");
    check(hiddenHood > 0 && hooded.tuckedLimbs > 0,
          Format("the hood hides some hair (%d of %d cells)", hiddenHood,
                 hooded.latticeCells));
    m->SyncHairTuck();
    check(m->ProbeHairTuck().drawnCells == hooded.drawnCells,
          "a second sync with nothing changed changes nothing");

    check(m->WearItem(helm, slot), "the helm replaces it");
    const Mob::HairTuckProbe helmed = m->ProbeHairTuck();
    hiddenHelm = helmed.latticeCells - helmed.drawnCells;
    check(hiddenHelm >= hiddenHood,
          Format("a closed helm hides at least what an open cowl does "
                 "(%d vs %d)", hiddenHelm, hiddenHood));

    m->UnwearItem(slot);
    const Mob::HairTuckProbe after = m->ProbeHairTuck();
    check(after.drawnCells == after.latticeCells && after.tuckedLimbs == 0,
          Format("headgear off, every cell is drawn again (%d of %d)",
                 after.drawnCells, after.latticeCells));
  }
  mobs.Reset();
  detail = Format("%d checks, %s: %d hair cells, hood hides %d, helm %d",
                  checks, stem.c_str(), lattice, hiddenHood, hiddenHelm);
  std::printf("hair-tuck: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

// ---- garment-tint: A GARMENT'S MATTER LANDS IN THE GRID IN ITS OWN COLOUR ----
//
// The owner's report: "I destroy clothing that contains absolutely zero purple
// and it creates purple 'robe' voxels". Purple is `cloth`'s tint 0, its
// natural indigo -- what a cloth cell is when nothing told it otherwise. The
// cause was Mob::LimbVoxelsToParticles, the spray every carve of a CREATURE'S
// limb ejects (blade kerf, blast, bite, under-size fragment): it copied the
// voxel's payload verbatim, and a limb voxel's nibble is the positional jitter
// variant 0..2, not a tint. So every garment cut on a body landed as cloth
// tints 0, 1, 2 -- indigo, dark indigo, gold -- whatever it was painted. Loose
// debris had always quantized (DebrisSystem::VoxelsToParticles); the creature
// half never learned. A dyed garment had the twin bug everywhere: its art is
// a grey weave, and the grey, not the dye, was what got quantized.
//
// What is pinned, through the real carve on a real rig:
//   A. the shipped robe (cloth, painted black-and-grey, none of it near
//      cloth's indigo): every tinted voxel its spray deposits carries a tint
//      its own art quantizes to -- never a jitter variant -- and none is tint
//      0, because no colour on the robe is closest to indigo;
//   B. a DYED commoner's tunic (linen grey weave x a red dye): the commonest
//      linen tint in its spray is the tint nearest the DYE (computed here from
//      materials.json's own list, not through the engine's quantizer), and it
//      differs from what the same tunic sprays undyed;
//   C. DebrisSystem's per-path counter names the limb spray and saw no painted
//      voxel land as tint 0 (rule 6: the instrument that would have named this
//      bug in one run).
// CPU only: the spray is the deposit (particles carry the payload verbatim
// into the grid), so no tick is needed to see the colour a cell will take.
Status GateGarmentTint(Ctx& c, std::string& detail) {
  bool ok = true;
  int checks = 0;
  auto check = [&](bool cond, const std::string& what) {
    checks++;
    if (!cond) {
      ok = false;
      std::printf("garment-tint: FAILED %s\n", what.c_str());
    }
  };
  MobSystem& mobs = c.mobs;
  DebrisSystem& debris = c.debris;
  // The quantizer's table is refreshed by DebrisSystem::PreTick; a gate run on
  // its own may not have ticked yet.
  debris.RefreshTintMap();
  int avDef = -1;
  for (size_t i = 0; i < mobs.Defs().size(); i++)
    if (mobs.Defs()[i].name == kAvatarDefName) avDef = (int)i;
  const ItemDef* robe = c.items.At(c.items.Find("robe"));
  const ItemDef* tunic = c.items.At(c.items.Find("tunic"));
  if (avDef < 0 || robe == nullptr || tunic == nullptr) {
    detail = "no rig, robe or tunic to test with";
    std::printf("garment-tint: FAIL (%s)\n", detail.c_str());
    return Status::Fail;
  }
  auto tintedMat = [&](uint32_t m) {
    return m < c.mats.size() && !c.mats[m].tints.empty();
  };
  // NearestTint, restated: plain squared RGB, ties to the lowest index.
  auto nearestTint = [&](uint32_t m, uint32_t rgb) {
    const std::vector<uint32_t>& t = c.mats[m].tints;
    uint32_t best = 0;
    int bestD = INT32_MAX;
    for (size_t i = 0; i < t.size(); i++) {
      const int dr = (int)((rgb >> 16) & 255) - (int)((t[i] >> 16) & 255);
      const int dg = (int)((rgb >> 8) & 255) - (int)((t[i] >> 8) & 255);
      const int db = (int)(rgb & 255) - (int)(t[i] & 255);
      const int d = dr * dr + dg * dg + db * db;
      if (d < bestD) { bestD = d; best = (uint32_t)i; }
    }
    return best;
  };
  const uint32_t minSpawns =
      (uint32_t)BaselineNumber("garmentTintMinSpawns", 8.0);

  // One fixture: a fresh rig wearing `piece` (dyed `dye`), every one of its
  // shells blasted through the real ejecting carve. Returns the tinted spray
  // as (material, nibble) counts and the allowed set per material, which is
  // what the piece's own lattice quantizes to.
  struct Spray {
    std::vector<std::pair<uint32_t, uint32_t>> tinted;  // (mat, nibble)
    std::vector<std::vector<uint8_t>> allowed;          // [mat][nibble]
    uint32_t outside = 0;                                // not in allowed
    std::string firstOutside;
    bool dressed = false;
  };
  auto blast = [&](const ItemDef* piece, uint32_t dye) {
    Spray s;
    s.allowed.assign(c.mats.size(), std::vector<uint8_t>(16, 0));
    mobs.Reset();
    const IVec3 wOrg = c.world.WindowOrigin();
    const int sx = wOrg.x * (int)kChunk + 150, sz = wOrg.z * (int)kChunk + 150;
    const int h = World::TerrainHeight(sx, sz, kDefaultSeed);
    const uint64_t id = mobs.Spawn(avDef, {sx, h + 1, sz});
    Mob* m = mobs.FindMobById(id);
    if (m == nullptr) return s;
    int slot = -1;
    for (int e = 0; e < kEquipSlotCount && slot < 0; e++)
      if (EquipSlotIsWorn(e) && EquipSlotAccepts(e, piece->kind)) slot = e;
    if (slot < 0 || !m->WearItem(piece, slot, nullptr, dye)) return s;
    s.dressed = true;
    std::vector<int> shells;
    for (int p = 0; p < m->WornPieceCount(); p++)
      for (int sl : m->WornSlotsAt(p)) shells.push_back(sl);
    for (int sl : shells) {
      std::vector<PrefabVoxel> lat;
      m->LatticeOfSlot(sl, lat);
      for (const PrefabVoxel& v : lat) {
        const uint32_t mat = v.material & 0xFFFu;
        if (tintedMat(mat))
          s.allowed[mat][debris.GridStateFor(mat, v.color, 0, dye) & 15u] = 1;
      }
    }
    for (int sl : shells) {
      const uint64_t body = mobs.LimbBody(id, sl);
      const uint32_t n = mobs.LimbVoxelCount(id, sl);
      if (body == 0 || n == 0) continue;
      const Vec3 centre =
          (mobs.LimbVoxelPos(id, sl, 0) + mobs.LimbVoxelPos(id, sl, n - 1)) * 0.5f;
      std::vector<ParticleSpawn> spawns;
      mobs.CarveLimbRadial(body, centre, 3.0f, /*ragged=*/false, /*eject=*/true,
                           c.world, spawns);
      for (const ParticleSpawn& p : spawns) {
        const uint32_t mat = p.payload & 0xFFFu, nib = (p.payload >> 12) & 0xFu;
        if (!tintedMat(mat)) continue;
        s.tinted.push_back({mat, nib});
        if (!s.allowed[mat][nib]) {
          if (s.outside++ == 0)
            s.firstOutside = Format("%s tint %u", c.mats[mat].name.c_str(), nib);
        }
      }
    }
    mobs.Reset();
    return s;
  };
  auto modeOf = [](const Spray& s, uint32_t mat) {
    uint32_t hist[16] = {};
    for (const auto& [m, nb] : s.tinted)
      if (m == mat) hist[nb]++;
    uint32_t best = 0;
    for (uint32_t i = 1; i < 16; i++)
      if (hist[i] > hist[best]) best = i;
    return std::make_pair(best, hist[best]);
  };

  debris.ResetTintStats();

  // ---- A. the robe ---------------------------------------------------------
  const Spray rs = blast(robe, 0);
  check(rs.dressed, "the robe goes on the stock rig");
  uint32_t clothId = 0;
  for (size_t i = 0; i < c.mats.size(); i++)
    if (c.mats[i].name == "cloth") clothId = (uint32_t)i;
  uint32_t robeTint0 = 0, robeCloth = 0;
  for (const auto& [m, nb] : rs.tinted)
    if (m == clothId) {
      robeCloth++;
      if (nb == 0) robeTint0++;
    }
  check(robeCloth >= minSpawns,
        Format("the robe's carve sprays cloth (%u voxels, need %u)", robeCloth,
               minSpawns));
  check(rs.outside == 0,
        Format("every tinted voxel the robe sprays carries a tint its own art "
               "quantizes to (%u of %zu did not; first: %s)",
               rs.outside, rs.tinted.size(),
               rs.firstOutside.empty() ? "-" : rs.firstOutside.c_str()));
  check(robeTint0 == 0,
        Format("and none of it lands as cloth's own indigo (%u of %u did)",
               robeTint0, robeCloth));

  // ---- B. the dyed tunic ---------------------------------------------------
  uint32_t linenId = 0;
  for (const auto& cv : tunic->cover)
    for (const PrefabVoxel& v : cv.voxels)
      if (tintedMat(v.material & 0xFFFu)) linenId = v.material & 0xFFFu;
  // A red the dye wheel names: drawn at the reference grey it IS this colour.
  const uint32_t dye = DyePack(0.6f, 0.18f, 0.18f);
  const uint32_t dyeRgb = ((dye & 0xFFu) << 16) | (dye & 0xFF00u) |
                          ((dye >> 16) & 0xFFu);
  const Spray plain = blast(tunic, 0);
  const Spray dyed = blast(tunic, dye);
  check(linenId != 0 && plain.dressed && dyed.dressed,
        "the tunic is a tinted material and goes on");
  const auto pm = modeOf(plain, linenId), dm = modeOf(dyed, linenId);
  const uint32_t want = linenId ? nearestTint(linenId, dyeRgb) : 0;
  check(dm.second >= minSpawns,
        Format("the dyed tunic sprays linen (%u voxels in its commonest tint)",
               dm.second));
  check(dyed.outside == 0,
        Format("every tinted voxel the dyed tunic sprays is one its art x dye "
               "quantizes to (%u did not; first: %s)", dyed.outside,
               dyed.firstOutside.empty() ? "-" : dyed.firstOutside.c_str()));
  check(dm.first == want,
        Format("its commonest linen tint is the one nearest the dye #%06x "
               "(tint %u wanted, %u landed)", dyeRgb, want, dm.first));
  check(want != pm.first,
        Format("and the dye is visible: undyed the same tunic lands as tint %u",
               pm.first));

  // ---- C. the attribution counter ------------------------------------------
  const DebrisSystem::TintPathStats ls =
      debris.TintStats(DebrisSystem::kTintLimbSpray);
  check(ls.deposits > 0 && ls.painted > 0,
        Format("the limb-spray counter saw the deposits (%u, %u painted)",
               ls.deposits, ls.painted));
  check(ls.paintedTint0 == 0,
        Format("and no painted limb voxel landed as tint 0 (%u did)",
               ls.paintedTint0));

  // The carves' gobbets are debris bodies; the gates after this one count
  // bodies, so none may outlive it.
  debris.Reset();
  RecordObserved("garmentTintRobeCloth", (double)robeCloth);
  RecordObserved("garmentTintDyedMode", (double)dm.first);
  detail = Format(
      "%d checks; robe: %u cloth voxels sprayed, %u indigo, %u off-art; dyed "
      "tunic #%06x: mode tint %u (want %u, undyed %u), %u off-art; limb-spray "
      "counter %u deposits / %u painted / %u painted->tint0",
      checks, robeCloth, robeTint0, rs.outside, dyeRgb, dm.first, want,
      pm.first, dyed.outside, ls.deposits, ls.painted, ls.paintedTint0);
  std::printf("garment-tint: %s (%s)\n", ok ? "PASS" : "FAIL", detail.c_str());
  return ok ? Status::Pass : Status::Fail;
}

}  // namespace

const std::vector<Gate>& DyeGates() {
  static const std::vector<Gate> g = {
      // Needs mob defs, physics and real terrain to stand a rig on, so it
      // declares the same dependency the other wearing gates do rather than
      // assuming whatever the previous gate left standing.
      {"dye", "equipment", {"prefab"}, false, GateDye},
      {"hair-tuck", "equipment", {"prefab"}, false, GateHairTuck},
      {"garment-tint", "equipment", {"prefab"}, false, GateGarmentTint},
  };
  return g;
}

}  // namespace selftest
