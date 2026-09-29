#pragma once

#include <unordered_map>

#include "game/avatar.h"
#include "game/caster.h"
#include "game/equipment.h"
#include "game/worlditems.h"
#include "game/mob.h"
#include "phys/debris.h"
#include "sim/worldio.h"

namespace dialogue {
class Store;
}

// The ONE place the entity systems register into the save format
// (sim/worldio.h entities.sve). Both the frame loop and the selftest build
// their EntityIO here, so a system added in one place is persistable in both —
// and a system that is NOT registered here is structurally unable to persist,
// which is exactly the audit finding this closes.
//
// Extension path: give the new system SaveState/LoadState (its bytes are its
// own business), pick a fresh FourCC, append a section here. The container
// never changes; older builds skip the unknown section.
//
// `avatar` is nullable (headless paths without a player body): its section is
// simply absent, and a save without it loads the avatar fresh.
//
// LIFETIME: the returned sections capture the systems by reference; the
// EntityIO must not outlive them.

// THE PLAYER'S KIT ('PLYR'): what they are carrying, wearing and have bound.
//
// It is a BUNDLE OF REFERENCES rather than a system with its own SaveState.
// It was written that way because unlike debris/mobs/avatar there WAS no
// object that owned all of it: the hotbar predates the pack, the caster is
// deliberately separate from the player, and the section was assembled from
// whichever main() locals happened to be in scope.
//
// SINCE N5 THAT OBJECT EXISTS. `PlayerSession` (game/session.h) owns the
// caster, the hotbar and the kit, and `PlayerKitOf(session, glyphs, items)`
// builds this struct from it — so a caller names ONE player plus the two
// CONTENT libraries, which are the world's and not the player's, instead of
// naming five things and being trusted to pick five that belong together.
// The struct stays a bundle of references on purpose: the two libraries are
// genuinely not the session's, and a `PlayerSession&` here would put game/
// session.h in every file that saves anything.
//
// EVERYTHING IS STORED BY NAME. Item and glyph slots hold indices into
// ItemLibrary::items / GlyphLibrary::glyphs, both of which are FILE-ORDER
// dependent and change whenever content is edited. A save that stored indices
// would silently hand back a different sword — or a different spell — after
// any edit to items.json or glyphs.json. Names that no longer resolve drop the
// slot with a log line, because content legitimately disappears between saves
// and that is the contract, not a failure.
//
// Nullable: a headless path with no player writes no section, and a save
// without one loads a fresh kit.
//
// W2-M: the hotbar, bag and equipment are ONE Kit, and the kit is the avatar's
// (Mob::kit_). `wearer` is the creature wearing it — the save flushes its worn
// shells' live damage into the stacks first (Mob::KitFlushWorn) and a load
// marks its worn slots stale so the rig re-dresses from what was loaded.
// Nullable: a fixture's bare Kit has nobody wearing it.
struct PlayerKitRefs {
  PlayerCaster* caster = nullptr;
  const GlyphLibrary* glyphs = nullptr;
  Kit* kit = nullptr;
  const ItemLibrary* items = nullptr;
  Mob* wearer = nullptr;

  bool Complete() const {
    return caster && glyphs && kit && items;
  }
};

// Version 2 of the 'PLYR' payload. v1 carried the containers; v2 appends the
// WORN DAMAGE map (game/equipment.h), so a burnt robe comes back burnt.
//
// The bump is real rather than an append-and-hope: v1 payloads are REFUSED,
// not partially applied. A half-loaded kit is worse than a refused one, the
// existing round-trip test asserts exactly that refusal, and a save format
// that silently accepts a shorter payload is how a truncated file turns into
// a player's inventory quietly emptying.
// Version 3 adds the CONDITION SUMMARY to each shell record (voxels at spawn,
// voxels still live). Derivable from the lattice only while the piece is on a
// body, which is exactly when it is not in this file — see WornShellDamage.
// Version 4 appends the GRIMOIRE (docs/PLAN_magic_grammar.md §12c): the
// player's pages (name + words, all by name), then the twenty bound slots as
// (kind, name) pairs. A v3 payload still loads: an empty grimoire, and the ten
// bound names it carries land in bank A. v2 and older are refused as before.
// Version 5 appends the DYES (game/dye.h): one word per slot of each of the
// three containers, in the same order and with the same self-describing counts
// the slots themselves use.
//
// APPENDED RATHER THAN WIDENING THE SLOT RECORDS, which would have been
// tidier. The slot section is the FIRST thing in the payload, so putting a
// fourth field in it moves every byte after it and makes v4 unreadable — and
// the point of the tail-append shape this format already uses (worn damage in
// v2, the grimoire in v4) is that an older payload keeps loading and simply
// stops early. A v4 kit loads with every garment undyed, which is exactly what
// it was.
// Version 6 appends VESSEL CONTENTS (game/container.h): one packed word per
// slot, the dyes' shape again. A v5 kit loads with every flask empty.
// Version 7 (rule-unification W2-M) moves worn damage from the v2 section's
// one-blob-per-item-NAME map onto the SLOTS: the v2 section is written empty
// and a per-slot damage array is appended in the dyes' shape, so two robes in
// one pack keep two sets of holes. A v2..v6 kit loads its by-name damage onto
// every stack of that name, which is what those files meant.
// Version 8 (2026-09-26) appends MIXED vessel contents per slot
// (game/composition.h): up to 16 (material, eighths) portions. The v6 word is
// still written (the main portion) and a v8 reader replaces it; a v7 kit
// loads each vessel as the one portion its word named.
// Version 9 (2026-09-27) appends each slot's vessel STOPPER (one word per
// slot, the dyes' shape) and lets a portion carry the DISSOLVED bit
// (composition.h kDissolvedBit); a v8 kit loads every vessel unstoppered.
constexpr uint32_t kPlayerKitSaveVersion = 9;
constexpr uint32_t kPlayerKitOldestLoadable = 3;

// ITEMS ON THE GROUND ('ITMS'): what is lying around, by name and pose.
//
// A SEPARATE SECTION FROM 'DBRS', even though every ground item IS a debris
// body. Debris saves SHAPES; this saves IDENTITY, and the two have different
// lifetimes — the body may be destroyed and rebuilt by a load, but "that was a
// sword" has to survive. Storing the name in the debris record instead would
// put an item concept inside the physics layer, which is the coupling
// game/worlditems.h exists to avoid.
//
// The bodies themselves are RE-CREATED on load from the item library, not
// restored from DBRS: a ground item is fully described by its name and its
// transform, so re-dropping it is both simpler and immune to a DBRS format
// change. The cost is that a ground item's carved lattice does not survive a
// save — see the note in worlditems.h; it round-trips as authored.
struct WorldItemRefs {
  WorldItems* reg = nullptr;
  Physics* phys = nullptr;
  DebrisSystem* debris = nullptr;
  MicroBodySet* micro = nullptr;
  const ItemLibrary* items = nullptr;

  bool Complete() const { return reg && phys && debris && items; }
};

// v2 inserts the DYE (game/dye.h) after each entry's name: a dropped red tunic
// and a dropped blue one share a name and a lattice, so the colour is the only
// thing telling them apart and nothing else in the record implies it. v1 still
// loads — its items come back undyed, which is what they were.
// v3 inserts the vessel FILL word after the dye (the ItemInstance's fill); a
// v2 ground flask loads empty.
// v4 (W2-M) appends each entry's worn DAMAGE after its lattice (every shell of
// the piece; the body is only its largest panel). A v3 item loads as
// authored-under-its-lattice, which is what it was.
// v5 (2026-09-26) appends each entry's MIXED contents after its damage and
// a v5 reader replaces the v3 word with it; a v4 flask loads as one portion.
// v6 (2026-09-27) appends each entry's vessel STOPPER word after its
// contents; a v5 ground flask loads unstoppered.
constexpr uint32_t kWorldItemSaveVersion = 6;

// The 'PLYR' serializer, exposed so the grimoire gate can write an OLDER
// version's payload (everything up to that version's last block) and prove
// the loader still takes it.
void SavePlayerKit(const PlayerKitRefs& r, std::vector<uint8_t>& out,
                   uint32_t version = kPlayerKitSaveVersion);

// 'MOBG' v1: the mob id counter (u64 as two u32), world.sve. 'TIME' v1: the
// celestial clock (u32 engaged, i64 scaleNum, scaleDen, ticks, rem,
// prevTicks), world.sve. Both S4 (PLAN_save_system.md); persist.cpp says why.
constexpr uint32_t kMobGlobalSaveVersion = 1;
constexpr uint32_t kWorldTimeSaveVersion = 1;

// WHICH FILE EACH SECTION LIVES IN (S4, sim/worldio.h layout):
//   world.sve          DBRS, MOBG, TIME, WTRB, DLGF
//   players/<id>.svp   AVTR, PLYR
//   r_x_y_z.sve        MOBS, ITMS -- one record per creature / item
// Every section keeps its whole-payload save/load, so a pre-S4 entities.sve
// (all of them in one file) still loads, and so do the gates that round-trip
// one section's bytes.
//
// 'DLGF' v1 (world.sve, GLOBAL): the dialogue store's FLAGS and MET set
// (game/dialogue.h). Registered only when `talk` is given; its reset clears
// both, so a save without the section loads a world where nobody has been
// spoken to. The conversations themselves are content and are not saved.
EntityIO MakeEntityIO(DebrisSystem& debris, MobSystem& mobs,
                      PlayerAvatar* avatar,
                      const PlayerKitRefs* player = nullptr,
                      const WorldItemRefs* ground = nullptr,
                      dialogue::Store* talk = nullptr);

// THE SKY AFTER A LOAD, when the sim clock could not follow the save.
//
// main.cpp resumes its sim tick at the saved one only when that moves it
// FORWARD (the stamp-nibble and tick-tag arguments there). An in-session
// reload of an older save therefore keeps the later tick, and with the
// celestial clock disengaged the sky IS the sim tick -- the sun would stay
// where this session had it, not where the save had it. Call this after the
// resume with the save's tick (WorldStamp::tick, only when `known`): if the
// clock is disengaged and the two ticks differ, it engages the clock at 1x at
// the saved position, so time of day round-trips whatever the sim tick did.
// A clock the 'TIME' section restored as engaged already holds the saved
// position and is left alone. Returns whether it engaged.
bool ResumeWorldClock(uint32_t savedSimTick, uint32_t simTickNow);

// ---- NPCs OUTLIVE THE WINDOW (docs/PLAN_save_system.md S5b) -----------------
//
// PARK: MobSystem's out-of-window branch hands the creature's Mob::SaveOne
// record to the function Bind installs, which appends it -- as a 'MOBS'
// EntityRecord at the creature's origin, the exact record a save writes -- to
// the region bucket holding that origin (ChunkStore::DormantEntities). The
// rig is then torn down as it always was. Nothing else is needed for a save:
// SaveWorld already writes every bucket as dormant + live, so a parked
// creature reaches disk with the next save and a load leaves it parked unless
// the window holds it.
//
// UNPARK: Unpark walks the buckets of the regions the window intersects and
// makes live (MobSystem::LoadOne, the load path) the 'MOBS' records whose
// origin is inside the window by at least kUnparkMarginChunks on every face.
// The margin is the hysteresis against the park pad (16 voxels OUTSIDE the
// window): a creature must travel two chunks to flip back, so one standing on
// the window edge cannot park and unpark on alternate ticks.
//
// Three things can make a record WAIT (it stays parked, untouched, and is
// retried on a later call -- never dropped):
//   * THE BUDGET. At most `budget` creatures per call (kUnparkPerCall by
//     default; main calls once per tick). A window shift into a crowded town
//     is a Spawn + BuildRig + Jolt bodies per creature; spread over ticks it
//     is a trickle rather than a frame spike.
//   * THE CAP. MobSystem::HasRoomToSpawn is false: the live crowd is full.
//     Parked creatures never hold a cap slot; they come back when one frees.
//   * THE GROUND. The chunks under the creature's footprint (its min corner
//     and half a chunk on in x and z), at the feet and one below, must be in
//     the ON-DEMAND FETCH CACHE (World::Cached, the store the mob
//     ground probe reads) at a version no older than the tick this system
//     first asked for it. The CPU never holds the whole window (CLAUDE.md
//     "CPU mirror is 3x3x3 chunks"), and a cached copy from before the chunk
//     last left the window is stale terrain -- so the first sight of a chunk
//     issues a fetch (FetchSource::Mob, coalesced) and the creature is
//     placed only once the answer is back. SenseGround already makes gravity
//     wait on unknown ground; this makes the creature not EXIST until the
//     ground it stands on is known, so neither the drive nor the static-
//     collider sweep ever meets a hole.
//
// OUT OF SCOPE (follow-ups): parked creatures do not tick -- no off-screen
// travel, hunger or schedule; they stand frozen where they left the window.
// Ground items (ITMS) do not park. Multiplayer: see Bind.
class MobParking {
 public:
  // Creatures made live per Unpark call. main.cpp calls once per sim tick, so
  // this is creatures per tick: a crowd of 16 (MobSystem's cap) is back in
  // eight ticks, ~0.13 s, and no tick pays for more than two rig builds.
  static constexpr uint32_t kUnparkPerCall = 2;
  // Unpark only well inside the window (see the hysteresis note above).
  static constexpr int kUnparkMarginChunks = 1;

  struct Stats {
    uint64_t parked = 0;       // records appended by the park function
    uint64_t unparked = 0;     // records made live
    uint64_t failed = 0;       // records LoadOne refused (dropped, logged)
    uint64_t groundWaits = 0;  // a record left parked: its ground not known yet
    uint64_t capWaits = 0;     // ...the live crowd was full
    uint64_t budgetWaits = 0;  // ...this call's budget was spent
    uint64_t spawnWaits = 0;   // ...Spawn refused it (rig/pool): kept, retried
    uint32_t lastCall = 0;     // creatures made live by the last call
    uint32_t maxCall = 0;      // the most any one call made live
  };

  // Install (enabled) or remove (disabled) the park function on `mobs`,
  // writing into `store`. Idempotent: a call that changes nothing costs a
  // compare, so the frame loop may call it every tick.
  //
  // MULTIPLAYER (M9): parking is decided by the machine that OWNS the
  // creature (MobSystem only parks a non-ghost; the ownership rule has
  // already handed a creature to any peer whose window holds it), and main
  // enables it only on a machine whose store is the world's -- single player
  // and the host. A client's store is not what the host saves, so a client
  // that parked would hide the creature from the world file; a client keeps
  // the pre-S5b despawn (and its MobGone), which is no worse than before.
  // KNOWN GAP: a creature the host parked comes back only when the HOST's
  // window reaches it; a client walking into that region alone does not see
  // it (that needs the host to unpark on the client's behalf and hand off).
  void Bind(MobSystem& mobs, ChunkStore& store, bool enabled);
  // See the class comment. `tick` is the sim tick just run (the fetch-cache
  // freshness bound is in ticks). Returns creatures made live.
  uint32_t Unpark(MobSystem& mobs, ChunkStore& store, World& world,
                  uint32_t tick, uint32_t budget = kUnparkPerCall);
  // Forget the ground-wait bookkeeping and the "nothing to do" shortcut
  // (a load, a regen, a teleporting gate). The binding is kept.
  void ResetWaits();
  const Stats& GetStats() const { return stats_; }
  void ResetStats() { stats_ = Stats{}; }

 private:
  bool enabled_ = false;
  MobSystem* boundMobs_ = nullptr;
  ChunkStore* boundStore_ = nullptr;
  // Skip the region walk when the window has not moved and nothing inside it
  // was left waiting: the steady state costs one compare per tick.
  bool haveOrigin_ = false;
  IVec3 lastOrigin_{};
  bool pending_ = true;
  // Per CHUNK: the tick this system first asked for it. A cached copy is
  // trusted only at or after that tick. Pruned to the window on every shift,
  // so a chunk that leaves and re-enters asks again.
  struct Wait {
    IVec3 wc{};
    uint32_t since = 0;
  };
  std::unordered_map<uint64_t, Wait> waits_;
  Stats stats_;
  bool GroundKnown(World& world, IVec3 wc, uint32_t tick);
};
