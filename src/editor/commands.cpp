// commands.cpp — see commands.h. The built-in commands, the session (undo /
// redo / journal) and the --edit-script runner.

#include "editor/commands.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "sim/world.h"
#include "world/refs_npc.h"
#include "world/structures.h"

namespace editor {

namespace fs = std::filesystem;

namespace {

// ---- argument helpers: every refusal names the field ----------------------------

bool GetVec(const Json& a, const char* k, IVec3& v, std::string& err) {
  if (!a.contains(k)) {
    err = std::string(k) + ": missing (want [x, y, z], whole voxels)";
    return false;
  }
  const Json& j = a[k];
  if (!j.is_array() || j.size() != 3 || !j[0].is_number() || !j[1].is_number() ||
      !j[2].is_number()) {
    err = std::string(k) + ": must be [x, y, z] (whole voxels), got " + j.dump();
    return false;
  }
  v = {j[0].get<int>(), j[1].get<int>(), j[2].get<int>()};
  return true;
}

bool GetStr(const Json& a, const char* k, std::string& v, std::string& err, bool required = true) {
  if (!a.contains(k)) {
    if (required) err = std::string(k) + ": missing (want a string)";
    return !required;
  }
  if (!a[k].is_string()) {
    err = std::string(k) + ": must be a string, got " + a[k].dump();
    return false;
  }
  v = a[k].get<std::string>();
  return true;
}

bool GetInt(const Json& a, const char* k, int& v, std::string& err) {
  if (!a.contains(k)) return true;   // optional: keep the default
  if (!a[k].is_number()) {
    err = std::string(k) + ": must be a number, got " + a[k].dump();
    return false;
  }
  v = a[k].get<int>();
  return true;
}

size_t Edit(const std::string& a, const std::string& b) {
  std::vector<size_t> d(b.size() + 1);
  for (size_t j = 0; j <= b.size(); j++) d[j] = j;
  for (size_t i = 1; i <= a.size(); i++) {
    size_t prev = d[0];
    d[0] = i;
    for (size_t j = 1; j <= b.size(); j++) {
      const size_t t = d[j];
      d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
      prev = t;
    }
  }
  return d[b.size()];
}

// A material by name ("cobble", "air") or id. "did you mean" on a typo.
bool GetMat(const Context& c, const Json& a, const char* k, uint16_t& m, std::string& err) {
  if (!a.contains(k)) {
    err = std::string(k) + ": missing (a material name like \"cobble\", or \"air\")";
    return false;
  }
  const Json& j = a[k];
  if (j.is_number_integer()) {
    const int v = j.get<int>();
    if (v < 0 || v >= (int)c.mats.size()) {
      err = std::string(k) + ": no material has id " + std::to_string(v);
      return false;
    }
    m = (uint16_t)v;
    return true;
  }
  if (!j.is_string()) {
    err = std::string(k) + ": must be a material name or id, got " + j.dump();
    return false;
  }
  const std::string n = j.get<std::string>();
  if (n == "air" || n == "erase") {
    m = 0;
    return true;
  }
  for (size_t i = 1; i < c.mats.size(); i++)
    if (c.mats[i] == n) {
      m = (uint16_t)i;
      return true;
    }
  std::string best;
  size_t bd = 99;
  for (size_t i = 1; i < c.mats.size(); i++) {
    const size_t d = Edit(n, c.mats[i]);
    if (d < bd) {
      bd = d;
      best = c.mats[i];
    }
  }
  err = std::string(k) + ": \"" + n + "\" is not a material in materials.json" +
        (bd <= 3 ? " (did you mean \"" + best + "\"?)" : "");
  return false;
}

std::string MatName(const Context& c, uint16_t m) {
  return m < c.mats.size() ? c.mats[m] : std::to_string(m);
}

refs::RefStore* Store(Context& c, std::string& err) {
  if (c.refs == nullptr) err = "no map's references are loaded (a harness map, or a tool run without --map)";
  return c.refs;
}

// ---- the structure a vox/slot command edits, and its frame ------------------------

struct Target {
  StructEdit* se = nullptr;
  bool world = false;          // args are world voxels
  IVec3 pos{};                 // the instance (world frame)
  int yaw = 0;
};

bool GetTarget(Context& c, const Json& a, Target& t, std::string& err) {
  std::string name = c.active;
  if (a.contains("struct")) {
    if (!GetStr(a, "struct", name, err)) return false;
  }
  if (name.empty()) {
    err = "no structure is open: run struct.open {\"ref\": \"<a structure ref id>\"} (or "
          "{\"asset\": \"<name>\"}) first, or name one with \"struct\"";
    return false;
  }
  t.se = c.Load(name, err);
  if (t.se == nullptr) return false;
  std::string frame = "house";
  if (!GetStr(a, "frame", frame, err, false)) return false;
  if (frame == "world") {
    // A redo carries the instance it was made through (Session::Run).
    std::string instId = name == c.active ? c.instance : std::string();
    if (a.contains("_instance") && a["_instance"].is_string()) instId = a["_instance"].get<std::string>();
    if (instId.empty() || c.refs == nullptr) {
      err = "frame: \"world\" needs the structure opened through a placed ref (struct.open "
            "{\"ref\": ...}); this one was opened by asset name, so use house coordinates";
      return false;
    }
    const refs::Ref* r = c.refs->Find(instId);
    if (r == nullptr) {
      err = "frame: the instance " + instId + " is gone";
      return false;
    }
    t.world = true;
    t.pos = r->pos;
    t.yaw = r->yaw;
  } else if (frame != "house") {
    err = "frame: \"" + frame + "\" is not a frame (\"house\" or \"world\")";
    return false;
  }
  return true;
}

IVec3 ToHouse(const Target& t, IVec3 v) { return t.world ? WorldToHouse(v, t.pos, t.yaw) : v; }

bool GetPoint(const Target& t, const Json& a, const char* k, IVec3& v, std::string& err) {
  if (!GetVec(a, k, v, err)) return false;
  v = ToHouse(t, v);
  return true;
}

// A box from min/max (either order), converted to the house frame.
bool GetBox(const Target& t, const Json& a, IVec3& lo, IVec3& hi, std::string& err) {
  IVec3 p, q;
  if (!GetVec(a, "min", p, err) || !GetVec(a, "max", q, err)) return false;
  p = ToHouse(t, p);
  q = ToHouse(t, q);
  lo = {std::min(p.x, q.x), std::min(p.y, q.y), std::min(p.z, q.z)};
  hi = {std::max(p.x, q.x), std::max(p.y, q.y), std::max(p.z, q.z)};
  const int64_t vol = (int64_t)(hi.x - lo.x + 1) * (hi.y - lo.y + 1) * (hi.z - lo.z + 1);
  if (vol > 4 * 1024 * 1024) {
    err = "min/max: the box is " + std::to_string(vol) +
          " cells; one command edits at most 4,194,304 (a .vox is at most 256 on a side)";
    return false;
  }
  return true;
}

// Apply cell changes; the inverse puts the old materials back.
void ApplyCells(Context& c, StructEdit& se, const std::vector<std::pair<IVec3, uint16_t>>& ch,
                std::vector<Command>& inv) {
  auto p = std::make_shared<Payload>();
  std::set<uint64_t> seen;
  for (const auto& [h, m] : ch) {
    const uint16_t prev = se.Get(h);
    if (prev == m) continue;
    if (seen.insert(StructEdit::Key(h)).second) {
      p->cells.push_back(h.x);
      p->cells.push_back(h.y);
      p->cells.push_back(h.z);
      p->cells.push_back(prev);
    }
    se.Set(h, m);
  }
  (void)c;
  if (p->cells.empty()) return;
  Command u{"vox.set_cells", Json{{"struct", se.name}}, p};
  inv.push_back(std::move(u));
}

// ---- the refs half -------------------------------------------------------------------

// Put back a ref exactly as it was (line and row): every ref edit's inverse.
Command PutBack(const refs::Ref& old, int fileIndex = -1) {
  auto p = std::make_shared<Payload>();
  p->ref = std::make_shared<refs::Ref>(old);
  return Command{"_ref.put", Json{{"id", old.id}, {"index", fileIndex}}, p};
}

bool RefArg(Context& c, const Json& a, const char* k, const refs::Ref*& r, std::string& err,
            bool authored = true) {
  refs::RefStore* st = Store(c, err);
  if (st == nullptr) return false;
  std::string id;
  if (!GetStr(a, k, id, err)) return false;
  r = st->Find(id);
  if (r == nullptr) {
    err = std::string(k) + ": no ref \"" + id + "\" in map '" + st->MapName() + "'";
    return false;
  }
  if (authored && !r->derivedFrom.empty()) {
    err = std::string(k) + ": \"" + id + "\" is a slot of the structure " + r->derivedFrom +
          " (derived from its asset, never written to a file): open the house (struct.open) "
          "and edit the slot, or place a separate ref";
    return false;
  }
  return true;
}

// P7's link resolution, one entry: does `e` (written on `holder`) name `id`?
bool LinkNames(const refs::Ref& holder, const std::string& e, const std::string& id) {
  if (e == id) return true;
  const size_t s = holder.id.rfind('/');
  if (s != std::string::npos && holder.id.substr(0, s) + "/" + e == id) return true;   // sibling
  return refs::GroupOfId(holder.id) + "/" + e == id;                                  // same group
}

// ---- slots -------------------------------------------------------------------------

Command SlotsBack(const StructEdit& se) {
  auto p = std::make_shared<Payload>();
  p->json = se.SlotsJson();
  return Command{"_slot.restore", Json{{"struct", se.name}}, p};
}

// Slot props given in world coordinates -> house (boxes and hingeLine).
Json PropsToHouse(const Target& t, const Json& props) {
  if (!t.world || !props.is_object()) return props;
  Json out = props;
  for (auto it = out.begin(); it != out.end(); ++it) {
    Json& v = it.value();
    if (v.is_object() && v.contains("min") && v.contains("max") && v["min"].is_array() &&
        v["max"].is_array() && v["min"].size() == 3 && v["max"].size() == 3) {
      IVec3 a{v["min"][0].get<int>(), v["min"][1].get<int>(), v["min"][2].get<int>()};
      IVec3 b{v["max"][0].get<int>(), v["max"][1].get<int>(), v["max"][2].get<int>()};
      a = ToHouse(t, a);
      b = ToHouse(t, b);
      v["min"] = Json::array({std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)});
      v["max"] = Json::array({std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)});
    }
  }
  return out;
}

// ---- the built-ins ----------------------------------------------------------------------

void RegisterBuiltins(Registry& R) {
  // ======================= references (world voxels) =======================
  R.Register({"ref.place", "{id, kind, base?, pos:[x,y,z], yaw?, props?}",
              "place a new reference (npc, door, container, bed, marker, waynode, structure)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                refs::RefStore* st = Store(c, err);
                if (st == nullptr) return false;
                const Json& a = cmd.args;
                refs::Ref r;
                if (!GetStr(a, "id", r.id, err) || !GetStr(a, "kind", r.kind, err)) return false;
                if (!GetStr(a, "base", r.base, err, false)) return false;
                if (!GetVec(a, "pos", r.pos, err) || !GetInt(a, "yaw", r.yaw, err)) return false;
                if (a.contains("props")) {
                  if (!a["props"].is_object()) {
                    err = "props: must be an object, got " + a["props"].dump();
                    return false;
                  }
                  r.props = a["props"];
                }
                if (r.kind.empty()) {
                  err = "kind: cannot be empty";
                  return false;
                }
                if (r.kind == "structure" && !structures::ValidYaw(r.yaw)) {
                  err = "yaw: " + std::to_string(r.yaw) +
                        " -- structures turn in quarter turns only (0, 90, 180, 270)";
                  return false;
                }
                if (r.kind == "structure" && !structures::AssetExists(c.assetDir, r.base)) {
                  err = "base: assets/structures/" + r.base + ".struct.json does not exist";
                  return false;
                }
                const std::string group = refs::GroupOfId(r.id);
                const bool newGroup = st->Group(group) == nullptr;
                if (!refs::Place(*st, r, &err)) return false;
                inv.push_back(Command{"_ref.erase", Json{{"id", r.id}, {"dropGroup", newGroup}}, nullptr});
                return true;
              }});
  R.Register({"ref.move", "{id, pos?:[x,y,z], by?:[dx,dy,dz], yaw?}",
              "move and/or turn a reference (world voxels; yaw in degrees, heading 0 = +Z)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                const refs::Ref* r;
                if (!RefArg(c, cmd.args, "id", r, err)) return false;
                const refs::Ref old = *r;
                IVec3 pos = r->pos;
                int yaw = r->yaw;
                if (cmd.args.contains("pos") && !GetVec(cmd.args, "pos", pos, err)) return false;
                if (cmd.args.contains("by")) {
                  IVec3 d;
                  if (!GetVec(cmd.args, "by", d, err)) return false;
                  pos = {pos.x + d.x, pos.y + d.y, pos.z + d.z};
                }
                if (!GetInt(cmd.args, "yaw", yaw, err)) return false;
                yaw = ((yaw % 360) + 360) % 360;
                if (old.kind == "structure" && !structures::ValidYaw(yaw)) {
                  err = "yaw: " + std::to_string(yaw) + " -- structures turn in quarter turns only";
                  return false;
                }
                if (pos.x == old.pos.x && pos.y == old.pos.y && pos.z == old.pos.z && yaw == old.yaw)
                  return true;   // nothing to do: no undo step
                if (!refs::Move(*c.refs, old.id, pos, yaw, &err)) return false;
                inv.push_back(PutBack(old));
                return true;
              }});
  R.Register({"ref.set_prop", "{id, key, value}  (value null removes the prop)",
              "set or remove one prop of a reference",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                const refs::Ref* r;
                if (!RefArg(c, cmd.args, "id", r, err)) return false;
                std::string key;
                if (!GetStr(cmd.args, "key", key, err)) return false;
                if (!cmd.args.contains("value")) {
                  err = "value: missing (use null to remove the prop)";
                  return false;
                }
                const refs::Ref old = *r;
                const Json& v = cmd.args["value"];
                if (v.is_null() ? !old.props.contains(key)
                                : (old.props.contains(key) && old.props[key] == v))
                  return true;
                if (!refs::SetProp(*c.refs, old.id, key, v, &err)) return false;
                inv.push_back(PutBack(old));
                return true;
              }});
  R.Register({"ref.set_field", "{id, field: \"kind\"|\"base\", value}",
              "change what a reference is (kind) or what it is an instance of (base)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                const refs::Ref* r;
                if (!RefArg(c, cmd.args, "id", r, err)) return false;
                std::string field, value;
                if (!GetStr(cmd.args, "field", field, err) || !GetStr(cmd.args, "value", value, err))
                  return false;
                const refs::Ref old = *r;
                if ((field == "kind" && value == old.kind) || (field == "base" && value == old.base))
                  return true;
                if (!refs::SetField(*c.refs, old.id, field, value, &err)) return false;
                inv.push_back(PutBack(old));
                return true;
              }});
  R.Register({"ref.delete", "{id}", "delete a reference (undo puts it back on the same line)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                const refs::Ref* r;
                if (!RefArg(c, cmd.args, "id", r, err)) return false;
                refs::Ref removed;
                int row = -1;
                // A COPY of the id: Erase frees the map node `r` points into.
                const std::string id = r->id;
                if (!refs::Delete(*c.refs, id, &err, &removed, &row)) return false;
                inv.push_back(PutBack(removed, row));
                return true;
              }});
  R.Register({"ref.duplicate", "{id, as, by?:[dx,dy,dz]}",
              "copy a reference to a new id (default 10 voxels along +x)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                const refs::Ref* r;
                if (!RefArg(c, cmd.args, "id", r, err)) return false;
                refs::Ref n = *r;
                if (!GetStr(cmd.args, "as", n.id, err)) return false;
                IVec3 d{10, 0, 0};
                if (cmd.args.contains("by") && !GetVec(cmd.args, "by", d, err)) return false;
                n.pos = {n.pos.x + d.x, n.pos.y + d.y, n.pos.z + d.z};
                const bool newGroup = c.refs->Group(refs::GroupOfId(n.id)) == nullptr;
                if (!refs::Place(*c.refs, n, &err)) return false;
                inv.push_back(Command{"_ref.erase", Json{{"id", n.id}, {"dropGroup", newGroup}}, nullptr});
                return true;
              }});
  R.Register({"ref.link", "{a, b}",
              "join two waynodes (a link is two-way; written on the authored end)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                const refs::Ref *a, *b;
                if (!RefArg(c, cmd.args, "a", a, err, false) || !RefArg(c, cmd.args, "b", b, err, false))
                  return false;
                if (a->id == b->id) {
                  err = "b: a waynode cannot link to itself";
                  return false;
                }
                for (const refs::Ref* w : {a, b})
                  if (w->kind != "waynode") {
                    err = "\"" + w->id + "\" is a " + w->kind + ", not a waynode";
                    return false;
                  }
                const refs::WayGraph& g = refs::Graph(*c.refs);
                const int ia = g.Find(a->id), ib = g.Find(b->id);
                for (const refs::WayGraph::Edge& e : g.edges)
                  if (!e.autoLinked && ((e.a == ia && e.b == ib) || (e.a == ib && e.b == ia))) {
                    err = a->id + " and " + b->id + " are already linked";
                    return false;
                  }
                // Written on the AUTHORED end (a slot's links belong to its house).
                const refs::Ref* holder = a->derivedFrom.empty() ? a : b->derivedFrom.empty() ? b : nullptr;
                if (holder == nullptr) {
                  err = "both are slots of a structure: link them inside the house (open it, "
                        "slot.set_prop links)";
                  return false;
                }
                const refs::Ref* other = holder == a ? b : a;
                const refs::Ref old = *holder;
                Json links = old.props.contains("links") && old.props["links"].is_array()
                                 ? old.props["links"]
                                 : Json::array();
                links.push_back(other->id);
                if (!refs::SetProp(*c.refs, old.id, "links", links, &err)) return false;
                inv.push_back(PutBack(old));
                return true;
              }});
  R.Register({"ref.unlink", "{a, b}", "remove the link between two waynodes, wherever it is written",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                const refs::Ref *a, *b;
                if (!RefArg(c, cmd.args, "a", a, err, false) || !RefArg(c, cmd.args, "b", b, err, false))
                  return false;
                const refs::Ref ra = *a, rb = *b;
                bool any = false;
                for (int side = 0; side < 2; side++) {
                  const refs::Ref& h = side == 0 ? ra : rb;
                  const std::string& other = side == 0 ? rb.id : ra.id;
                  if (!h.props.contains("links") || !h.props["links"].is_array()) continue;
                  Json links = h.props["links"];
                  bool hit = false;
                  for (size_t i = links.size(); i-- > 0;)
                    if (links[i].is_string() && LinkNames(h, links[i].get<std::string>(), other)) {
                      links.erase(links.begin() + (ptrdiff_t)i);
                      hit = true;
                    }
                  if (!hit) continue;
                  if (!h.derivedFrom.empty()) {
                    err = "the link is written in the structure " + h.derivedFrom +
                          " (slot " + h.id + "): open the house and edit the slot's links";
                    return false;
                  }
                  if (!refs::SetProp(*c.refs, h.id, "links", links.empty() ? Json() : links, &err))
                    return false;
                  inv.push_back(PutBack(h));
                  any = true;
                }
                if (!any) {
                  err = ra.id + " and " + rb.id + " are not linked by a written link (an autoLink "
                                                  "edge is removed by lowering autoLink)";
                  return false;
                }
                return true;
              }});
  // ---- the inverses ----
  R.Register({"_ref.put", "{id, index}", "restore a ref line exactly (undo)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                refs::RefStore* st = Store(c, err);
                if (st == nullptr || !cmd.payload || !cmd.payload->ref) {
                  if (err.empty()) err = "_ref.put: no ref payload";
                  return false;
                }
                const refs::Ref& want = *cmd.payload->ref;
                const refs::Ref* cur = st->Find(want.id);
                const int index = cmd.args.value("index", -1);
                if (cur != nullptr) inv.push_back(PutBack(*cur));
                else inv.push_back(Command{"_ref.erase", Json{{"id", want.id}, {"dropGroup", false}}, nullptr});
                return st->Upsert(want, &err, cur != nullptr ? -1 : index);
              },
              true, true});
  R.Register({"_ref.erase", "{id, dropGroup}", "remove a placed ref (undo of a place)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                refs::RefStore* st = Store(c, err);
                if (st == nullptr) return false;
                std::string id;
                if (!GetStr(cmd.args, "id", id, err)) return false;
                refs::Ref removed;
                int row = -1;
                if (!refs::Delete(*st, id, &err, &removed, &row)) return false;
                if (cmd.args.value("dropGroup", false)) st->DropGroupIfEmpty(removed.group);
                inv.push_back(PutBack(removed, row));
                return true;
              },
              true, true});

  // ======================= structures =======================
  R.Register({"struct.open", "{ref} | {asset}",
              "open a placed structure (or an asset by name) for editing",
              [](Context& c, const Command& cmd, std::vector<Command>&, std::string& err) {
                std::string asset, inst;
                if (cmd.args.contains("ref")) {
                  const refs::Ref* r;
                  if (!RefArg(c, cmd.args, "ref", r, err, false)) return false;
                  if (r->kind != "structure") {
                    err = "ref: \"" + r->id + "\" is a " + r->kind + ", not a structure";
                    return false;
                  }
                  asset = r->base;
                  inst = r->id;
                } else if (!GetStr(cmd.args, "asset", asset, err)) {
                  err = "want {\"ref\": \"<structure ref id>\"} or {\"asset\": \"<name>\"}";
                  return false;
                }
                if (c.Load(asset, err) == nullptr) return false;
                c.active = asset;
                c.instance = inst;
                return true;
              },
              false});
  // A structure from nothing (P8: a well, a bench, a fence -- anything the
  // house generator does not make). Opens an EMPTY buffer under a new asset
  // name; the voxel and slot tools build it in the house frame (y = 0 is the
  // floor, x/z centred by convention) and struct.save writes both files.
  R.Register({"struct.new", "{asset}",
              "start a new, empty structure asset (build it with vox.* / slot.*, then struct.save)",
              [](Context& c, const Command& cmd, std::vector<Command>&, std::string& err) {
                std::string asset;
                if (!GetStr(cmd.args, "asset", asset, err)) return false;
                bool okName = !asset.empty() && asset.front() != '/' && asset.back() != '/' &&
                              std::count(asset.begin(), asset.end(), '/') <= 1;
                for (char ch : asset)
                  okName &= (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' ||
                            ch == '/';
                if (!okName) {
                  err = "asset: \"" + asset + "\" -- lowercase letters, digits and _, optionally "
                        "one folder (\"harrowby_well\", \"props/bench\")";
                  return false;
                }
                const std::string base = c.assetDir + "/structures/" + asset;
                if (c.structs.count(asset) != 0 || std::filesystem::exists(base + ".vox") ||
                    std::filesystem::exists(base + ".struct.json")) {
                  err = "asset: structures/" + asset + " already exists (struct.open it instead)";
                  return false;
                }
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(base).parent_path(), ec);
                auto se = std::make_unique<StructEdit>();
                se->InitNew(asset, ::kVoxelsPerMetre);
                c.structs.emplace(asset, std::move(se));
                c.active = asset;
                c.instance.clear();
                return true;
              },
              false});
  R.Register({"struct.close", "{discard?: bool}",
              "stop editing the open structure (refuses unsaved edits unless discard)",
              [](Context& c, const Command& cmd, std::vector<Command>&, std::string& err) {
                if (c.active.empty()) return true;
                StructEdit* se = c.Load(c.active, err);
                if (se != nullptr && se->Dirty() && !cmd.args.value("discard", false)) {
                  err = c.active + " has unsaved edits: struct.save first, or struct.close "
                                   "{\"discard\": true} to throw them away";
                  return false;
                }
                if (se != nullptr && se->Dirty()) c.structs.erase(c.active);   // discarded
                c.active.clear();
                c.instance.clear();
                return true;
              },
              false});
  R.Register({"struct.save", "{struct?}",
              "write the structure's .vox + .struct.json (handEdited) and re-stamp every copy",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                std::string name = c.active;
                if (cmd.args.contains("struct") && !GetStr(cmd.args, "struct", name, err)) return false;
                if (name.empty()) {
                  err = "no structure is open (struct.open first, or name one with \"struct\")";
                  return false;
                }
                StructEdit* se = c.Load(name, err);
                if (se == nullptr) return false;
                if (!se->Dirty()) return true;   // nothing to write, no undo step
                auto p = std::make_shared<Payload>();
                p->u = se->SavedHash();
                if (!se->Save(c.assetDir, c.mats, err, &p->a, &p->b)) return false;
                inv.push_back(Command{"_struct.restore", Json{{"struct", name}}, p});
                if (c.onAssetSaved) c.onAssetSaved(name);
                return true;
              }});
  R.Register({"_struct.restore", "{struct}", "put a structure's files back (undo of a save)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                std::string name;
                if (!GetStr(cmd.args, "struct", name, err) || !cmd.payload) return false;
                StructEdit* se = c.Load(name, err);
                if (se == nullptr) return false;
                const std::string base = c.assetDir + "/structures/" + name;
                auto now = std::make_shared<Payload>();
                now->u = se->SavedHash();
                for (auto [path, bytes, keep] :
                     {std::tuple<std::string, const std::string*, std::string*>{base + ".vox", &cmd.payload->a, &now->a},
                      {base + ".struct.json", &cmd.payload->b, &now->b}}) {
                  std::ifstream in(path, std::ios::binary);
                  std::ostringstream ss;
                  ss << in.rdbuf();
                  *keep = ss.str();
                  in.close();
                  if (bytes->empty()) {
                    // The undone save was the FIRST (struct.new): there was
                    // no file before it, so there is none after.
                    std::error_code ec;
                    std::filesystem::remove(path, ec);
                    continue;
                  }
                  std::ofstream f(path, std::ios::binary | std::ios::trunc);
                  if (!f) {
                    err = "could not write " + path;
                    return false;
                  }
                  f.write(bytes->data(), (std::streamsize)bytes->size());
                }
                se->SetSavedHash(cmd.payload->u);
                inv.push_back(Command{"_struct.restore", Json{{"struct", name}}, now});
                if (c.onAssetSaved) c.onAssetSaved(name);
                return true;
              },
              true, true});
  R.Register({"struct.revert", "{struct?}",
              "throw away unsaved edits: re-read the structure from disk (clears undo history)",
              [](Context& c, const Command& cmd, std::vector<Command>&, std::string& err) {
                std::string name = c.active;
                if (cmd.args.contains("struct") && !GetStr(cmd.args, "struct", name, err)) return false;
                if (name.empty()) {
                  err = "no structure is open";
                  return false;
                }
                c.structs.erase(name);
                return c.Load(name, err) != nullptr;
              },
              false});

  // ======================= voxels (house frame) =======================
  R.Register({"vox.set", "{pos, mat, frame?}", "set one voxel (mat \"air\" erases)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 p;
                uint16_t m;
                if (!GetTarget(c, cmd.args, t, err) || !GetPoint(t, cmd.args, "pos", p, err) ||
                    !GetMat(c, cmd.args, "mat", m, err))
                  return false;
                ApplyCells(c, *t.se, {{p, m}}, inv);
                return true;
              }});
  R.Register({"vox.set_cells", "{cells: [[x,y,z,mat], ...], frame?}",
              "set a list of voxels (mat as a name or id)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                if (!GetTarget(c, cmd.args, t, err)) return false;
                std::vector<std::pair<IVec3, uint16_t>> ch;
                if (cmd.payload) {
                  const std::vector<int32_t>& v = cmd.payload->cells;
                  for (size_t i = 0; i + 3 < v.size(); i += 4)
                    ch.push_back({{v[i], v[i + 1], v[i + 2]}, (uint16_t)v[i + 3]});
                } else {
                  if (!cmd.args.contains("cells") || !cmd.args["cells"].is_array()) {
                    err = "cells: missing (want [[x, y, z, \"mat\"], ...])";
                    return false;
                  }
                  int i = 0;
                  for (const Json& e : cmd.args["cells"]) {
                    if (!e.is_array() || e.size() != 4 || !e[0].is_number() || !e[1].is_number() ||
                        !e[2].is_number()) {
                      err = "cells[" + std::to_string(i) + "]: want [x, y, z, mat], got " + e.dump();
                      return false;
                    }
                    uint16_t m;
                    const Json one{{"mat", e[3]}};
                    if (!GetMat(c, one, "mat", m, err)) {
                      err = "cells[" + std::to_string(i) + "]: " + err;
                      return false;
                    }
                    ch.push_back({ToHouse(t, {e[0].get<int>(), e[1].get<int>(), e[2].get<int>()}), m});
                    i++;
                  }
                }
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.brush", "{pos, radius, shape?: \"sphere\"|\"cube\", mat, frame?}",
              "paint (or erase with \"air\") a sphere or cube of voxels",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 p;
                uint16_t m;
                int r = 1;
                std::string shape = "sphere";
                if (!GetTarget(c, cmd.args, t, err) || !GetPoint(t, cmd.args, "pos", p, err) ||
                    !GetMat(c, cmd.args, "mat", m, err) || !GetInt(cmd.args, "radius", r, err) ||
                    !GetStr(cmd.args, "shape", shape, err, false))
                  return false;
                if (r < 0 || r > 32) {
                  err = "radius: " + std::to_string(r) + " is outside 0..32";
                  return false;
                }
                if (shape != "sphere" && shape != "cube") {
                  err = "shape: \"" + shape + "\" is not \"sphere\" or \"cube\"";
                  return false;
                }
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int z = -r; z <= r; z++)
                  for (int y = -r; y <= r; y++)
                    for (int x = -r; x <= r; x++)
                      if (shape == "cube" || x * x + y * y + z * z <= r * r + r)
                        ch.push_back({{p.x + x, p.y + y, p.z + z}, m});
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.box_fill", "{min, max, mat, frame?}", "fill a box (mat \"air\" clears it)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 lo, hi;
                uint16_t m;
                if (!GetTarget(c, cmd.args, t, err) || !GetBox(t, cmd.args, lo, hi, err) ||
                    !GetMat(c, cmd.args, "mat", m, err))
                  return false;
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int z = lo.z; z <= hi.z; z++)
                  for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++) ch.push_back({{x, y, z}, m});
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.clear", "{min, max, frame?}", "empty a box (the same as box_fill with air)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 lo, hi;
                if (!GetTarget(c, cmd.args, t, err) || !GetBox(t, cmd.args, lo, hi, err)) return false;
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int z = lo.z; z <= hi.z; z++)
                  for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++) ch.push_back({{x, y, z}, 0});
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.box_hollow", "{min, max, frame?}",
              "empty the INSIDE of a box, keeping its six faces (carve a room out of a block)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 lo, hi;
                if (!GetTarget(c, cmd.args, t, err) || !GetBox(t, cmd.args, lo, hi, err)) return false;
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int z = lo.z + 1; z < hi.z; z++)
                  for (int y = lo.y + 1; y < hi.y; y++)
                    for (int x = lo.x + 1; x < hi.x; x++) ch.push_back({{x, y, z}, 0});
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.box_shell", "{min, max, mat, frame?}",
              "build the six faces of a box in a material (walls + floor + ceiling); inside untouched",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 lo, hi;
                uint16_t m;
                if (!GetTarget(c, cmd.args, t, err) || !GetBox(t, cmd.args, lo, hi, err) ||
                    !GetMat(c, cmd.args, "mat", m, err))
                  return false;
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int z = lo.z; z <= hi.z; z++)
                  for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++)
                      if (x == lo.x || x == hi.x || y == lo.y || y == hi.y || z == lo.z || z == hi.z)
                        ch.push_back({{x, y, z}, m});
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.replace", "{min, max, from, to, frame?}",
              "inside a box, change every voxel of one material to another",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 lo, hi;
                uint16_t from, to;
                if (!GetTarget(c, cmd.args, t, err) || !GetBox(t, cmd.args, lo, hi, err) ||
                    !GetMat(c, cmd.args, "from", from, err) || !GetMat(c, cmd.args, "to", to, err))
                  return false;
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int z = lo.z; z <= hi.z; z++)
                  for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++)
                      if (t.se->Get({x, y, z}) == from) ch.push_back({{x, y, z}, to});
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.line", "{from, to, mat, radius?, frame?}",
              "a straight line of voxels (a beam); radius > 0 makes it a square beam",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 a, b;
                uint16_t m;
                int r = 0;
                if (!GetTarget(c, cmd.args, t, err) || !GetPoint(t, cmd.args, "from", a, err) ||
                    !GetPoint(t, cmd.args, "to", b, err) || !GetMat(c, cmd.args, "mat", m, err) ||
                    !GetInt(cmd.args, "radius", r, err))
                  return false;
                if (r < 0 || r > 8) {
                  err = "radius: " + std::to_string(r) + " is outside 0..8";
                  return false;
                }
                const int n = std::max({std::abs(b.x - a.x), std::abs(b.y - a.y), std::abs(b.z - a.z)});
                if (n > 1024) {
                  err = "from/to: the line is " + std::to_string(n) + " voxels long (at most 1024)";
                  return false;
                }
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int i = 0; i <= n; i++) {
                  // Integer interpolation, rounded half away from zero: the same
                  // cells every run, on every machine.
                  auto lerp = [&](int p, int q) {
                    if (n == 0) return p;
                    const long long num = (long long)(q - p) * i * 2 + (q >= p ? n : -n);
                    return p + (int)(num / (2LL * n));
                  };
                  const IVec3 p{lerp(a.x, b.x), lerp(a.y, b.y), lerp(a.z, b.z)};
                  for (int z = -r; z <= r; z++)
                    for (int y = -r; y <= r; y++)
                      for (int x = -r; x <= r; x++) ch.push_back({{p.x + x, p.y + y, p.z + z}, m});
                }
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.copy", "{min, max, frame?}", "copy a box into the clipboard",
              [](Context& c, const Command& cmd, std::vector<Command>&, std::string& err) {
                Target t;
                IVec3 lo, hi;
                if (!GetTarget(c, cmd.args, t, err) || !GetBox(t, cmd.args, lo, hi, err)) return false;
                Clipboard& cb = c.clip;
                cb.size = {hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
                cb.cells.assign((size_t)cb.size.x * cb.size.y * cb.size.z, 0);
                for (int z = 0; z < cb.size.z; z++)
                  for (int y = 0; y < cb.size.y; y++)
                    for (int x = 0; x < cb.size.x; x++)
                      cb.cells[((size_t)z * cb.size.y + y) * cb.size.x + x] =
                          t.se->Get({lo.x + x, lo.y + y, lo.z + z});
                cb.valid = true;
                cb.from = t.se->name;
                return true;
              },
              false});
  R.Register({"vox.paste", "{pos, rot?: 0..3, mirror?: \"none\"|\"x\"|\"z\", air?: \"keep\"|\"skip\", frame?}",
              "paste the clipboard with its min corner at pos: mirrored, then turned rot x 90 deg",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 p;
                int rot = 0;
                std::string mirror = "none", air = "keep";
                if (!GetTarget(c, cmd.args, t, err) || !GetPoint(t, cmd.args, "pos", p, err) ||
                    !GetInt(cmd.args, "rot", rot, err) || !GetStr(cmd.args, "mirror", mirror, err, false) ||
                    !GetStr(cmd.args, "air", air, err, false))
                  return false;
                const Clipboard& cb = cmd.payload && cmd.payload->clip ? *cmd.payload->clip : c.clip;
                if (!cb.valid) {
                  err = "the clipboard is empty: vox.copy a box first";
                  return false;
                }
                if (mirror != "none" && mirror != "x" && mirror != "z") {
                  err = "mirror: \"" + mirror + "\" is not \"none\", \"x\" or \"z\"";
                  return false;
                }
                if (air != "keep" && air != "skip") {
                  err = "air: \"" + air + "\" is not \"keep\" (paste air too) or \"skip\"";
                  return false;
                }
                rot = ((rot % 4) + 4) % 4;
                const int W = cb.size.x, D = cb.size.z;
                std::vector<std::pair<IVec3, uint16_t>> ch;
                for (int z = 0; z < cb.size.z; z++)
                  for (int y = 0; y < cb.size.y; y++)
                    for (int x = 0; x < cb.size.x; x++) {
                      const uint16_t m = cb.At(x, y, z);
                      if (m == 0 && air == "skip") continue;
                      int mx = mirror == "x" ? W - 1 - x : x, mz = mirror == "z" ? D - 1 - z : z;
                      int w = W, d = D;
                      // Quarter turns about +Y inside the box: (x, z) -> (z, w-1-x).
                      for (int k = 0; k < rot; k++) {
                        const int nx = mz, nz = w - 1 - mx;
                        mx = nx;
                        mz = nz;
                        std::swap(w, d);
                      }
                      ch.push_back({{p.x + mx, p.y + y, p.z + mz}, m});
                    }
                ApplyCells(c, *t.se, ch, inv);
                return true;
              }});
  R.Register({"vox.move", "{min, max, by: [dx,dy,dz], frame?}",
              "move a box of voxels (what is left behind becomes air)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                IVec3 lo, hi, d;
                if (!GetTarget(c, cmd.args, t, err) || !GetBox(t, cmd.args, lo, hi, err) ||
                    !GetVec(cmd.args, "by", d, err))
                  return false;
                if (t.world) {   // a world delta turned into the house frame
                  const IVec3 z0 = WorldToHouse({0, 0, 0}, {0, 0, 0}, t.yaw);
                  const IVec3 dd = WorldToHouse(d, {0, 0, 0}, t.yaw);
                  d = {dd.x - z0.x, dd.y - z0.y, dd.z - z0.z};
                }
                std::vector<std::pair<IVec3, uint16_t>> ch;
                std::vector<std::pair<IVec3, uint16_t>> moved;
                for (int z = lo.z; z <= hi.z; z++)
                  for (int y = lo.y; y <= hi.y; y++)
                    for (int x = lo.x; x <= hi.x; x++) {
                      ch.push_back({{x, y, z}, 0});
                      moved.push_back({{x + d.x, y + d.y, z + d.z}, t.se->Get({x, y, z})});
                    }
                ch.insert(ch.end(), moved.begin(), moved.end());
                // Later writes win: apply in order, recording each cell's FIRST old value.
                auto p = std::make_shared<Payload>();
                std::set<uint64_t> seen;
                for (const auto& [h, m] : ch) {
                  const uint16_t prev = t.se->Get(h);
                  if (seen.insert(StructEdit::Key(h)).second) {
                    p->cells.insert(p->cells.end(), {h.x, h.y, h.z, (int32_t)prev});
                  }
                  t.se->Set(h, m);
                }
                inv.push_back(Command{"vox.set_cells", Json{{"struct", t.se->name}}, p});
                return true;
              }});

  // ======================= slots (house frame) =======================
  R.Register({"slot.add", "{name, kind, pos, yaw?, props?, frame?}",
              "add a slot (door / bed / container / marker / waynode) to the open structure",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                structures::Slot s;
                if (!GetTarget(c, cmd.args, t, err) || !GetStr(cmd.args, "name", s.name, err) ||
                    !GetStr(cmd.args, "kind", s.kind, err) || !GetPoint(t, cmd.args, "pos", s.pos, err) ||
                    !GetInt(cmd.args, "yaw", s.yaw, err))
                  return false;
                std::string why;
                if (!refs::ValidId("x/" + s.name, &why) || s.name.find('/') != std::string::npos) {
                  err = "name: \"" + s.name + "\": " + (why.empty() ? "may not contain '/'" : why);
                  return false;
                }
                if (t.se->FindSlot(s.name) >= 0) {
                  err = "name: the structure already has a slot \"" + s.name + "\"";
                  return false;
                }
                if (t.world) s.yaw = ((s.yaw - t.yaw) % 360 + 360) % 360;
                if (cmd.args.contains("props")) {
                  if (!cmd.args["props"].is_object()) {
                    err = "props: must be an object";
                    return false;
                  }
                  s.props = PropsToHouse(t, cmd.args["props"]);
                }
                inv.push_back(SlotsBack(*t.se));
                std::vector<structures::Slot> all = t.se->Slots();
                all.push_back(std::move(s));
                t.se->SetSlots(std::move(all));
                return true;
              }});
  R.Register({"slot.move", "{name, pos?, by?, yaw?, frame?}",
              "move / turn a slot (its box props move with it)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                std::string name;
                if (!GetTarget(c, cmd.args, t, err) || !GetStr(cmd.args, "name", name, err)) return false;
                const int i = t.se->FindSlot(name);
                if (i < 0) {
                  err = "name: no slot \"" + name + "\" in " + t.se->name;
                  return false;
                }
                std::vector<structures::Slot> all = t.se->Slots();
                structures::Slot& s = all[(size_t)i];
                IVec3 np = s.pos;
                if (cmd.args.contains("pos") && !GetPoint(t, cmd.args, "pos", np, err)) return false;
                if (cmd.args.contains("by")) {
                  IVec3 d;
                  if (!GetVec(cmd.args, "by", d, err)) return false;
                  if (t.world) {
                    const IVec3 z0 = WorldToHouse({0, 0, 0}, {0, 0, 0}, t.yaw);
                    const IVec3 dd = WorldToHouse(d, {0, 0, 0}, t.yaw);
                    d = {dd.x - z0.x, dd.y - z0.y, dd.z - z0.z};
                  }
                  np = {np.x + d.x, np.y + d.y, np.z + d.z};
                }
                int yaw = s.yaw;
                if (cmd.args.contains("yaw")) {
                  if (!GetInt(cmd.args, "yaw", yaw, err)) return false;
                  if (t.world) yaw -= t.yaw;
                  yaw = ((yaw % 360) + 360) % 360;
                }
                const IVec3 d{np.x - s.pos.x, np.y - s.pos.y, np.z - s.pos.z};
                if (d.x == 0 && d.y == 0 && d.z == 0 && yaw == s.yaw) return true;
                inv.push_back(SlotsBack(*t.se));
                s.pos = np;
                s.props = ShiftSlotProps(s.props, d);
                // A turn swings the box props about the slot's pos.
                const int dq = (((yaw - s.yaw) / 90) % 4 + 4) % 4;
                if (dq != 0 && s.props.is_object()) {
                  for (auto it = s.props.begin(); it != s.props.end(); ++it) {
                    Json& v = it.value();
                    auto turn = [&](int x, int z, int* ox, int* oz) {
                      const IVec3 w = HouseToWorld({x - np.x, 0, z - np.z}, {np.x, 0, np.z}, dq * 90);
                      *ox = w.x;
                      *oz = w.z;
                    };
                    if (v.is_object() && v.contains("min") && v.contains("max") && v["min"].is_array() &&
                        v["min"].size() == 3) {
                      int ax, az, bx, bz;
                      turn(v["min"][0].get<int>(), v["min"][2].get<int>(), &ax, &az);
                      turn(v["max"][0].get<int>(), v["max"][2].get<int>(), &bx, &bz);
                      v["min"][0] = std::min(ax, bx);
                      v["min"][2] = std::min(az, bz);
                      v["max"][0] = std::max(ax, bx);
                      v["max"][2] = std::max(az, bz);
                    } else if (it.key() == "hingeLine" && v.is_array() && v.size() == 2) {
                      // A cell CORNER: turn the corner point (as cell x - 0.5 twice).
                      const int cx = v[0].get<int>() * 2 - 1, cz = v[1].get<int>() * 2 - 1;
                      const IVec3 w =
                          HouseToWorld({cx - np.x * 2, 0, cz - np.z * 2}, {np.x * 2, 0, np.z * 2}, dq * 90);
                      v[0] = (w.x + 1) / 2;
                      v[1] = (w.z + 1) / 2;
                    }
                  }
                }
                s.yaw = yaw;
                t.se->SetSlots(std::move(all));
                return true;
              }});
  R.Register({"slot.set_prop", "{name, key, value}  (value null removes it)",
              "set or remove one prop of a slot (box props in the house frame)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                std::string name, key;
                if (!GetTarget(c, cmd.args, t, err) || !GetStr(cmd.args, "name", name, err) ||
                    !GetStr(cmd.args, "key", key, err))
                  return false;
                if (!cmd.args.contains("value")) {
                  err = "value: missing (use null to remove the prop)";
                  return false;
                }
                const int i = t.se->FindSlot(name);
                if (i < 0) {
                  err = "name: no slot \"" + name + "\" in " + t.se->name;
                  return false;
                }
                std::vector<structures::Slot> all = t.se->Slots();
                Json& props = all[(size_t)i].props;
                if (!props.is_object()) props = Json::object();
                const Json v = PropsToHouse(t, Json{{key, cmd.args["value"]}})[key];
                if (v.is_null() ? !props.contains(key) : (props.contains(key) && props[key] == v))
                  return true;
                inv.push_back(SlotsBack(*t.se));
                if (v.is_null()) props.erase(key);
                else props[key] = v;
                t.se->SetSlots(std::move(all));
                return true;
              }});
  R.Register({"slot.remove", "{name}", "remove a slot from the open structure",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                Target t;
                std::string name;
                if (!GetTarget(c, cmd.args, t, err) || !GetStr(cmd.args, "name", name, err)) return false;
                const int i = t.se->FindSlot(name);
                if (i < 0) {
                  err = "name: no slot \"" + name + "\" in " + t.se->name;
                  return false;
                }
                inv.push_back(SlotsBack(*t.se));
                std::vector<structures::Slot> all = t.se->Slots();
                all.erase(all.begin() + i);
                t.se->SetSlots(std::move(all));
                return true;
              }});
  R.Register({"_slot.restore", "{struct}", "put a structure's slot list back (undo)",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                std::string name;
                if (!GetStr(cmd.args, "struct", name, err) || !cmd.payload) return false;
                StructEdit* se = c.Load(name, err);
                if (se == nullptr) return false;
                std::vector<structures::Slot> s;
                if (!StructEdit::SlotsFromJson(cmd.payload->json, s, err)) return false;
                inv.push_back(SlotsBack(*se));
                se->SetSlots(std::move(s));
                return true;
              },
              true, true});

  // ======================= grouping =======================
  R.Register({"batch", "{cmds: [{cmd, args}, ...]}", "several commands as ONE undo step",
              [](Context& c, const Command& cmd, std::vector<Command>& inv, std::string& err) {
                if (!cmd.args.contains("cmds") || !cmd.args["cmds"].is_array()) {
                  err = "cmds: missing (want [{\"cmd\": ..., \"args\": {...}}, ...])";
                  return false;
                }
                std::vector<Command> done;
                int i = 0;
                for (const Json& e : cmd.args["cmds"]) {
                  Command sub;
                  sub.name = e.value("cmd", std::string());
                  sub.args = e.contains("args") ? e["args"] : Json::object();
                  const CommandDef* d = Commands().Find(sub.name);
                  std::string why;
                  std::vector<Command> subInv;
                  if (d == nullptr || !d->apply(c, sub, subInv, why)) {
                    // Roll back what this batch did, newest first.
                    for (size_t k = done.size(); k-- > 0;) {
                      const CommandDef* u = Commands().Find(done[k].name);
                      std::vector<Command> ignore;
                      std::string e2;
                      if (u) u->apply(c, done[k], ignore, e2);
                    }
                    err = "cmds[" + std::to_string(i) + "] " + sub.name + ": " +
                          (d == nullptr ? "no such command" : why);
                    return false;
                  }
                  for (Command& u : subInv) done.push_back(std::move(u));
                  i++;
                }
                for (Command& u : done) inv.push_back(std::move(u));
                return true;
              }});
}

}  // namespace

// ---- context ------------------------------------------------------------------------

StructEdit* Context::Load(const std::string& asset, std::string& err) {
  auto it = structs.find(asset);
  if (it != structs.end()) return it->second.get();
  auto se = std::make_unique<StructEdit>();
  std::vector<std::string> warn;
  if (!se->Load(assetDir, asset, err, warn)) return nullptr;
  for (const std::string& w : warn) std::fprintf(stderr, "editor: %s\n", w.c_str());
  StructEdit* p = se.get();
  structs.emplace(asset, std::move(se));
  return p;
}

// ---- registry -------------------------------------------------------------------------

void Registry::Register(CommandDef d) {
  const std::string n = d.name;
  defs_[n] = std::move(d);
}

const CommandDef* Registry::Find(const std::string& name) const {
  auto it = defs_.find(name);
  return it == defs_.end() ? nullptr : &it->second;
}

std::vector<const CommandDef*> Registry::All() const {
  std::vector<const CommandDef*> v;
  for (const auto& [n, d] : defs_) v.push_back(&d);
  return v;
}

Registry& Commands() {
  static Registry r;
  static bool once = false;
  if (!once) {
    once = true;
    RegisterBuiltins(r);
  }
  return r;
}

std::string CommandListText() {
  std::string s;
  for (const CommandDef* d : Commands().All()) {
    if (d->internal) continue;
    s += d->name + "  " + d->args + "\n      " + d->help + (d->undoable ? "" : "  [not an undo step]") + "\n";
  }
  s += "undo  {}\n      undo the last step\nredo  {}\n      redo the last undone step\n";
  return s;
}

// ---- session ----------------------------------------------------------------------------

bool Session::ApplyOne(const Command& c, std::vector<Command>& inv, std::string& err) {
  const CommandDef* d = Commands().Find(c.name);
  if (d == nullptr) {
    std::string best;
    size_t bd = 99;
    for (const CommandDef* x : Commands().All()) {
      const size_t e = Edit(c.name, x->name);
      if (!x->internal && e < bd) {
        bd = e;
        best = x->name;
      }
    }
    err = "\"" + c.name + "\" is not a command" + (bd <= 4 ? " (did you mean " + best + "?)" : "") +
          "; sandvox --edit-commands lists them";
    return false;
  }
  return d->apply(ctx, c, inv, err);
}

void Session::Push(Entry e) {
  undo_.push_back(std::move(e));
  if (undo_.size() > maxUndo) undo_.erase(undo_.begin());
  redo_.clear();
}

bool Session::Run(const Command& c, std::string* errOut) {
  std::string err;
  std::vector<Command> inv;
  if (!ApplyOne(c, inv, err)) {
    if (errOut) *errOut = err;
    return false;
  }
  rev_++;
  Journal(c);
  const CommandDef* d = Commands().Find(c.name);
  if (d != nullptr && d->undoable && !inv.empty()) {
    // The redo must hit the same house through the same copy even after it
    // was closed: name them in the recorded command.
    Command fwd = c;
    if ((c.name.rfind("vox.", 0) == 0 || c.name.rfind("slot.", 0) == 0 || c.name == "struct.save") &&
        !c.args.contains("struct") && !ctx.active.empty()) {
      fwd.args["struct"] = ctx.active;
      if (!ctx.instance.empty()) fwd.args["_instance"] = ctx.instance;
    }
    if (c.name == "batch" && !ctx.active.empty() && fwd.args.contains("cmds") && fwd.args["cmds"].is_array())
      for (Json& sub : fwd.args["cmds"]) {
        const std::string n = sub.value("cmd", std::string());
        if (n.rfind("vox.", 0) != 0 && n.rfind("slot.", 0) != 0 && n != "struct.save") continue;
        if (!sub.contains("args") || !sub["args"].is_object()) sub["args"] = Json::object();
        if (sub["args"].contains("struct")) continue;
        sub["args"]["struct"] = ctx.active;
        if (!ctx.instance.empty()) sub["args"]["_instance"] = ctx.instance;
      }
    if (c.name == "vox.paste" && !(c.payload && c.payload->clip)) {
      auto pl = std::make_shared<Payload>();
      pl->clip = std::make_shared<Clipboard>(ctx.clip);
      fwd.payload = pl;
    }
    if (groupDepth_ > 0) {
      group_.forward.push_back(fwd);
      for (Command& u : inv) group_.inverse.push_back(std::move(u));
    } else {
      Entry e;
      e.label = c.name;
      e.forward.push_back(fwd);
      e.inverse = std::move(inv);
      Push(std::move(e));
    }
  }
  return true;
}

void Session::BeginGroup(const std::string& label) {
  if (groupDepth_++ == 0) {
    group_ = Entry{};
    group_.label = label;
  }
}

void Session::EndGroup() {
  if (groupDepth_ == 0) return;
  if (--groupDepth_ > 0) return;
  if (!group_.inverse.empty()) Push(std::move(group_));
  group_ = Entry{};
}

void Session::Record(const std::string& label, std::vector<Command> forward,
                     std::vector<Command> inverse, const std::vector<Command>& journal) {
  if (inverse.empty()) return;
  for (const Command& j : journal) Journal(j);
  rev_++;
  if (groupDepth_ > 0) {
    for (Command& f : forward) group_.forward.push_back(std::move(f));
    for (Command& u : inverse) group_.inverse.push_back(std::move(u));
    return;
  }
  Entry e;
  e.label = label;
  e.forward = std::move(forward);
  e.inverse = std::move(inverse);
  Push(std::move(e));
}

bool Session::Undo(std::string* msg) {
  if (groupDepth_ > 0) EndGroup();
  if (undo_.empty()) {
    if (msg) *msg = "nothing to undo";
    return false;
  }
  Entry e = std::move(undo_.back());
  undo_.pop_back();
  std::string err;
  for (size_t i = e.inverse.size(); i-- > 0;) {
    std::vector<Command> ignore;
    if (!ApplyOne(e.inverse[i], ignore, err)) {
      if (msg) *msg = "undo of " + e.label + " failed part-way: " + err;
      redo_.clear();
      rev_++;
      return false;
    }
  }
  rev_++;
  JournalNote(Json{{"cmd", "undo"}});
  if (msg) *msg = "undid " + e.label;
  redo_.push_back(std::move(e));
  return true;
}

bool Session::Redo(std::string* msg) {
  if (redo_.empty()) {
    if (msg) *msg = "nothing to redo";
    return false;
  }
  Entry e = std::move(redo_.back());
  redo_.pop_back();
  Entry again;
  again.label = e.label;
  again.forward = e.forward;
  std::string err;
  for (const Command& f : e.forward) {
    std::vector<Command> inv;
    if (!ApplyOne(f, inv, err)) {
      if (msg) *msg = "redo of " + e.label + " failed: " + err;
      rev_++;
      return false;
    }
    for (Command& u : inv) again.inverse.push_back(std::move(u));
  }
  rev_++;
  JournalNote(Json{{"cmd", "redo"}});
  if (msg) *msg = "redid " + e.label;
  undo_.push_back(std::move(again));
  return true;
}

void Session::ClearHistory() {
  undo_.clear();
  redo_.clear();
  groupDepth_ = 0;
  group_ = Entry{};
  rev_++;
}

std::vector<std::string> Session::UnsavedStructures() const {
  std::vector<std::string> v;
  for (const auto& [n, se] : ctx.structs)
    if (se && se->Dirty()) v.push_back(n);
  return v;
}

bool Session::OpenJournal(const std::string& path, std::string* err) {
  journalPath_.clear();
  if (path.empty()) return true;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream f(path, std::ios::binary | std::ios::app);
  if (!f) {
    if (err) *err = "could not open " + path;
    return false;
  }
  journalPath_ = path;
  return true;
}

void Session::JournalNote(const Json& line) {
  if (journalPath_.empty()) return;
  std::ofstream f(journalPath_, std::ios::binary | std::ios::app);
  f << line.dump() << "\n";
}

void Session::Journal(const Command& c) {
  if (journalPath_.empty()) return;
  const CommandDef* d = Commands().Find(c.name);
  if (d != nullptr && d->internal) return;
  Json line{{"cmd", c.name}, {"args", c.args}};
  if (c.payload && c.name == "vox.set_cells" && !c.args.contains("cells")) {
    Json cells = Json::array();
    const std::vector<int32_t>& v = c.payload->cells;
    for (size_t i = 0; i + 3 < v.size(); i += 4) cells.push_back({v[i], v[i + 1], v[i + 2], v[i + 3]});
    line["args"]["cells"] = cells;
  }
  JournalNote(line);
}

// ---- the script runner -----------------------------------------------------------------

bool RunScript(Session& s, const std::string& path, ScriptResult& out, bool save) {
  out = ScriptResult{};
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    out.error = path + ": cannot open";
    return false;
  }
  const size_t depth0 = s.UndoDepth();
  const size_t maxWas = s.maxUndo;
  s.maxUndo = SIZE_MAX;
  struct Restore {
    Session& s;
    size_t m;
    ~Restore() {
      s.maxUndo = m;
    }
  } restoreMax{s, maxWas};
  auto rollback = [&]() {
    while (s.UndoDepth() > depth0) {
      std::string m;
      if (!s.Undo(&m)) break;
    }
  };
  std::string line;
  int n = 0;
  while (std::getline(f, line)) {
    n++;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    size_t a = line.find_first_not_of(" \t");
    if (a == std::string::npos || line[a] == '#' || line.compare(a, 2, "//") == 0) continue;
    out.lines++;
    Json j;
    try {
      j = Json::parse(line.substr(a));
    } catch (const std::exception& e) {
      out.error = path + ":" + std::to_string(n) + ": not valid JSON (" + e.what() + ")";
      rollback();
      return false;
    }
    if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) {
      out.error = path + ":" + std::to_string(n) +
                  ": want {\"cmd\": \"<name>\", \"args\": {...}} (or the args inline)";
      rollback();
      return false;
    }
    const std::string name = j["cmd"].get<std::string>();
    std::string err;
    bool ok;
    if (name == "undo") {
      ok = s.Undo(&err);
    } else if (name == "redo") {
      ok = s.Redo(&err);
    } else {
      Json args = Json::object();
      if (j.contains("args")) args = j["args"];
      else
        for (auto it = j.begin(); it != j.end(); ++it)
          if (it.key() != "cmd") args[it.key()] = it.value();
      const CommandDef* d = Commands().Find(name);
      if (d != nullptr && d->internal) {
        err = "\"" + name + "\" is internal (an undo step), not for scripts";
        ok = false;
      } else {
        ok = s.Run(Command{name, args, nullptr}, &err);
      }
    }
    if (!ok) {
      out.error = path + ":" + std::to_string(n) + ": " + name + ": " + err;
      rollback();
      return false;
    }
    out.applied++;
  }
  if (save) {
    for (const std::string& a : s.UnsavedStructures()) {
      std::string err;
      if (!s.Run("struct.save", Json{{"struct", a}}, &err)) {
        out.error = path + ": saving " + a + ": " + err;
        rollback();
        return false;
      }
      out.saved.push_back(a);
    }
  }
  return true;
}

}  // namespace editor
