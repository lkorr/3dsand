#pragma once
// Named registry of sound assets. A SoundSet is one logical sound with one or
// more mono variant buffers; a trigger picks a variant at random (footsteps,
// impacts) or by a stable index (loops, so two emitters sharing a set stay
// decorrelated).
//
// LAYOUT. Sets come from `assets/sounds/`, and a set is a FOLDER:
//
//     assets/sounds/footsteps/leaf/leaf_01.wav   -> set "footsteps/leaf"
//     assets/sounds/footsteps/leaf/leaf_02.wav      (variant 2 of the same set)
//     assets/sounds/ambience/wind.wav            -> set "ambience/wind"
//
// The set name is the path relative to `assets/sounds/` with the filename
// dropped when the file sits in a subfolder of its own. A folder of variants is
// the whole convention -- there is no manifest to keep in sync, so adding a
// 12th leaf step is dropping a file in and pressing the rescan key. That is
// deliberately the same "data, not code" stance materials.json takes.
//
// AUDIO-THREAD SAFETY, and why the double indirection is not an accident.
// Buffers live behind unique_ptr inside a vector. The vector may reallocate
// when a rescan appends a set, but the unique_ptr PAYLOADS never move, so a
// `const std::vector<float>*` handed to a playing voice stays valid for the
// lifetime of the library. The registry is therefore append-only: Rescan() may
// run on the game thread while audio plays, and nothing is ever freed or
// reallocated out from under a voice. Do not add a remove/replace operation
// without solving reclamation first -- a voice may hold that pointer.

#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace audio {

struct SoundSet {
  std::string name;
  std::vector<std::unique_ptr<std::vector<float>>> variants;
};

class Library {
 public:
  // Scans `soundDir` recursively. A missing directory is not an error -- the
  // game runs silent rather than refusing to start (DESIGN.md §6: diagnostics,
  // not breakage). Returns the number of variant buffers loaded.
  int Init(double sampleRate, std::string soundDir);

  // Loads any files that appeared since the last scan. Never removes or
  // reallocates existing buffers, so this is safe while audio is playing.
  int Rescan();

  int Count() const { return (int)sets_.size(); }
  const SoundSet& Set(int id) const { return *sets_[(size_t)id]; }
  bool Valid(int id) const {
    return id >= 0 && id < Count() && !Set(id).variants.empty();
  }

  // -1 if no set has that name. Names are as described above, e.g.
  // "footsteps/leaf". Game thread (does a linear scan; call at load, not per
  // trigger -- callers cache the id).
  int Find(const std::string& name) const;
  const char* NameOf(int id) const;

  // nullptr when the id or variant is out of range.
  const std::vector<float>* Variant(int id, size_t v) const;
  const std::vector<float>* RandomVariant(int id, std::mt19937& rng) const;
  // Random variant that is not `avoid` when the set has >= 2 of them. Repeating
  // the same footstep sample twice in a row is the single most recognizable
  // "this is a game" tell, and it is far more audible than any spectral detail.
  // Returns the chosen index through `avoid`.
  const std::vector<float>* RandomVariantNoRepeat(int id, std::mt19937& rng,
                                                  int& avoid) const;

  const std::string& Dir() const { return dir_; }
  // Diagnostics from the last scan (files that failed to decode, etc).
  const std::vector<std::string>& Warnings() const { return warnings_; }

 private:
  int GetOrAddSet(const std::string& name);
  int ScanDir();

  double sampleRate_ = 48000.0;
  std::string dir_;
  std::vector<std::unique_ptr<SoundSet>> sets_;
  std::set<std::string> loadedFiles_;  // paths already decoded (or failed)
  std::vector<std::string> warnings_;
};

}  // namespace audio
