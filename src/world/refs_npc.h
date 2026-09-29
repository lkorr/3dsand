// refs_npc.h — NPC RESIDENTS: villagers with a home and a day
// (docs/PLAN_world_editor.md §2.6 / §5 P7, DESIGN.md §16.P7).
//
// Two ref kinds and the layer that drives them:
//
//   npc      A villager. Spawns the mob def named by `base` (default "human")
//            -- dressed from props.outfit, armed from props.weapon, on the
//            behaviour profile props.behavior (default "villager") -- the
//            first time the ref comes into the window, AT THE PLACE ITS
//            SCHEDULE SAYS IT IS NOW (catch-up), carrying the ref id
//            (Mob::RefId). The use verb on it begins its props.dialogue.
//            props: name, schedule, dialogue, home, bed, work (and any other
//            ROLE a schedule row names: "tavern": "harrowby/alehouse/hearth"),
//            outfit ["tunic#B4472A", "breeches", ...], weapon "cleaver",
//            behavior "villager".
//   waynode  A point on the walking graph. props.links: the nodes it joins,
//            by ref id -- or, inside a structure, by SLOT NAME (a sibling
//            under the same parent: "waynode_room_0" on
//            "harrowby/smithy/waynode_front_0_in" means
//            "harrowby/smithy/waynode_room_0"). props.autoLink: metres
//            (default 0 = off).
//
// THE WAYNODE LINKING RULE (how houses join the village):
//   1. A link is TWO-WAY: written on either end, it joins both. So an outdoor
//      node joins a house by naming the house's outside-door node --
//      "links": ["harrowby/smithy/waynode_front_0_out"] -- and the house's own
//      file never has to know the village exists.
//   2. A name is resolved as: an exact ref id; else a sibling slot (the same
//      parent id + "/" + name); else a ref in the same group. Anything else is
//      a warning on the References page and no edge.
//   3. AUTO-LINK is opt-in per node: props.autoLink = R metres joins that
//      node to every waynode within R metres (3D, at most 2 m apart in
//      height). There is NO line-of-sight test when the graph is built -- the
//      world's voxels are not on the CPU -- so an auto edge that an NPC then
//      fails to walk (no progress for 5 s) is dropped FOR THAT NPC and its
//      route re-planned. Explicit links are the reliable tool; autoLink is
//      for a village green where every node sees every other.
//   4. An edge whose segment crosses a DOOR's leaf box (any door ref) is a
//      DOOR EDGE: the NPC stops before it, opens the door through the door
//      kind's own onUse (the player's path), waits until it stands open,
//      walks through, and closes it behind itself once clear of the swing.
//
// THE DAY. Each tick (the npc kind's tick, inside TickRefs, BEFORE
// mobs.PreTick) every active villager with a live body: the schedule row for
// the clock (game/schedule.h, integer minutes from the day phase) -> its
// anchor (ResolveAnchor) -> a route (nearest waynode to the feet, Dijkstra
// over the graph, nearest waynode to the anchor, then the anchor) -> the next
// point for the feet, written into ai::Brain::routine. The arbiter's activity
// verbs walk ONE leg at a time on the local navigator, so combat and flight
// still interrupt (the villager profile's weights). A conversation holds it
// and turns it to the speaker. On arrival: sleep lies down (anim activity
// "sleep"), work/eat/goto face the anchor's yaw, socialize faces the nearest
// other villager, wander strolls between hashed points around the anchor.
//
// DETERMINISM. Every decision is a function of (refs, schedules, clock
// minute, the body's position, door phases); the only randomness is
// rng::Hash3 on (ref hash, tick). No wall clock. Resident state is CPU
// scratch, never saved: after a load a villager simply re-plans from where
// it stands.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "game/schedule.h"
#include "math3d.h"
#include "world/refs.h"

class MobSystem;

namespace refs {

// Called from RegisterAllKinds: `npc` (replacing P1's) and `waynode`.
void RegisterResidentKinds();

// ---- schedules (assets/schedules/*.json) -------------------------------------
// The process library, loaded on first use from <AssetDir>/schedules.
schedule::Library& Schedules();
void ReloadSchedules();   // R in game

// ---- anchors -------------------------------------------------------------------
struct Anchor {
  bool ok = false;
  std::string why;          // !ok: in words ("role 'bed' is not a prop on ...")
  std::string refId;        // the ref it resolved to
  std::string how;          // "role bed", "tag gather", "ref", "home (default)"
  Vec3 foot{};              // where the feet stand, world voxels
  bool haveHeading = false; // face this way on arrival (radians, heading)
  float heading = 0.0f;
  bool bed = false;         // lie down here (a bed ref)
};
// `at`: a row's `at` ("" = home). See schedule.h for the three spellings.
Anchor ResolveAnchor(const RefStore& s, const Ref& npc, const std::string& at);

// ---- the waynode graph -------------------------------------------------------
struct WayGraph {
  struct Node {
    std::string id;
    Vec3 p{};   // foot point: the node's cell centre at its floor
  };
  struct Edge {
    int a = 0, b = 0;
    float cost = 0.0f;
    std::string door;     // a door ref whose leaf the edge crosses ("" none)
    bool autoLinked = false;
  };
  std::vector<Node> nodes;          // id order
  std::vector<Edge> edges;
  std::vector<std::vector<int>> adj;   // node -> edge indices
  std::vector<std::string> problems;   // unresolved links, named
  int Find(const std::string& id) const;
};
// The graph of `s`, rebuilt when the store's revision changes.
const WayGraph& Graph(const RefStore& s);

struct Route {
  std::vector<Vec3> pts;            // the feet go to each in turn; last = the anchor
  std::vector<std::string> nodes;   // parallel: the waynode id ("" = the anchor)
  std::vector<std::string> doorBefore;  // parallel: open this door before walking to pts[i]
  bool viaGraph = false;
  std::string note;
};
// `bad`: edges (node index pairs, a < b) this caller must not use.
Route PlanRoute(const RefStore& s, Vec3 from, Vec3 to,
                const std::set<std::pair<int, int>>* bad = nullptr);

// ---- what a villager is doing (the References page, the gates) -------------
enum class ResidentPhase : uint8_t {
  None,       // no schedule / no body
  Travel,     // walking the route
  DoorWait,   // at a door: opening it, waiting for it to stand open
  AtAnchor,   // arrived: doing the activity
  NoRoute,    // the anchor did not resolve, or every route failed
};
const char* ResidentPhaseName(ResidentPhase p);

struct ResidentStatus {
  std::string refId;
  uint64_t mobId = 0;
  Vec3 foot{};                  // where its feet were last tick
  std::string schedule;
  int minute = 0;               // the clock it last planned against
  int row = -1;                 // -1 = a gap (idles at home) / no schedule
  std::string activity;         // the row's `do` ("goto" in a gap)
  int nextRow = -1, nextFrom = 0;
  Anchor anchor;
  ResidentPhase phase = ResidentPhase::None;
  Route route;
  size_t cursor = 0;
  bool talking = false;         // a player is talking to it (holding)
  bool busy = false;            // the arbiter picked a fight or a flight
  std::string door;             // DoorWait: which door
  std::string note;             // the last thing worth saying
  uint32_t rowSince = 0;        // tick the current row was entered
  uint32_t arrivedTick = 0;     // tick it reached this row's anchor (0 = not yet)
  // Counters (gates): doors this villager opened / closed, anchors reached,
  // routes re-planned after a failed leg.
  uint32_t doorOpens = 0, doorCloses = 0, arrivals = 0, replans = 0;
};
bool ResidentStatusOf(const std::string& refId, ResidentStatus& out);
// Every villager the layer is driving, id order.
std::vector<std::string> Residents();
// The `do` of the villager's current row ("" when it has no schedule or is
// not live): dialogue's `activity` condition (dialogue::Store::activity).
std::string ResidentActivity(const std::string& refId);
// Where the villager should be NOW, for catch-up placement: false when it has
// no schedule or the anchor does not resolve. `foot` is the sole centre.
bool ScheduledPlace(const RefStore& s, const Ref& npc, uint32_t tick, Vec3& foot,
                    float& heading);
// Forget every villager's plan (a load, a gate's reset).
void ResetResidents();

}  // namespace refs
