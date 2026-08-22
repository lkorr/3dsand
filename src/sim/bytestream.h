#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Byte stream helpers for the entity save payloads (sim/worldio.h,
// entities.sve). In their own header so the systems that serialize themselves
// (phys/debris, game/mob, game/avatar) don't drag in worldio's GPU/sim stack.
//
// Raw little-endian PODs: this is a local save format for one machine class,
// not a wire format. Every read is bounds-checked and `ok` is sticky, so a
// truncated or hostile payload degrades to "section failed to apply" rather
// than reading past the buffer.

struct ByteWriter {
  std::vector<uint8_t>& out;
  void Bytes(const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    out.insert(out.end(), b, b + n);
  }
  template <typename T>
  void Pod(const T& v) {
    Bytes(&v, sizeof(T));
  }
  void U32(uint32_t v) { Pod(v); }
  void F32(float v) { Pod(v); }
  void Str(const std::string& s) {
    U32((uint32_t)s.size());
    Bytes(s.data(), s.size());
  }
  template <typename T>
  void PodVec(const std::vector<T>& v) {
    U32((uint32_t)v.size());
    Bytes(v.data(), v.size() * sizeof(T));
  }
};

struct ByteReader {
  const uint8_t* p;
  size_t n;
  size_t off = 0;
  bool ok = true;  // sticky: any short read poisons everything after it
  bool Bytes(void* dst, size_t len) {
    if (!ok || off + len > n) {
      ok = false;
      return false;
    }
    std::memcpy(dst, p + off, len);
    off += len;
    return true;
  }
  template <typename T>
  bool Pod(T& v) {
    return Bytes(&v, sizeof(T));
  }
  bool U32(uint32_t& v) { return Pod(v); }
  bool F32(float& v) { return Pod(v); }
  bool Str(std::string& s) {
    uint32_t len = 0;
    if (!U32(len) || off + len > n) {
      ok = false;
      return false;
    }
    s.assign((const char*)p + off, len);
    off += len;
    return true;
  }
  template <typename T>
  bool PodVec(std::vector<T>& v) {
    uint32_t count = 0;
    if (!U32(count)) return false;
    const size_t bytes = (size_t)count * sizeof(T);
    if (off + bytes > n) {
      ok = false;
      return false;
    }
    v.resize(count);
    std::memcpy(v.data(), p + off, bytes);
    off += bytes;
    return true;
  }
};
