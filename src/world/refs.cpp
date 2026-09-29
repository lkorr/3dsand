// refs.cpp — see refs.h (docs/PLAN_world_editor.md §2.1, DESIGN.md §16).

#include "world/refs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "game/mob.h"
#include "sim/bytestream.h"
#include "sim/chunkstore.h"
#include "sim/world.h"

namespace refs {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kRefsFourCC =
    'R' | ('E' << 8) | ('F' << 16) | ((uint32_t)'S' << 24);
constexpr size_t kMaxWarnings = 256;
constexpr size_t kMaxLog = 512;

uint64_t RegionKey(IVec3 rc) {
  return ((uint64_t)(uint32_t)(rc.x & 0x1FFFFF)) |
         ((uint64_t)(uint32_t)(rc.y & 0x1FFFFF) << 21) |
         ((uint64_t)(uint32_t)(rc.z & 0x1FFFFF) << 42);
}

IVec3 RegionOfRef(const Ref& r) {
  return ChunkStore::RegionOfVoxel(Vec3{(float)r.pos.x, (float)r.pos.y, (float)r.pos.z});
}

int FloorDiv(int v, int d) { return (v >= 0) ? v / d : -((-v + d - 1) / d); }

IVec3 ChunkOfRef(const Ref& r) {
  return {FloorDiv(r.pos.x, (int)kChunk), FloorDiv(r.pos.y, (int)kChunk),
          FloorDiv(r.pos.z, (int)kChunk)};
}

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

}  // namespace

// ---- ids --------------------------------------------------------------------

uint32_t RefHash(const std::string& id) {
  uint32_t h = 2166136261u;
  for (unsigned char c : id) {
    h ^= c;
    h *= 16777619u;
  }
  // 0 is "no ref" on the wire (TickInput::useRef).
  return h == 0 ? 1u : h;
}

std::string GroupOfId(const std::string& id) {
  const size_t s = id.find('/');
  return s == std::string::npos ? std::string() : id.substr(0, s);
}

bool ValidId(const std::string& id, std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return false;
  };
  if (id.empty()) return fail("id is empty");
  if (id.find('/') == std::string::npos)
    return fail("id must be group/name (e.g. \"harrowby/well\")");
  size_t partLen = 0;
  for (char c : id) {
    if (c == '/') {
      if (partLen == 0) return fail("id has an empty part (\"//\" or a leading/trailing '/')");
      partLen = 0;
      continue;
    }
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return fail("id may only use lowercase a-z, 0-9, '_', '-' and '/'");
    partLen++;
  }
  if (partLen == 0) return fail("id ends with '/'");
  return true;
}

// ---- text -------------------------------------------------------------------

std::string JsonInline(const Json& j) {
  if (j.is_object()) {
    if (j.empty()) return "{}";
    std::string s = "{ ";
    bool first = true;
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (!first) s += ", ";
      first = false;
      s += Json(it.key()).dump();
      s += ": ";
      s += JsonInline(it.value());
    }
    return s + " }";
  }
  if (j.is_array()) {
    if (j.empty()) return "[]";
    std::string s = "[";
    bool first = true;
    for (const Json& v : j) {
      if (!first) s += ", ";
      first = false;
      s += JsonInline(v);
    }
    return s + "]";
  }
  return j.dump();
}

std::string RefLine(const Ref& r) {
  std::string s = "{ \"id\": " + Json(r.id).dump();
  s += ", \"kind\": " + Json(r.kind).dump();
  if (!r.base.empty()) s += ", \"base\": " + Json(r.base).dump();
  char buf[96];
  std::snprintf(buf, sizeof buf, ", \"pos\": [%d, %d, %d], \"yaw\": %d", r.pos.x, r.pos.y,
                r.pos.z, r.yaw);
  s += buf;
  if (r.props.is_object() && !r.props.empty()) s += ", \"props\": " + JsonInline(r.props);
  for (auto it = r.extra.begin(); it != r.extra.end(); ++it)
    s += ", " + Json(it.key()).dump() + ": " + JsonInline(it.value());
  return s + " }";
}

std::string WriteGroup(const RefGroupFile& g) {
  std::string s = "{\n  \"group\": " + Json(g.group).dump() + ",\n";
  for (auto it = g.extra.begin(); it != g.extra.end(); ++it)
    s += "  " + Json(it.key()).dump() + ": " + JsonInline(it.value()) + ",\n";
  if (g.refs.empty()) {
    s += "  \"refs\": []\n}\n";
    return s;
  }
  s += "  \"refs\": [\n";
  for (size_t i = 0; i < g.refs.size(); i++) {
    s += "    " + RefLine(g.refs[i]);
    s += i + 1 < g.refs.size() ? ",\n" : "\n";
  }
  s += "  ]\n}\n";
  return s;
}

bool ParseGroup(const std::string& text, const std::string& label, RefGroupFile& out,
                std::vector<std::string>& warn) {
  out = RefGroupFile{};
  Json root;
  try {
    root = Json::parse(text);
  } catch (const std::exception& e) {
    warn.push_back(label + ": not valid JSON (" + e.what() + ")");
    return false;
  }
  if (!root.is_object()) {
    warn.push_back(label + ": the file must be a JSON object { \"group\", \"refs\" }");
    return false;
  }
  const std::string stem = fs::path(label).stem().string();
  if (root.contains("group") && root["group"].is_string()) {
    out.group = root["group"].get<std::string>();
  } else {
    out.group = stem;
    warn.push_back(label + ": group: missing or not a string; using the file name \"" +
                   stem + "\"");
  }
  if (!root.contains("refs") || !root["refs"].is_array()) {
    warn.push_back(label + ": refs: missing or not an array");
    return false;
  }
  for (auto it = root.begin(); it != root.end(); ++it)
    if (it.key() != "group" && it.key() != "refs") out.extra[it.key()] = it.value();

  int index = 0;
  for (const Json& jr : root["refs"]) {
    const std::string where = label + ": refs[" + std::to_string(index++) + "]";
    if (!jr.is_object()) {
      warn.push_back(where + ": not an object; skipped");
      continue;
    }
    Ref r;
    r.group = out.group;
    if (!jr.contains("id") || !jr["id"].is_string()) {
      warn.push_back(where + ": id: missing or not a string; skipped");
      continue;
    }
    r.id = jr["id"].get<std::string>();
    const std::string at = label + ": " + r.id;
    std::string why;
    if (!ValidId(r.id, &why)) {
      warn.push_back(at + ": id: " + why + "; skipped");
      continue;
    }
    if (!jr.contains("kind") || !jr["kind"].is_string()) {
      warn.push_back(at + ": kind: missing or not a string; skipped");
      continue;
    }
    r.kind = jr["kind"].get<std::string>();
    if (jr.contains("base")) {
      if (jr["base"].is_string()) {
        r.base = jr["base"].get<std::string>();
      } else {
        warn.push_back(at + ": base: not a string; ignored");
      }
    }
    bool posOk = jr.contains("pos") && jr["pos"].is_array() && jr["pos"].size() == 3;
    if (posOk)
      for (const Json& v : jr["pos"]) posOk &= v.is_number();
    if (!posOk) {
      warn.push_back(at + ": pos: must be [x, y, z] in world voxels; skipped");
      continue;
    }
    auto coord = [&](int k) {
      const Json& v = jr["pos"][k];
      if (v.is_number_integer()) return v.get<int>();
      const double d = v.get<double>();
      warn.push_back(at + ": pos: " + v.dump() + " is not a whole voxel; rounded");
      return (int)std::lround(d);
    };
    r.pos = {coord(0), coord(1), coord(2)};
    if (jr.contains("yaw")) {
      const Json& y = jr["yaw"];
      if (y.is_number_integer()) {
        r.yaw = y.get<int>();
      } else if (y.is_number()) {
        r.yaw = (int)std::lround(y.get<double>());
        warn.push_back(at + ": yaw: " + y.dump() + " is not whole degrees; rounded");
      } else {
        warn.push_back(at + ": yaw: not a number; 0 used");
      }
    }
    if (jr.contains("props")) {
      if (jr["props"].is_object()) {
        r.props = jr["props"];
      } else {
        warn.push_back(at + ": props: not an object; ignored");
      }
    }
    for (auto it = jr.begin(); it != jr.end(); ++it) {
      const std::string& k = it.key();
      if (k == "id" || k == "kind" || k == "base" || k == "pos" || k == "yaw" || k == "props")
        continue;
      r.extra[k] = it.value();
    }
    if (GroupOfId(r.id) != out.group)
      warn.push_back(at + ": id: does not start with the group \"" + out.group +
                     "/\" (kept; ids are map-wide, the prefix is the convention)");
    r.hash = RefHash(r.id);
    out.refs.push_back(std::move(r));
  }
  return true;
}

// ---- kinds ------------------------------------------------------------------

const char* RefEventName(RefEvent e) {
  switch (e) {
    case RefEvent::WindowLeft: return "window-left";
    case RefEvent::Edited: return "edited";
    case RefEvent::Deleted: return "deleted";
    case RefEvent::Reset: return "reset";
  }
  return "?";
}

void RefKindRegistry::Register(RefKind k) {
  const std::string n = k.name;
  kinds_[n] = std::move(k);
}

const RefKind* RefKindRegistry::Find(const std::string& name) const {
  auto it = kinds_.find(name);
  return it == kinds_.end() ? nullptr : &it->second;
}

std::vector<std::string> RefKindRegistry::Names() const {
  std::vector<std::string> out;
  for (const auto& [n, k] : kinds_) out.push_back(n);
  return out;
}

RefKindRegistry& Kinds() {
  static RefKindRegistry r;
  return r;
}

// ---- the store: loading -----------------------------------------------------

void RefStore::Warn(const std::string& w) {
  std::fprintf(stderr, "refs WARNING: %s\n", w.c_str());
  warnings_.push_back(w);
  if (warnings_.size() > kMaxWarnings)
    warnings_.erase(warnings_.begin(), warnings_.begin() + (warnings_.size() - kMaxWarnings));
}

bool RefStore::ValidateRef(const Ref& r, const std::string& label) {
  const RefKind* k = Kinds().Find(r.kind);
  if (k == nullptr) {
    Warn(label + ": " + r.id + ": kind: \"" + r.kind +
         "\" is not a kind this build knows; kept as written, inert");
    return false;
  }
  if (!k->validate) return true;
  std::vector<std::string> problems;
  k->validate(r, problems);
  for (const std::string& p : problems) Warn(label + ": " + r.id + ": " + p);
  return problems.empty();
}

void RefStore::Index() {
  byHash_.clear();
  byRegion_.clear();
  for (const auto& [id, r] : refs_) {
    auto [it, fresh] = byHash_.emplace(r.hash, id);
    if (!fresh)
      Warn("ref ids \"" + it->second + "\" and \"" + id +
           "\" share a use hash; rename one (the use key cannot tell them apart)");
    byRegion_[RegionKey(RegionOfRef(r))].push_back(id);
  }
}

namespace {
// Read one refs dir into groups (file-name order). Shared by LoadMap/Reload.
void ReadDir(const std::string& dir, std::map<std::string, RefGroupFile>& groups,
             std::vector<std::string>& order, std::map<std::string, Ref>& refs,
             std::vector<std::string>& warn) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> files;
  for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (const fs::path& p : files) {
    std::string text;
    const std::string label = "refs/" + p.filename().string();
    if (!ReadFile(p.string(), text)) {
      warn.push_back(label + ": could not be read");
      continue;
    }
    RefGroupFile g;
    if (!ParseGroup(text, label, g, warn)) continue;
    if (g.group != p.stem().string())
      warn.push_back(label + ": group: \"" + g.group +
                     "\" differs from the file name; the file name is what edits write to");
    g.group = p.stem().string();
    if (groups.count(g.group)) {
      warn.push_back(label + ": a second file for group \"" + g.group + "\"; skipped");
      continue;
    }
    std::vector<Ref> kept;
    for (Ref& r : g.refs) {
      r.group = g.group;
      if (refs.count(r.id)) {
        warn.push_back(label + ": " + r.id + ": id: already used by refs/" +
                       refs[r.id].group + ".json; this one is skipped (ids are map-wide)");
        continue;
      }
      refs[r.id] = r;
      kept.push_back(r);
    }
    g.refs = std::move(kept);
    order.push_back(g.group);
    groups[g.group] = std::move(g);
  }
}
}  // namespace

namespace {
std::vector<void (*)(RefStore*)>& GoneHooks() {
  static std::vector<void (*)(RefStore*)> h;
  return h;
}
}  // namespace

void AddStoreGoneHook(void (*fn)(RefStore*)) {
  for (auto f : GoneHooks())
    if (f == fn) return;
  GoneHooks().push_back(fn);
}

RefStore::~RefStore() {
  for (auto f : GoneHooks()) f(this);
}

void RefStore::LoadMap(const std::string& assetDir, const std::string& mapName) {
  assetDir_ = assetDir;
  map_ = mapName;
  dir_ = (fs::path(assetDir) / "worldmap" / mapName / "refs").string();
  groups_.clear();
  groupOrder_.clear();
  refs_.clear();
  warnings_.clear();
  active_.clear();
  retry_.clear();
  reapply_.clear();
  deltas_.clear();
  pulledRegions_.clear();
  graveyard_.clear();
  prev_.clear();
  log_.clear();
  haveOrigin_ = false;
  std::vector<std::string> warn;
  ReadDir(dir_, groups_, groupOrder_, refs_, warn);
  for (const std::string& w : warn) Warn(w);
  for (const auto& [id, r] : refs_) ValidateRef(r, "refs/" + r.group + ".json");
  Index();
  revision_++;
  std::printf("refs: map '%s': %zu ref(s) in %zu group(s), %zu warning(s)\n",
              mapName.c_str(), refs_.size(), groups_.size(), warnings_.size());
}

void RefStore::Reload(RefCtx& ctx) {
  std::map<std::string, RefGroupFile> groups;
  std::vector<std::string> order;
  std::map<std::string, Ref> refs;
  std::vector<std::string> warn;
  ReadDir(dir_, groups, order, refs, warn);
  // Vanished refs: undo now (we still hold their old line), forget the delta.
  for (auto it = refs_.begin(); it != refs_.end(); ++it) {
    if (refs.count(it->first)) continue;
    Deactivate(ctx, it->first, RefEvent::Deleted);
    retry_.erase(it->first);
    reapply_.erase(it->first);
    deltas_.erase(it->first);
  }
  // Changed lines are re-applied on the next Update (in id order there).
  for (const auto& [id, r] : refs) {
    auto old = refs_.find(id);
    if (old != refs_.end() && RefLine(old->second) != RefLine(r)) {
      reapply_.insert(id);
      if (active_.count(id) && !prev_.count(id)) prev_[id] = old->second;
    }
  }
  groups_ = std::move(groups);
  groupOrder_ = std::move(order);
  refs_ = std::move(refs);
  warnings_.clear();
  for (const std::string& w : warn) Warn(w);
  for (const auto& [id, r] : refs_) ValidateRef(r, "refs/" + r.group + ".json");
  Index();
  haveOrigin_ = false;   // force the next Update to look at everything
  revision_++;
  std::printf("refs: reloaded '%s': %zu ref(s), %zu to re-apply, %zu warning(s)\n",
              map_.c_str(), refs_.size(), reapply_.size(), warnings_.size());
}

// ---- the store: reading -----------------------------------------------------

const Ref* RefStore::Find(const std::string& id) const {
  auto it = refs_.find(id);
  return it == refs_.end() ? nullptr : &it->second;
}

const Ref* RefStore::FindByHash(uint32_t h) const {
  auto it = byHash_.find(h);
  return it == byHash_.end() ? nullptr : Find(it->second);
}

std::vector<const Ref*> RefStore::ChildrenOf(const std::string& id) const {
  std::vector<const Ref*> out;
  const std::string pre = id + "/";
  for (auto it = refs_.lower_bound(pre); it != refs_.end(); ++it) {
    if (it->first.compare(0, pre.size(), pre) != 0) break;
    if (it->first.find('/', pre.size()) == std::string::npos) out.push_back(&it->second);
  }
  return out;
}

std::vector<const Ref*> RefStore::InRegion(IVec3 rc) const {
  std::vector<const Ref*> out;
  auto it = byRegion_.find(RegionKey(rc));
  if (it == byRegion_.end()) return out;
  for (const std::string& id : it->second)
    if (const Ref* r = Find(id)) out.push_back(r);
  return out;
}

const RefGroupFile* RefStore::Group(const std::string& g) const {
  auto it = groups_.find(g);
  return it == groups_.end() ? nullptr : &it->second;
}

std::string RefStore::GroupPath(const std::string& g) const {
  return (fs::path(dir_) / (g + ".json")).string();
}

// ---- the store: activation --------------------------------------------------

void RefStore::Activate(RefCtx& ctx, const Ref& r) {
  EnsurePulled(r);
  const RefKind* k = Kinds().Find(r.kind);
  Activation a = Activation::Done;
  if (k != nullptr && k->activate) a = k->activate(ctx, r);
  if (a == Activation::Retry) {
    retry_.insert(r.id);
    return;
  }
  retry_.erase(r.id);
  active_.insert(r.id);
  log_.push_back(RefActivity{ctx.tick, r.id, true, RefEvent::WindowLeft});
  if (log_.size() > kMaxLog) log_.erase(log_.begin(), log_.begin() + (log_.size() - kMaxLog));
}

void RefStore::Deactivate(RefCtx& ctx, const std::string& id, RefEvent why) {
  retry_.erase(id);
  if (!active_.count(id)) return;
  const Ref* r = Find(id);
  if (auto p = prev_.find(id); p != prev_.end()) r = &p->second;
  if (auto g = graveyard_.find(id); g != graveyard_.end()) r = &g->second;
  if (r != nullptr) {
    const RefKind* k = Kinds().Find(r->kind);
    if (k != nullptr && k->deactivate) k->deactivate(ctx, *r, why);
  }
  active_.erase(id);
  prev_.erase(id);
  log_.push_back(RefActivity{ctx.tick, id, false, why});
  if (log_.size() > kMaxLog) log_.erase(log_.begin(), log_.begin() + (log_.size() - kMaxLog));
}

void RefStore::NoteUse(UseRecord u) {
  uses_.push_back(std::move(u));
  if (uses_.size() > 64) uses_.erase(uses_.begin());
}

void RefStore::DeactivateAll(RefCtx& ctx, RefEvent why) {
  const std::vector<std::string> ids(active_.begin(), active_.end());   // id order
  for (const std::string& id : ids) Deactivate(ctx, id, why);
  retry_.clear();
}

void RefStore::Update(RefCtx& ctx, IVec3 wo) {
  lastTick_ = ctx.tick;
  const bool moved = !haveOrigin_ || wo.x != lastOrigin_.x || wo.y != lastOrigin_.y ||
                     wo.z != lastOrigin_.z;
  if (!moved && retry_.empty() && reapply_.empty() && graveyard_.empty()) return;
  haveOrigin_ = true;
  lastOrigin_ = wo;
  const int n = (int)(kWorldN / kChunk);
  const int m = kActivateMarginChunks;
  auto insideBy = [&](IVec3 c, int margin) {
    return c.x >= wo.x + margin && c.x < wo.x + n - margin && c.y >= wo.y + margin &&
           c.y < wo.y + n - margin && c.z >= wo.z + margin && c.z < wo.z + n - margin;
  };

  // 1. DELETED refs (an edit erased them): undo, in id order.
  for (auto& [id, r] : graveyard_) Deactivate(ctx, id, RefEvent::Deleted);
  graveyard_.clear();

  // 2. EDITED refs: undo what the old line did; step 4 re-applies the new one.
  for (const std::string& id : std::vector<std::string>(reapply_.begin(), reapply_.end()))
    Deactivate(ctx, id, RefEvent::Edited);
  reapply_.clear();

  // 3. LEFT THE WINDOW: wholly outside it (the hysteresis: margin 0).
  for (const std::string& id : std::vector<std::string>(active_.begin(), active_.end())) {
    const Ref* r = Find(id);
    if (r == nullptr || !insideBy(ChunkOfRef(*r), 0)) Deactivate(ctx, id, RefEvent::WindowLeft);
  }
  for (auto it = retry_.begin(); it != retry_.end();) {
    const Ref* r = Find(*it);
    it = (r == nullptr || !insideBy(ChunkOfRef(*r), m)) ? retry_.erase(it) : std::next(it);
  }

  // 4. CAME IN: every ref in a region the window touches that is inside by the
  //    margin and not yet active, in ID ORDER (the determinism rule).
  std::vector<std::string> cand;
  const IVec3 lo = ChunkStore::RegionOfChunk(wo);
  const IVec3 hi = ChunkStore::RegionOfChunk({wo.x + n - 1, wo.y + n - 1, wo.z + n - 1});
  for (int rz = lo.z; rz <= hi.z; rz++)
    for (int ry = lo.y; ry <= hi.y; ry++)
      for (int rx = lo.x; rx <= hi.x; rx++) {
        auto it = byRegion_.find(RegionKey({rx, ry, rz}));
        if (it == byRegion_.end()) continue;
        for (const std::string& id : it->second) cand.push_back(id);
      }
  std::sort(cand.begin(), cand.end());
  cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
  for (const std::string& id : cand) {
    if (active_.count(id)) continue;
    const Ref* r = Find(id);
    if (r == nullptr || !insideBy(ChunkOfRef(*r), m)) continue;
    Activate(ctx, *r);
  }
}

// ---- the store: deltas ------------------------------------------------------

void RefStore::EncodeDelta(const std::string& id, const RefDelta& d,
                           std::vector<uint8_t>& out) {
  ByteWriter w{out};
  w.U32(kSaveVersion);
  w.Str(id);
  w.Str(d.kind);
  w.U32(d.version);
  w.PodVec(d.bytes);
}

bool RefStore::DecodeDelta(const uint8_t* p, size_t n, std::string& id, RefDelta& d) {
  ByteReader r{p, n};
  uint32_t fmt = 0;
  r.U32(fmt);
  if (!r.ok || fmt != kSaveVersion) return false;
  r.Str(id);
  r.Str(d.kind);
  r.U32(d.version);
  r.PodVec(d.bytes);
  return r.ok && r.off == n;
}

bool RefStore::AcceptDelta(const std::string& id, RefDelta d, const char* from) {
  const Ref* r = Find(id);
  if (r == nullptr) {
    Warn(std::string("save: a saved state for ref \"") + id + "\" (" + d.kind + ", from " +
         from + ") names no ref in map '" + map_ +
         "' any more (deleted or renamed in its refs file); the state is dropped");
    return false;
  }
  if (r->kind != d.kind) {
    Warn(std::string("save: the saved state of ref \"") + id + "\" was written by kind \"" +
         d.kind + "\" but the ref is now \"" + r->kind + "\"; the state is dropped");
    return false;
  }
  deltas_[id] = std::move(d);
  return true;
}

void RefStore::EnsurePulled(const Ref& r) {
  if (chunkStore_ == nullptr) return;
  const IVec3 rc = RegionOfRef(r);
  if (!pulledRegions_.insert(RegionKey(rc)).second) return;
  std::vector<ChunkStore::EntityRecord>& dormant = chunkStore_->DormantEntities(rc);
  std::vector<std::pair<std::string, RefDelta>> got;
  for (size_t i = 0; i < dormant.size();) {
    if (dormant[i].section != kRefsFourCC) {
      i++;
      continue;
    }
    std::string id;
    RefDelta d;
    const bool ok = DecodeDelta(dormant[i].bytes.data(), dormant[i].bytes.size(), id, d);
    if (ok) got.emplace_back(std::move(id), std::move(d));
    else Warn("save: an unreadable ref state record in region (" + std::to_string(rc.x) +
              "," + std::to_string(rc.y) + "," + std::to_string(rc.z) + ") is dropped");
    dormant.erase(dormant.begin() + (ptrdiff_t)i);
  }
  std::sort(got.begin(), got.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [id, d] : got)
    if (!deltas_.count(id)) AcceptDelta(id, std::move(d), "a region bucket");
}

const RefDelta* RefStore::Delta(const std::string& id) {
  if (const Ref* r = Find(id)) EnsurePulled(*r);
  auto it = deltas_.find(id);
  return it == deltas_.end() ? nullptr : &it->second;
}

void RefStore::SetDelta(const std::string& id, const std::string& kind, uint32_t version,
                        std::vector<uint8_t> bytes) {
  const Ref* r = Find(id);
  if (r == nullptr) return;
  EnsurePulled(*r);
  deltas_[id] = RefDelta{kind, version, std::move(bytes)};
}

void RefStore::ClearDelta(const std::string& id) {
  if (const Ref* r = Find(id)) EnsurePulled(*r);
  deltas_.erase(id);
}

void RefStore::SaveDeltas(RefCtx& ctx, std::vector<std::pair<Vec3, std::vector<uint8_t>>>& out) {
  for (const std::string& id : active_) {
    const Ref* r = Find(id);
    if (r == nullptr) continue;
    const RefKind* k = Kinds().Find(r->kind);
    if (k != nullptr && k->flushDelta) k->flushDelta(ctx, *r);
  }
  for (const auto& [id, d] : deltas_) {
    const Ref* r = Find(id);
    if (r == nullptr) continue;
    std::vector<uint8_t> bytes;
    EncodeDelta(id, d, bytes);
    out.emplace_back(Vec3{(float)r->pos.x, (float)r->pos.y, (float)r->pos.z}, std::move(bytes));
  }
}

void RefStore::ResetForLoad(RefCtx& ctx) {
  DeactivateAll(ctx, RefEvent::Reset);
  deltas_.clear();
  pulledRegions_.clear();
  retry_.clear();
  haveOrigin_ = false;
}

// ---- the store: authoring ---------------------------------------------------

bool RefStore::WriteGroupFile(const std::string& group, std::string* err) {
  auto it = groups_.find(group);
  if (it == groups_.end()) {
    if (err) *err = "no group \"" + group + "\"";
    return false;
  }
  std::error_code ec;
  fs::create_directories(dir_, ec);
  const std::string path = GroupPath(group);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      if (err) *err = "could not write " + path;
      return false;
    }
    f << WriteGroup(it->second);
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    if (err) *err = "could not replace " + path + " (" + ec.message() + ")";
    return false;
  }
  return true;
}

bool RefStore::Upsert(const Ref& in, std::string* err, int fileIndex) {
  std::string why;
  if (!ValidId(in.id, &why)) {
    if (err) *err = in.id + ": " + why;
    return false;
  }
  if (dir_.empty()) {
    if (err) *err = "no map is loaded (RefStore::LoadMap was never called)";
    return false;
  }
  Ref r = in;
  r.hash = RefHash(r.id);
  auto old = refs_.find(r.id);
  r.group = old != refs_.end() ? old->second.group : GroupOfId(r.id);
  if (old == refs_.end()) {
    for (const auto& [h, id] : byHash_)
      if (h == r.hash) {
        if (err) *err = r.id + ": shares a use hash with \"" + id + "\"; pick another name";
        return false;
      }
  }
  RefGroupFile& g = groups_[r.group];
  if (g.group.empty()) {
    g.group = r.group;
    groupOrder_.push_back(r.group);
  }
  bool replaced = false;
  for (Ref& x : g.refs)
    if (x.id == r.id) {
      x = r;
      replaced = true;
    }
  if (!replaced) {
    if (fileIndex >= 0 && fileIndex < (int)g.refs.size())
      g.refs.insert(g.refs.begin() + fileIndex, r);
    else
      g.refs.push_back(r);
  }
  if (old != refs_.end() && active_.count(r.id) && !prev_.count(r.id))
    prev_[r.id] = old->second;
  refs_[r.id] = r;
  if (!WriteGroupFile(r.group, err)) return false;
  ValidateRef(r, "refs/" + r.group + ".json");
  Index();
  if (old != refs_.end() || active_.count(r.id)) reapply_.insert(r.id);
  haveOrigin_ = false;
  revision_++;
  return true;
}

bool RefStore::Erase(const std::string& id, std::string* err, Ref* removed,
                     int* fileIndex) {
  auto it = refs_.find(id);
  if (it == refs_.end()) {
    if (err) *err = "no ref \"" + id + "\"";
    return false;
  }
  const std::string group = it->second.group;
  if (removed) *removed = it->second;
  graveyard_[id] = it->second;
  refs_.erase(it);
  RefGroupFile& g = groups_[group];
  if (fileIndex) {
    *fileIndex = -1;
    for (size_t i = 0; i < g.refs.size(); i++)
      if (g.refs[i].id == id) *fileIndex = (int)i;
  }
  g.refs.erase(std::remove_if(g.refs.begin(), g.refs.end(),
                              [&](const Ref& x) { return x.id == id; }),
               g.refs.end());
  deltas_.erase(id);
  reapply_.erase(id);
  retry_.erase(id);
  if (!WriteGroupFile(group, err)) return false;
  Index();
  revision_++;
  return true;
}

// ---- the four authoring actions ---------------------------------------------

bool Place(RefStore& s, const Ref& r, std::string* err, int fileIndex) {
  if (s.Find(r.id) != nullptr) {
    if (err) *err = "\"" + r.id + "\" already exists (ids are never reused)";
    return false;
  }
  return s.Upsert(r, err, fileIndex);
}

bool Move(RefStore& s, const std::string& id, IVec3 pos, int yaw, std::string* err) {
  const Ref* r = s.Find(id);
  if (r == nullptr) {
    if (err) *err = "no ref \"" + id + "\"";
    return false;
  }
  Ref c = *r;
  c.pos = pos;
  c.yaw = yaw;
  return s.Upsert(c, err);
}

bool SetProp(RefStore& s, const std::string& id, const std::string& key, const Json& value,
             std::string* err) {
  const Ref* r = s.Find(id);
  if (r == nullptr) {
    if (err) *err = "no ref \"" + id + "\"";
    return false;
  }
  if (key.empty()) {
    if (err) *err = "a prop needs a name";
    return false;
  }
  Ref c = *r;
  if (value.is_null()) c.props.erase(key);
  else c.props[key] = value;
  return s.Upsert(c, err);
}

bool SetField(RefStore& s, const std::string& id, const std::string& field,
              const std::string& value, std::string* err) {
  const Ref* r = s.Find(id);
  if (r == nullptr) {
    if (err) *err = "no ref \"" + id + "\"";
    return false;
  }
  Ref c = *r;
  if (field == "kind") {
    if (value.empty()) {
      if (err) *err = "kind cannot be empty";
      return false;
    }
    c.kind = value;
  } else if (field == "base") {
    c.base = value;
  } else {
    if (err) *err = "\"" + field + "\" is not an editable field (kind, base; pos/yaw via Move)";
    return false;
  }
  return s.Upsert(c, err);
}

bool Delete(RefStore& s, const std::string& id, std::string* err, Ref* removed,
            int* fileIndex) {
  return s.Erase(id, err, removed, fileIndex);
}

uint64_t LiveMobOfRef(MobSystem& mobs, const std::string& id) {
  const Mob* m = mobs.FindMobByRef(id);
  return m != nullptr ? m->Id() : 0;
}

}  // namespace refs
