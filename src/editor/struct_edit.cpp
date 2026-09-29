// struct_edit.cpp — see struct_edit.h.

#include "editor/struct_edit.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace editor {

namespace fs = std::filesystem;

namespace {

bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool WriteFileAtomic(const std::string& path, const std::string& bytes, std::string& err) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) {
      err = "could not write " + path;
      return false;
    }
    f.write(bytes.data(), (std::streamsize)bytes.size());
    if (!f) {
      err = "could not write " + path;
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    err = "could not replace " + path + " (" + ec.message() + ")";
    return false;
  }
  return true;
}

void PutU32(std::string& s, uint32_t v) {
  char b[4];
  std::memcpy(b, &v, 4);
  s.append(b, 4);
}

// The RGBA chunk's 1024 bytes out of a .vox, or "" (a file without one).
std::string FindRgbaChunk(const std::string& vox) {
  size_t off = 8;
  while (off + 12 <= vox.size()) {
    const char* id = vox.data() + off;
    uint32_t content = 0;
    std::memcpy(&content, vox.data() + off + 4, 4);
    off += 12;
    if (std::memcmp(id, "MAIN", 4) == 0) continue;   // children follow inline
    if (off + content > vox.size()) break;
    if (std::memcmp(id, "RGBA", 4) == 0 && content >= 1024) return vox.substr(off, 1024);
    off += content;
  }
  return std::string();
}

bool IsBox(const Json& v) {
  auto vec = [](const Json& a) {
    if (!a.is_array() || a.size() != 3) return false;
    for (const Json& e : a)
      if (!e.is_number_integer()) return false;
    return true;
  };
  return v.is_object() && v.contains("min") && v.contains("max") && vec(v["min"]) && vec(v["max"]);
}

uint64_t Mix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

}  // namespace

std::vector<std::string> LoadMaterialNames(const std::string& assetDir) {
  std::vector<std::string> names{"air"};
  std::string text;
  if (!ReadFile(assetDir + "/materials/materials.json", text)) return names;
  try {
    const Json j = Json::parse(text);
    if (j.contains("materials") && j["materials"].is_array())
      for (const Json& m : j["materials"])
        if (m.is_object() && m.contains("id") && m["id"].is_string())
          names.push_back(m["id"].get<std::string>());
  } catch (...) {
  }
  return names;
}

// ---- the transform ----------------------------------------------------------------

namespace {
int Quarter(int yaw) { return (((yaw / 90) % 4) + 4) % 4; }
// (x, z) turned `q` quarter turns the way a heading increases (+Z -> +X).
void Turn(int q, int x, int z, int* ox, int* oz) {
  switch (q & 3) {
    case 0: *ox = x;  *oz = z;  break;
    case 1: *ox = z;  *oz = -x; break;
    case 2: *ox = -x; *oz = -z; break;
    default:*ox = -z; *oz = x;  break;
  }
}
}  // namespace

IVec3 HouseToWorld(IVec3 h, IVec3 pos, int yaw) {
  int x, z;
  Turn(Quarter(yaw), h.x, h.z, &x, &z);
  return {pos.x + x, pos.y + h.y, pos.z + z};
}

IVec3 WorldToHouse(IVec3 w, IVec3 pos, int yaw) {
  int x, z;
  Turn((4 - Quarter(yaw)) & 3, w.x - pos.x, w.z - pos.z, &x, &z);
  return {x, w.y - pos.y, z};
}

int HouseYawToWorld(int houseYaw, int yaw) { return (((houseYaw + yaw) % 360) + 360) % 360; }

Json ShiftSlotProps(const Json& props, IVec3 d) {
  if (!props.is_object()) return props;
  Json out = props;
  for (auto it = out.begin(); it != out.end(); ++it) {
    Json& v = it.value();
    if (IsBox(v)) {
      for (const char* k : {"min", "max"}) {
        Json& a = v[k];
        a[0] = a[0].get<int>() + d.x;
        a[1] = a[1].get<int>() + d.y;
        a[2] = a[2].get<int>() + d.z;
      }
    } else if (it.key() == "hingeLine" && v.is_array() && v.size() == 2 &&
               v[0].is_number_integer() && v[1].is_number_integer()) {
      v[0] = v[0].get<int>() + d.x;
      v[1] = v[1].get<int>() + d.z;
    }
  }
  return out;
}

// ---- cells ------------------------------------------------------------------------

uint64_t StructEdit::Key(IVec3 h) {
  auto u = [](int v) { return (uint64_t)(uint32_t)(v + (1 << 20)) & 0x1FFFFFull; };
  return u(h.x) | (u(h.y) << 21) | (u(h.z) << 42);
}

IVec3 StructEdit::Unkey(uint64_t k) {
  auto s = [](uint64_t v) { return (int)(v & 0x1FFFFFull) - (1 << 20); };
  return {s(k), s(k >> 21), s(k >> 42)};
}

uint64_t StructEdit::CellHash(uint64_t key, uint16_t mat) {
  return Mix(key * 0x9E3779B97F4A7C15ULL + mat + 1);
}

uint16_t StructEdit::Get(IVec3 h) const {
  auto it = cells_.find(Key(h));
  return it == cells_.end() ? 0 : it->second;
}

uint16_t StructEdit::Set(IVec3 h, uint16_t mat) {
  const uint64_t k = Key(h);
  auto it = cells_.find(k);
  const uint16_t prev = it == cells_.end() ? 0 : it->second;
  if (prev == mat) return prev;
  if (prev != 0) cellHash_ -= CellHash(k, prev);
  if (mat != 0) cellHash_ += CellHash(k, mat);
  if (mat == 0) cells_.erase(it);
  else if (it == cells_.end()) cells_.emplace(k, mat);
  else it->second = mat;
  hash_ = Mix(cellHash_ ^ (slotHash_ * 31));
  rev_++;
  changed_.push_back({h, mat});
  return prev;
}

bool StructEdit::Bounds(IVec3& lo, IVec3& hi) const {
  lo = {INT_MAX, INT_MAX, INT_MAX};
  hi = {INT_MIN, INT_MIN, INT_MIN};
  for (const auto& [k, m] : cells_) {
    const IVec3 c = Unkey(k);
    lo = {std::min(lo.x, c.x), std::min(lo.y, c.y), std::min(lo.z, c.z)};
    hi = {std::max(hi.x, c.x), std::max(hi.y, c.y), std::max(hi.z, c.z)};
  }
  return !cells_.empty();
}

std::vector<std::pair<IVec3, uint16_t>> StructEdit::TakeChanged() {
  std::vector<std::pair<IVec3, uint16_t>> v;
  v.swap(changed_);
  return v;
}

// ---- slots ------------------------------------------------------------------------

void StructEdit::RehashSlots() {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : SlotsJson().dump()) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  slotHash_ = h;
  hash_ = Mix(cellHash_ ^ (slotHash_ * 31));
}

void StructEdit::SetSlots(std::vector<structures::Slot> s) {
  slots_ = std::move(s);
  rev_++;
  RehashSlots();
}

int StructEdit::FindSlot(const std::string& n) const {
  for (size_t i = 0; i < slots_.size(); i++)
    if (slots_[i].name == n) return (int)i;
  return -1;
}

Json StructEdit::SlotsJson() const {
  Json a = Json::array();
  for (const structures::Slot& s : slots_) {
    Json o = Json::object();
    o["name"] = s.name;
    o["kind"] = s.kind;
    o["pos"] = Json::array({s.pos.x, s.pos.y, s.pos.z});
    o["yaw"] = s.yaw;
    if (s.props.is_object() && !s.props.empty()) o["props"] = s.props;
    a.push_back(std::move(o));
  }
  return a;
}

bool StructEdit::SlotsFromJson(const Json& j, std::vector<structures::Slot>& out,
                               std::string& err) {
  out.clear();
  if (!j.is_array()) {
    err = "slots: must be an array";
    return false;
  }
  for (const Json& o : j) {
    structures::Slot s;
    if (!o.is_object() || !o.contains("name") || !o["name"].is_string() || !o.contains("kind") ||
        !o["kind"].is_string() || !o.contains("pos") || !o["pos"].is_array() ||
        o["pos"].size() != 3) {
      err = "slots: each needs name, kind and pos [x, y, z]";
      return false;
    }
    s.name = o["name"].get<std::string>();
    s.kind = o["kind"].get<std::string>();
    s.pos = {o["pos"][0].get<int>(), o["pos"][1].get<int>(), o["pos"][2].get<int>()};
    s.yaw = o.contains("yaw") && o["yaw"].is_number() ? o["yaw"].get<int>() : 0;
    if (o.contains("props") && o["props"].is_object()) s.props = o["props"];
    out.push_back(std::move(s));
  }
  return true;
}

// ---- load -------------------------------------------------------------------------

bool StructEdit::Load(const std::string& assetDir, const std::string& nm, std::string& err,
                      std::vector<std::string>& warn) {
  structures::Asset a;
  if (!structures::LoadAsset(assetDir, nm, true, a, err, warn)) return false;
  std::string text;
  if (!ReadFile(assetDir + "/structures/" + nm + ".struct.json", text)) {
    err = "assets/structures/" + nm + ".struct.json does not exist";
    return false;
  }
  try {
    json_ = Json::parse(text);
  } catch (const std::exception& e) {
    err = std::string("assets/structures/") + nm + ".struct.json is not valid JSON (" + e.what() + ")";
    return false;
  }
  std::string vox;
  ReadFile(assetDir + "/structures/" + nm + ".vox", vox);
  rgba_ = FindRgbaChunk(vox);
  name = nm;
  origin_ = a.origin;
  size_ = a.size;
  cells_.clear();
  changed_.clear();
  cellHash_ = 0;
  cells_.reserve(a.prefab.models.empty() ? 0 : a.prefab.models[0].voxels.size() * 2);
  for (const PrefabModel& m : a.prefab.models)
    for (const PrefabVoxel& v : m.voxels) {
      if (v.material == 0) continue;
      const IVec3 h{m.offset.x + v.x - origin_.x, m.offset.y + v.y - origin_.y,
                    m.offset.z + v.z - origin_.z};
      const uint64_t k = Key(h);
      if (cells_.emplace(k, v.material).second) cellHash_ += CellHash(k, v.material);
    }
  slots_.clear();
  for (const structures::Slot& s : a.slots) {
    structures::Slot h = s;
    h.pos = {s.pos.x - origin_.x, s.pos.y - origin_.y, s.pos.z - origin_.z};
    h.props = ShiftSlotProps(s.props, {-origin_.x, -origin_.y, -origin_.z});
    slots_.push_back(std::move(h));
  }
  RehashSlots();
  changed_.clear();
  savedHash_ = hash_;
  rev_++;
  return true;
}

// ---- save -------------------------------------------------------------------------

structures::Asset StructEdit::AsAsset() const {
  structures::Asset a;
  a.name = name;
  a.origin = origin_;
  a.size = size_;
  for (const structures::Slot& s : slots_) {
    structures::Slot b = s;
    b.pos = {s.pos.x + origin_.x, s.pos.y + origin_.y, s.pos.z + origin_.z};
    b.props = ShiftSlotProps(s.props, origin_);
    a.slots.push_back(std::move(b));
  }
  return a;
}

bool StructEdit::BuildFiles(const std::vector<std::string>& matNames, std::string& vox,
                            std::string& jsonText, std::string& err) const {
  IVec3 lo, hi;
  if (!Bounds(lo, hi)) {
    err = name + ": the structure has no voxels left; a .vox cannot be empty (undo, or put "
                 "something back)";
    return false;
  }
  const IVec3 sz{hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
  const char* axis[3] = {"x (width)", "y (height)", "z (depth)"};
  const int ext[3] = {sz.x, sz.y, sz.z};
  for (int a = 0; a < 3; a++)
    if (ext[a] > 256) {
      err = name + ": the structure is " + std::to_string(ext[a]) + " voxels along " + axis[a] +
            "; a .vox holds at most 256 (trim it, or split the house in two)";
      return false;
    }
  // Voxels in a fixed order (z, y, x) so a save is a pure function of the
  // buffer: undo -> redo writes the same bytes.
  std::vector<std::pair<uint64_t, uint16_t>> sorted;
  sorted.reserve(cells_.size());
  std::set<uint16_t> used;
  for (const auto& [k, m] : cells_) {
    if (m > 255) {
      err = name + ": material \"" + (m < matNames.size() ? matNames[m] : std::to_string(m)) +
            "\" has id " + std::to_string(m) +
            "; a structure .vox can only hold material ids 1..255 (pick another material)";
      return false;
    }
    const IVec3 c = Unkey(k);
    const uint64_t order = ((uint64_t)(c.z - lo.z) << 40) | ((uint64_t)(c.y - lo.y) << 20) |
                           (uint64_t)(c.x - lo.x);
    sorted.push_back({order, m});
    used.insert(m);
  }
  std::sort(sorted.begin(), sorted.end());

  // ---- the .vox: one model, palette index == material id -------------------------
  std::string body;
  auto chunk = [&](const char* id, const std::string& content) {
    body.append(id, 4);
    PutU32(body, (uint32_t)content.size());
    PutU32(body, 0);
    body += content;
  };
  {
    std::string s;
    PutU32(s, (uint32_t)sz.x);   // vox x = engine x
    PutU32(s, (uint32_t)sz.z);   // vox y = depth (engine -z)
    PutU32(s, (uint32_t)sz.y);   // vox z = up (engine y)
    chunk("SIZE", s);
  }
  {
    std::string s;
    PutU32(s, (uint32_t)sorted.size());
    s.reserve(4 + sorted.size() * 4);
    for (const auto& [order, m] : sorted) {
      const int bx = (int)(order & 0xFFFFF), by = (int)((order >> 20) & 0xFFFFF),
                bz = (int)(order >> 40);
      // voxload.cpp: engine = (s.x, s.z, -s.y), rebased -> vox (x, D-1-z, y).
      s.push_back((char)(uint8_t)bx);
      s.push_back((char)(uint8_t)(sz.z - 1 - bz));
      s.push_back((char)(uint8_t)by);
      s.push_back((char)(uint8_t)m);
    }
    chunk("XYZI", s);
  }
  if (rgba_.size() == 1024) chunk("RGBA", rgba_);
  vox.clear();
  vox.append("VOX ", 4);
  PutU32(vox, 150);
  vox.append("MAIN", 4);
  PutU32(vox, 0);
  PutU32(vox, (uint32_t)body.size());
  vox += body;

  // ---- the .struct.json: re-based, every other field kept ----------------------------
  const IVec3 newOrigin{-lo.x, -lo.y, -lo.z};
  Json j = json_.is_object() ? json_ : Json::object();
  j["origin"] = Json::array({newOrigin.x, newOrigin.y, newOrigin.z});
  j["size"] = Json::array({sz.x, sz.y, sz.z});
  {
    Json mats = Json::object();
    std::set<uint16_t> placed;
    if (j.contains("materials") && j["materials"].is_object())
      for (auto it = j["materials"].begin(); it != j["materials"].end(); ++it)
        for (uint16_t m : used)
          if (m < matNames.size() && matNames[m] == it.key() && !placed.count(m)) {
            mats[it.key()] = (int)m;
            placed.insert(m);
          }
    for (uint16_t m : used)
      if (!placed.count(m)) mats[m < matNames.size() ? matNames[m] : std::to_string(m)] = (int)m;
    j["materials"] = mats;
  }
  {
    Json slots = Json::array();
    for (const structures::Slot& s : slots_) {
      Json o = Json::object();
      o["name"] = s.name;
      o["kind"] = s.kind;
      o["pos"] = Json::array({s.pos.x + newOrigin.x, s.pos.y + newOrigin.y, s.pos.z + newOrigin.z});
      o["yaw"] = s.yaw;
      o["props"] = ShiftSlotProps(s.props.is_object() ? s.props : Json::object(), newOrigin);
      slots.push_back(std::move(o));
    }
    j["slots"] = slots;
  }
  j["handEdited"] = true;
  // The P2 shape: one top-level key per line, one slot per line, the
  // generator block indented.
  std::string t = "{\n";
  size_t i = 0;
  for (auto it = j.begin(); it != j.end(); ++it, ++i) {
    t += "  " + Json(it.key()).dump() + ": ";
    const Json& v = it.value();
    if (it.key() == "slots" && v.is_array() && !v.empty()) {
      t += "[\n";
      for (size_t k = 0; k < v.size(); k++)
        t += "    " + v[k].dump() + (k + 1 < v.size() ? ",\n" : "\n");
      t += "  ]";
    } else if (v.is_object() && it.key() == "generator") {
      std::string d = v.dump(2);
      std::string ind;
      for (char c : d) {
        ind += c;
        if (c == '\n') ind += "  ";
      }
      t += ind;
    } else {
      t += v.dump();
    }
    t += i + 1 < j.size() ? ",\n" : "\n";
  }
  t += "}\n";
  jsonText = t;
  return true;
}

bool StructEdit::Save(const std::string& assetDir, const std::vector<std::string>& matNames,
                      std::string& err, std::string* oldVox, std::string* oldJson) {
  std::string vox, js;
  if (!BuildFiles(matNames, vox, js, err)) return false;
  const std::string base = assetDir + "/structures/" + name;
  if (oldVox) ReadFile(base + ".vox", *oldVox);
  if (oldJson) ReadFile(base + ".struct.json", *oldJson);
  if (!WriteFileAtomic(base + ".vox", vox, err)) return false;
  if (!WriteFileAtomic(base + ".struct.json", js, err)) return false;
  // What is on disk now is the truth: re-read the frame it was written in.
  try {
    json_ = Json::parse(js);
  } catch (...) {
  }
  IVec3 lo, hi;
  Bounds(lo, hi);
  origin_ = {-lo.x, -lo.y, -lo.z};
  size_ = {hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1};
  MarkClean();
  return true;
}

}  // namespace editor
