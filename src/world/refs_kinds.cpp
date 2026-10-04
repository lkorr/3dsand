// refs_kinds.cpp — THE KINDS THIS BUILD KNOWS (world/refs.h RefKindRegistry).
//
// RegisterAllKinds() is the ONE place a package adds its kinds: write a
// `RegisterXxxKinds()` in your own file, declare it below, call it here. The
// registry REPLACES a kind of the same name.
//
// P1 ships one here:
//   marker — a named point ("harrowby/green_well"). Inert: no activation
//            effect, not usable. props.tags (array of strings) is what
//            schedules and editors search by.
// P1's temporary `npc` (spawn the def once, "nothing to say yet") was
// replaced by P7's resident kind (world/refs_npc.cpp), which keeps its
// contract -- one spawn per ref (delta "spawned"), Mob::RefId, an edit
// respawns -- and adds the schedule, the walking graph and the talk.

#include "world/refs.h"
#include "world/structures.h"

namespace refs {

namespace {

void RegisterP1Kinds() {
  // ---- marker ----
  RefKind marker;
  marker.name = "marker";
  marker.validate = [](const Ref& r, std::vector<std::string>& p) {
    if (!r.props.contains("tags")) return;
    const Json& t = r.props["tags"];
    bool ok = t.is_array();
    if (ok)
      for (const Json& v : t) ok &= v.is_string();
    if (!ok) p.push_back("props.tags: must be a list of words, e.g. [\"gather\"]");
  };
  Kinds().Register(std::move(marker));
}

}  // namespace

void RegisterDoorKinds();       // world/refs_doors.cpp (P6)
void RegisterResidentKinds();   // world/refs_npc.cpp (P7)
void RegisterReadableKinds();   // world/refs_readable.cpp (demons D2)

void RegisterAllKinds() {
  static bool done = false;
  if (done) return;
  done = true;
  RegisterP1Kinds();
  // ---- later packages: one line each, AFTER P1 (a later registration of the
  // same name replaces the earlier one) ----
  RegisterDoorKinds();           // P6: door, container, bed
  // P4 after P6: a structure derives door/container/bed CHILD refs
  structures::RegisterStructureKinds();   // P4: structure (world/structures.h)
  RegisterResidentKinds();       // P7: npc, waynode
  RegisterReadableKinds();       // demons D2: readable (a book, a note)
}

}  // namespace refs
