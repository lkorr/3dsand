#include "audio/library.h"

#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace audio {
namespace {

bool IsAudioFile(const std::filesystem::path& p) {
  std::string e = p.extension().string();
  std::transform(e.begin(), e.end(), e.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return e == ".wav" || e == ".flac" || e == ".mp3" || e == ".ogg";
}

// Set name for a file: its directory path relative to the sound root, using
// '/' separators. A file sitting directly in the root becomes a set of its own
// named after the file, so both layouts work:
//
//   footsteps/leaf/leaf_03.wav  -> "footsteps/leaf"   (folder of variants)
//   ambience/wind.wav           -> "ambience/wind"    (lone file)
std::string SetNameFor(const std::filesystem::path& file,
                       const std::filesystem::path& root) {
  std::filesystem::path rel = std::filesystem::relative(file, root);
  std::filesystem::path dir = rel.parent_path();
  std::filesystem::path named = dir.empty() ? rel.stem() : dir;
  if (!dir.empty()) {
    // A folder whose files are its variants. Keep the folder path as the name.
    named = dir;
  }
  std::string s = named.generic_string();
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  std::replace(s.begin(), s.end(), ' ', '_');
  return s;
}

}  // namespace

int Library::Init(double sampleRate, std::string soundDir) {
  sampleRate_ = sampleRate;
  dir_ = std::move(soundDir);
  return ScanDir();
}

int Library::Rescan() { return ScanDir(); }

int Library::ScanDir() {
  namespace fs = std::filesystem;
  warnings_.clear();
  std::error_code ec;
  if (dir_.empty() || !fs::is_directory(dir_, ec)) return 0;

  // Sorted so set ids and variant order are deterministic across runs and
  // across platforms -- a random-variant pick seeded the same way must produce
  // the same sequence, or a recorded repro stops reproducing.
  std::vector<fs::path> files;
  for (auto it = fs::recursive_directory_iterator(
           dir_, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    // `raw/` holds the uncut source takes that scripts/split_footsteps.py cuts
    // from -- 11-16 second performances, not playable assets. Decoding them
    // would hold megabytes of samples nothing can trigger, and would expose a
    // bogus "raw" set to the material mapping. Skipped by DIRECTORY so the
    // takes can stay next to the assets they produced.
    if (it->is_directory(ec) && it->path().filename() == "raw") {
      it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec)) continue;
    if (IsAudioFile(it->path())) files.push_back(it->path());
  }
  std::sort(files.begin(), files.end());

  int loaded = 0;
  for (const fs::path& p : files) {
    const std::string key = p.generic_string();
    if (loadedFiles_.count(key)) continue;
    loadedFiles_.insert(key);  // insert before decoding: a failure is not retried

    // One call does all three conversions the engine needs:
    //   f32          -- the engine's sample format
    //   1 channel    -- MONO, because the spatializer synthesizes the stereo
    //                   image itself from the emitter position. A stereo asset
    //                   would arrive with its own baked image fighting the
    //                   panner, which smears direction and defeats the point.
    //   sampleRate_  -- the rate the DEVICE actually negotiated, not a
    //                   hardcoded 48k.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, (ma_uint32)sampleRate_);
    ma_uint64 frames = 0;
    void* pcm = nullptr;
    if (ma_decode_file(p.string().c_str(), &cfg, &frames, &pcm) != MA_SUCCESS) {
      warnings_.push_back("failed to decode " + key);
      continue;
    }
    // Mono, so frame count == sample count.
    std::vector<float> samples((const float*)pcm, (const float*)pcm + frames);
    ma_free(pcm, nullptr);
    if (samples.empty()) {
      warnings_.push_back("empty after decode: " + key);
      continue;
    }

    const int id = GetOrAddSet(SetNameFor(p, fs::path(dir_)));
    sets_[(size_t)id]->variants.push_back(
        std::make_unique<std::vector<float>>(std::move(samples)));
    loaded++;
  }
  return loaded;
}

int Library::GetOrAddSet(const std::string& name) {
  for (size_t i = 0; i < sets_.size(); i++)
    if (sets_[i]->name == name) return (int)i;
  auto s = std::make_unique<SoundSet>();
  s->name = name;
  sets_.push_back(std::move(s));
  return (int)sets_.size() - 1;
}

int Library::Find(const std::string& name) const {
  for (size_t i = 0; i < sets_.size(); i++)
    if (sets_[i]->name == name) return (int)i;
  return -1;
}

const char* Library::NameOf(int id) const {
  if (id < 0 || id >= Count()) return "none";
  return sets_[(size_t)id]->name.c_str();
}

const std::vector<float>* Library::Variant(int id, size_t v) const {
  if (!Valid(id)) return nullptr;
  const SoundSet& s = Set(id);
  return s.variants[v % s.variants.size()].get();
}

const std::vector<float>* Library::RandomVariant(int id, std::mt19937& rng) const {
  if (!Valid(id)) return nullptr;
  const SoundSet& s = Set(id);
  std::uniform_int_distribution<size_t> d(0, s.variants.size() - 1);
  return s.variants[d(rng)].get();
}

const std::vector<float>* Library::RandomVariantNoRepeat(int id, std::mt19937& rng,
                                                         int& avoid) const {
  if (!Valid(id)) return nullptr;
  const SoundSet& s = Set(id);
  const size_t n = s.variants.size();
  size_t pick;
  if (n < 2) {
    pick = 0;
  } else {
    // Draw from the n-1 variants that are not `avoid`, then shift past it.
    // Cheaper and better-distributed than rejection-sampling in a loop.
    std::uniform_int_distribution<size_t> d(0, n - 2);
    pick = d(rng);
    if (avoid >= 0 && pick >= (size_t)avoid) pick++;
  }
  avoid = (int)pick;
  return s.variants[pick].get();
}

}  // namespace audio
