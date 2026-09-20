#include "game/sidecar.h"

#include <cmath>
#include <filesystem>
#include <fstream>

namespace sidecar {
namespace {

// A NAMED ARRAY is one whose every element is an object with a string `name`.
// The predicate is deliberately all-or-nothing: `limbs`, `sockets`, `natural`
// and `states` qualify, and `mask`, `groups`, `parts`, `axis` and every
// coordinate triple do not, so the RFC rule still covers everything that is a
// VALUE rather than a list of things. One unnamed element opts the whole array
// back out, which is the conservative direction — a replace where a merge was
// wanted is visible in the file, a merge where a replace was wanted is not.
bool AllNamed(const json& a) {
  if (!a.is_array() || a.empty()) return false;
  for (const json& e : a)
    if (!e.is_object() || !e.contains("name") || !e["name"].is_string())
      return false;
  return true;
}

const json* FindNamed(const json& arr, const std::string& name) {
  for (const json& e : arr)
    if (e.is_object() && e.contains("name") && e["name"].is_string() &&
        e["name"].get<std::string>() == name)
      return &e;
  return nullptr;
}

// Is `target` at `path` a member of the top-level `clips` object? That is the
// one place the retime rule fires, and asking by path keeps the rule out of
// the generic merge — `clips` is an object of clips and a clip is an object of
// anything, so there is no structural test that would not also fire on a limb
// that happened to carry a `durationMs`.
bool IsClipSlot(const std::string& path) {
  if (path.rfind("/clips/", 0) != 0) return false;
  return path.find('/', 7) == std::string::npos;
}

void MergeNamedArray(json& target, const json& patch,
                     const std::string& path) {
  json out = json::array();
  for (const json& base : target) {
    const std::string nm = base["name"].get<std::string>();
    const json* over = FindNamed(patch, nm);
    json merged = base;
    if (over) MergePatch(merged, *over, path + "/" + nm);
    out.push_back(std::move(merged));
  }
  // Unmatched names APPEND, after the base's own order. A character that adds
  // a tail gets it at the end; it never displaces a limb the save file counts
  // on finding where it was.
  for (const json& p : patch)
    if (!FindNamed(target, p["name"].get<std::string>())) out.push_back(p);
  target = std::move(out);
}

// JS Math.round on a non-negative value, so the C++ and the JS mirror land on
// the same millisecond. Key times are never negative.
int RoundHalfUp(double v) { return (int)std::floor(v + 0.5); }

}  // namespace

// ---- clip retime ------------------------------------------------------------
//
// Scaling the KEY TIMES and not the blends: a blend is how long the clip takes
// to fade in, which is a property of the transition and not of the stride.
void RetimeClip(json& clip, int oldMs, int newMs) {
  if (oldMs <= 0 || newMs <= 0 || oldMs == newMs) return;
  if (!clip.contains("tracks") || !clip["tracks"].is_object()) return;
  const double ratio = (double)newMs / (double)oldMs;
  for (auto& track : clip["tracks"]) {
    if (!track.is_object()) continue;
    for (const char* chan : {"rot", "pos"}) {
      if (!track.contains(chan) || !track[chan].is_array()) continue;
      for (auto& key : track[chan])
        if (key.is_object() && key.contains("t") && key["t"].is_number())
          key["t"] = RoundHalfUp(key["t"].get<double>() * ratio);
    }
  }
}

// ---- the merge --------------------------------------------------------------

void MergePatch(json& target, const json& patch, const std::string& path) {
  if (!patch.is_object()) {
    target = patch;  // a scalar or an array replaces (RFC 7396)
    return;
  }
  if (!target.is_object()) target = json::object();
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    const std::string& k = it.key();
    const json& v = it.value();
    if (v.is_null()) {
      target.erase(k);
      continue;
    }
    const std::string sub = path + "/" + k;
    if (!target.contains(k)) {
      target[k] = v;
      continue;
    }
    // THE RETIME, and it has to happen BEFORE the duration is written: the
    // scale factor is new/old and the old one is about to be overwritten.
    if (IsClipSlot(sub) && v.is_object() && v.contains("durationMs") &&
        v["durationMs"].is_number() && !v.contains("tracks") &&
        target[k].is_object() && target[k].contains("durationMs") &&
        target[k]["durationMs"].is_number())
      RetimeClip(target[k], target[k]["durationMs"].get<int>(),
                 v["durationMs"].get<int>());
    if (AllNamed(target[k]) && AllNamed(v)) {
      MergeNamedArray(target[k], v, sub);
      continue;
    }
    MergePatch(target[k], v, sub);
  }
}

// ---- effects ----------------------------------------------------------------

void ApplyEffect(json& def, const json& effect, const std::string& where,
                 std::string& log) {
  if (!effect.is_object()) {
    log += where + ": effect is not a JSON object — ignored\n";
    return;
  }
  if (effect.contains("patch")) {
    if (effect["patch"].is_object()) MergePatch(def, effect["patch"], "");
    else log += where + ": effect `patch` is not an object — ignored\n";
  }
  if (!effect.contains("scale")) return;
  if (!effect["scale"].is_object()) {
    log += where + ": effect `scale` is not an object — ignored\n";
    return;
  }
  // A DOTTED PATH TO A NUMBER, multiplied in place. No array indices and no
  // wildcards: an effect that needs to reach into a list is describing a
  // different body, which is what `patch` is for.
  for (auto it = effect["scale"].begin(); it != effect["scale"].end(); ++it) {
    if (!it.value().is_number()) {
      log += where + ": scale." + it.key() + " is not a number — ignored\n";
      continue;
    }
    const double factor = it.value().get<double>();
    json* node = &def;
    size_t at = 0;
    bool reached = true;
    const std::string& p = it.key();
    while (at < p.size()) {
      const size_t dot = p.find('.', at);
      const std::string seg =
          p.substr(at, dot == std::string::npos ? std::string::npos : dot - at);
      if (!node->is_object() || !node->contains(seg)) {
        reached = false;
        break;
      }
      node = &(*node)[seg];
      at = dot == std::string::npos ? p.size() : dot + 1;
    }
    if (!reached || !node->is_number()) {
      log += where + ": scale." + p +
             " names nothing this body has a number at — ignored\n";
      continue;
    }
    // ALWAYS A DOUBLE, even where the base wrote an integer. `cadence: 8`
    // scaled by 0.8 is 6.4 — rounding it back to a whole number because the
    // author happened to omit a decimal point would silently quantise every
    // ratio an effect is made of, and stride is speed/cadence, so it would
    // quantise the walk. The handful of keys that are genuinely COUNTS
    // (rot.bites) are read through lround at the loader anyway.
    *node = node->get<double>() * factor;
  }
}

// ---- reading and resolving --------------------------------------------------

bool ReadJsonDoc(const std::string& path, json& out, std::string& log) {
  std::ifstream f(path);
  if (!f) {
    log += path + ": missing sidecar\n";
    return false;
  }
  try {
    out = json::parse(f);
  } catch (const std::exception& e) {
    log += path + ": JSON parse error: " + std::string(e.what()) + "\n";
    return false;
  }
  if (!out.is_object()) {
    log += path + ": sidecar is not a JSON object — skipped\n";
    return false;
  }
  return true;
}

namespace {

bool ResolveExtends(const std::string& dir, const std::string& path, json& out,
                    std::string& log, int depth) {
  if (depth > kMaxExtends) {
    log += path + ": `extends` nested more than " + std::to_string(kMaxExtends) +
           " deep (a cycle?) — skipped\n";
    return false;
  }
  json child;
  if (!ReadJsonDoc(path, child, log)) return false;
  const std::string base = child.value("extends", std::string());
  if (base.empty()) {
    out = std::move(child);
    return true;
  }
  json parent;
  const std::string bp =
      (std::filesystem::path(dir) / (base + ".json")).string();
  if (!ResolveExtends(dir, bp, parent, log, depth + 1)) {
    log += path + ": extends \"" + base + "\", which did not load — skipped\n";
    return false;
  }
  // `effects` ACCUMULATES, base first, deduped. A creature that extends a
  // zombie is a zombie, and the plain array-replace rule would have made
  // "inherit the zombie and change one thing" silently drop the zombie. An
  // explicit `null` still clears the list — that is the RFC rule, applied by
  // the merge below, and it is the deliberate way off.
  if (child.contains("effects") && child["effects"].is_array() &&
      parent.contains("effects") && parent["effects"].is_array()) {
    json combined = parent["effects"];
    for (const json& e : child["effects"]) {
      bool seen = false;
      for (const json& c : combined) seen = seen || c == e;
      if (!seen) combined.push_back(e);
    }
    child["effects"] = std::move(combined);
  }
  // Not inherited: the chain is resolved here, once, and a def that carried
  // the key downstream would invite a second reader to resolve it again
  // against a different base.
  child.erase("extends");
  MergePatch(parent, child, "");
  out = std::move(parent);
  return true;
}

}  // namespace

bool Load(const std::string& dir, const std::string& path, json& out,
          std::string& log) {
  if (!ResolveExtends(dir, path, out, log, 0)) return false;
  if (!out.contains("effects")) return true;
  if (!out["effects"].is_array()) {
    log += path + ": `effects` is not an array of names — ignored\n";
    out.erase("effects");
    return true;
  }
  const std::filesystem::path fxDir = std::filesystem::path(dir) / "effects";
  for (const json& e : out["effects"]) {
    if (!e.is_string()) {
      log += path + ": an `effects` entry is not a name — ignored\n";
      continue;
    }
    const std::string fp = (fxDir / (e.get<std::string>() + ".json")).string();
    json fx;
    // A MISSING EFFECT IS LOUD BUT NOT FATAL. The def is the creature; the
    // effect is a coat of paint on it, and dropping the whole character
    // because a modifier file was renamed would lose the body as well.
    if (!ReadJsonDoc(fp, fx, log)) {
      log += path + ": effect \"" + e.get<std::string>() +
             "\" did not load — this body is unmodified\n";
      continue;
    }
    ApplyEffect(out, fx, fp, log);
  }
  return true;
}

}  // namespace sidecar
