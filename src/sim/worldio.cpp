#include "sim/worldio.h"

#include <algorithm>
#include <cstdio>

namespace {
// 'SVM4': grew the voxel-size bit pattern + the material name table. Bumped
// from SVM3 so an old build refuses a new save outright instead of misreading
// the longer header.
constexpr uint32_t kMetaMagic = 0x344D5653;  // 'SVM4'
constexpr uint32_t kEntMagic = 0x31455653;   // 'SVE1'

std::string MetaPath(const std::string& dir) { return dir + "/meta.svm"; }
std::string EntPath(const std::string& dir) { return dir + "/entities.sve"; }

std::string FourCCStr(uint32_t id) {
  char s[5] = {(char)(id & 0xFF), (char)((id >> 8) & 0xFF),
               (char)((id >> 16) & 0xFF), (char)((id >> 24) & 0xFF), 0};
  for (char& c : s)
    if (c && (c < 32 || c > 126)) c = '?';
  return s;
}

// Read a whole file into `out`. False (with no complaint) when absent.
bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (!fp) return false;
  std::fseek(fp, 0, SEEK_END);
  long len = std::ftell(fp);
  std::fseek(fp, 0, SEEK_SET);
  out.resize(len > 0 ? (size_t)len : 0);
  bool ok = out.empty() || std::fread(out.data(), 1, out.size(), fp) == out.size();
  std::fclose(fp);
  return ok;
}

// ---- entities.sve -----------------------------------------------------------

bool WriteEntities(const std::string& dir, const EntityIO& entities) {
  std::vector<uint8_t> buf;
  ByteWriter w{buf};
  w.U32(kEntMagic);
  w.U32((uint32_t)entities.sections.size());
  std::vector<uint8_t> payload;
  for (const EntitySection& s : entities.sections) {
    payload.clear();
    if (s.save) s.save(payload);
    w.U32(s.id);
    w.U32(s.version);
    w.U32((uint32_t)payload.size());
    w.Bytes(payload.data(), payload.size());
  }
  FILE* fp = std::fopen(EntPath(dir).c_str(), "wb");
  if (!fp) return false;
  bool ok = std::fwrite(buf.data(), 1, buf.size(), fp) == buf.size();
  std::fclose(fp);
  return ok;
}

void LoadEntities(const std::string& dir, const EntityIO& entities) {
  // Reset EVERY registered system first, whether or not the file (or its
  // section) exists: entities from the session being replaced must not stand
  // in the loaded world, and an older grid-only save must load into an empty
  // entity state rather than a haunted one.
  for (const EntitySection& s : entities.sections)
    if (s.reset) s.reset();

  std::vector<uint8_t> buf;
  if (!ReadFileBytes(EntPath(dir), buf)) return;  // absent: nothing to apply
  ByteReader r{buf.data(), buf.size()};
  uint32_t magic = 0, count = 0;
  if (!r.U32(magic) || magic != kEntMagic || !r.U32(count)) {
    std::fprintf(stderr, "load: entities.sve is corrupt (bad header)\n");
    return;
  }
  for (uint32_t i = 0; i < count; i++) {
    uint32_t id = 0, version = 0, len = 0;
    if (!r.U32(id) || !r.U32(version) || !r.U32(len) || r.off + len > r.n) {
      std::fprintf(stderr, "load: entities.sve truncated at section %u\n", i);
      return;
    }
    const uint8_t* payload = r.p + r.off;
    r.off += len;  // the table lets us seek past ANY section, known or not
    const EntitySection* match = nullptr;
    for (const EntitySection& s : entities.sections)
      if (s.id == id) match = &s;
    if (!match || !match->load) {
      // Forward compatibility: a newer build's section is skipped, loudly.
      std::printf("load: skipping unknown entity section '%s' (v%u, %u bytes)\n",
                  FourCCStr(id).c_str(), version, len);
      continue;
    }
    if (!match->load(payload, len, version))
      std::fprintf(stderr,
                   "load: entity section '%s' (v%u) failed to apply; that "
                   "system starts empty\n",
                   FourCCStr(id).c_str(), version);
  }
}

// ---- meta.svm ---------------------------------------------------------------

uint32_t VoxelMetersBits() {
  uint32_t bits = 0;
  static_assert(sizeof(bits) == sizeof(kVoxelMeters), "float32 expected");
  std::memcpy(&bits, &kVoxelMeters, sizeof(bits));
  return bits;
}

}  // namespace

bool SaveWorld(GpuContext& ctx, World& world, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities) {
  ChunkStore& store = stream.Store();
  if (store.Bound() && store.Dir() != path) {
    std::fprintf(stderr, "save: store is bound to %s (one world dir per session)\n",
                 store.Dir().c_str());
    return false;
  }

  // resident window -> store (unfiltered: air chunks too, so a re-fill needs
  // no snapshot trust; drains in-flight async evictions)
  stream.FlushResident();

  if (!store.BindSave(path)) return false;
  size_t regions = 0;
  uint64_t bytes = 0;
  if (!store.Flush(&regions, &bytes)) return false;

  // Entities BEFORE meta, for the same reason meta comes last at all: meta's
  // presence marks a completed save, so everything it vouches for must already
  // be on disk. A grid-only save removes any stale entities.sve — pairing an
  // old entity file with a new grid would resurrect bodies over terrain that
  // no longer matches.
  if (entities) {
    if (!WriteEntities(path, *entities)) {
      std::fprintf(stderr, "save: failed to write entities.sve\n");
      return false;
    }
  } else {
    std::remove(EntPath(path).c_str());
  }

  // meta last: its presence marks a completed save
  FILE* fp = std::fopen(MetaPath(path).c_str(), "wb");
  if (!fp) return false;
  {
    std::vector<uint8_t> meta;
    ByteWriter w{meta};
    w.U32(kMetaMagic);
    w.U32(kWorldN);
    w.U32(kChunk);
    // Exact bit pattern, compared bitwise on load: a float compare with any
    // tolerance would wave through "close" voxel sizes, and there is no such
    // thing — the grid is authored in voxels, so any change rescales it.
    w.U32(VoxelMetersBits());
    IVec3 o = world.WindowOrigin();
    w.Pod(o.x);
    w.Pod(o.y);
    w.Pod(o.z);
    // The full material NAME table, not just a hash: material IDs are baked
    // into every saved chunk (world.h: append, never reorder), and storing the
    // names lets a refused load say exactly WHICH id changed meaning instead
    // of "hash mismatch". ~50 names is a few hundred bytes.
    w.U32((uint32_t)mats.size());
    for (const MaterialDef& m : mats) w.Str(m.name);
    bool ok = std::fwrite(meta.data(), 1, meta.size(), fp) == meta.size();
    std::fclose(fp);
    if (!ok) return false;
  }
  std::printf("saved %s (%.2f MB across %zu regions)\n", path.c_str(),
              bytes / 1e6, regions);
  return true;
}

bool LoadWorld(GpuContext& ctx, World& world, Simulation& sim, Stream& stream,
               const std::string& path, const std::vector<MaterialDef>& mats,
               const EntityIO* entities) {
  std::vector<uint8_t> meta;
  if (!ReadFileBytes(MetaPath(path), meta)) {
    std::fprintf(stderr, "load: %s has no meta.svm\n", path.c_str());
    return false;
  }
  ByteReader r{meta.data(), meta.size()};
  uint32_t magic = 0, worldN = 0, chunk = 0, vmBits = 0;
  int32_t origin[3] = {};
  r.U32(magic);
  r.U32(worldN);
  r.U32(chunk);
  r.U32(vmBits);
  r.Pod(origin[0]);
  r.Pod(origin[1]);
  r.Pod(origin[2]);
  if (!r.ok || magic != kMetaMagic) {
    // Includes SVM3-and-earlier saves: refusing beats guessing at a header we
    // cannot verify (the old format recorded neither voxel size nor materials,
    // exactly the two silent-corruption axes this header exists to close).
    std::fprintf(stderr,
                 "load: %s is not a compatible world dir (bad or pre-SVM4 "
                 "meta.svm magic %08x, want %08x)\n",
                 path.c_str(), magic, kMetaMagic);
    return false;
  }
  if (worldN != kWorldN || chunk != kChunk) {
    std::fprintf(stderr,
                 "load: %s world constants mismatch (saved N=%u chunk=%u, "
                 "build has N=%u chunk=%u)\n",
                 path.c_str(), worldN, chunk, kWorldN, kChunk);
    return false;
  }
  if (vmBits != VoxelMetersBits()) {
    float savedVm = 0;
    std::memcpy(&savedVm, &vmBits, sizeof(savedVm));
    // Bit patterns as well as values: two floats can round-trip to the same
    // %g text and still differ, and the compare is bitwise on purpose.
    std::fprintf(stderr,
                 "load: %s was saved at kVoxelMeters=%.9g (bits %08x), this "
                 "build is %.9g (bits %08x) — the world would load at the "
                 "wrong physical scale\n",
                 path.c_str(), savedVm, vmBits, kVoxelMeters, VoxelMetersBits());
    return false;
  }
  {
    uint32_t savedCount = 0;
    r.U32(savedCount);
    std::vector<std::string> names(savedCount);
    for (uint32_t i = 0; i < savedCount && r.ok; i++) r.Str(names[i]);
    if (!r.ok) {
      std::fprintf(stderr, "load: %s meta.svm material table is truncated\n",
                   path.c_str());
      return false;
    }
    // Saved chunks reference materials BY ID; anything but an append means an
    // old id now names a different substance (stone chunks turning to blood
    // with a green build is the failure this refuses).
    const uint32_t common = std::min<uint32_t>(savedCount, (uint32_t)mats.size());
    for (uint32_t i = 0; i < common; i++) {
      if (names[i] != mats[i].name) {
        std::fprintf(stderr,
                     "load: %s material table mismatch at id %u: saved "
                     "'%s', this build has '%s' (materials.json was reordered "
                     "or renamed — saved chunks would decode as the wrong "
                     "materials)\n",
                     path.c_str(), i, names[i].c_str(), mats[i].name.c_str());
        return false;
      }
    }
    if (savedCount > (uint32_t)mats.size()) {
      std::fprintf(stderr,
                   "load: %s uses %u materials but this build has only %zu "
                   "(first missing: '%s')\n",
                   path.c_str(), savedCount, mats.size(),
                   names[mats.size()].c_str());
      return false;
    }
    // savedCount < mats.size() is fine: appending materials is the sanctioned
    // way to grow the table, and old saves simply never reference the new ids.
  }

  ChunkStore& store = stream.Store();
  if (!store.BindLoad(path)) {
    std::fprintf(stderr, "load: store is bound to %s (one world dir per session)\n",
                 store.Bound() ? store.Dir().c_str() : "?");
    return false;
  }

  {
    // Snapshot restore (worldgen-equivalent), not a live mutation: the direct
    // upload path in FillSlots is sanctioned the same way worldgen's direct
    // writes are. All GAMEPLAY writes still flow through the MutationQueue.
    // The held readback snapshot describes the PRE-LOAD world and must not be
    // consumable afterwards (see World::InvalidateSnapshot).
    world.InvalidateSnapshot();
    stream.ReloadWindow({origin[0], origin[1], origin[2]});
    rhi::CommandEncoder enc = ctx.device.CreateCommandEncoder();
    sim.EncodeLoadReset(enc);
    rhi::CommandBuffer cmd = enc.Finish();
    ctx.queue.Submit(1, &cmd);
  }

  // Entities after the grid: their load paths (Jolt bodies, terrain anchors)
  // read the world that is now in place. With no EntityIO the entity file is
  // ignored entirely — grid-only callers keep their exact old behaviour.
  if (entities) LoadEntities(path, *entities);

  std::printf("loaded %s (%zu chunks in RAM after window fill)\n", path.c_str(),
              stream.Store().Count());
  return true;
}
