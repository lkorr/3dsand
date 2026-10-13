#include "ui/framering.h"

#include <algorithm>
#include <cstdio>
#include <utility>

void BugFrameRing::Ensure(const rhi::Device& dev, uint32_t w, uint32_t h,
                          rhi::TextureFormat fmt) {
  if (w == fw_ && h == fh_ && fmt == fmt_ && tex_[0]) return;
  fw_ = w;
  fh_ = h;
  fmt_ = fmt;
  // Quarter size: a 4x4 sheet of them is the window's own size again.
  tw_ = std::max(1u, w / kCols);
  th_ = std::max(1u, h / kCols);
  for (int i = 0; i < kTiles; i++) {
    tex_[i] = dev.CreateTexture({tw_, th_, 1}, fmt,
                                rhi::TextureUsage::CopyDst | rhi::TextureUsage::CopySrc,
                                "bugRingTile");
    view_[i] = tex_[i].CreateView();
  }
  Clear();
}

bool BugFrameRing::Due(double now) const {
  // A couple of ms early is fine: at 60 fps the tile period (62.5 ms) is not
  // a whole number of frames, and waiting for the NEXT frame every time
  // would drift the sheet to 5-frame spacing (83 ms) and short of a second.
  return now - last_ >= kSpanSec / kTiles - 0.008;
}

void BugFrameRing::Capture(const rhi::CommandEncoder& enc, const rhi::TextureView& src,
                           double now) {
  if (!tex_[0]) return;
  enc.BlitTexture(src, view_[head_], /*linear=*/true);
  t_[head_] = now;
  held_[head_] = true;
  head_ = (head_ + 1) % kTiles;
  last_ = now;
}

void BugFrameRing::Clear() {
  for (int i = 0; i < kTiles; i++) held_[i] = false;
  head_ = 0;
  last_ = -1e9;
}

bool BugFrameRing::ReadSheet(const rhi::Device& dev, const rhi::Queue& queue, bool bgra,
                             double now, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h,
                             std::string& note) {
  std::vector<int> pick;
  for (int i = 0; i < kTiles; i++)
    if (held_[i] && t_[i] >= now - kSpanSec - 0.001 && t_[i] <= now + 0.001) pick.push_back(i);
  if (pick.empty()) return false;
  std::sort(pick.begin(), pick.end(), [&](int a, int b) { return t_[a] < t_[b]; });

  const uint64_t tileBytes = (uint64_t)tw_ * th_ * 4;
  rhi::Buffer buf = dev.CreateBuffer(tileBytes * pick.size(),
                                     rhi::BufferUsage::MapRead | rhi::BufferUsage::CopyDst,
                                     "bugRingRead");
  rhi::CommandEncoder enc = dev.CreateCommandEncoder();
  for (size_t k = 0; k < pick.size(); k++) {
    rhi::TexelCopyTexture src{};
    src.texture = tex_[pick[k]];
    rhi::TexelCopyBuffer dst{};
    dst.buffer = buf;
    dst.offset = tileBytes * k;
    dst.bytesPerRow = tw_ * 4;
    dst.rowsPerImage = th_;
    enc.CopyTextureToBuffer(src, dst, {tw_, th_, 1});
  }
  queue.Submit(enc.Finish());
  dev.WaitIdle();
  std::vector<uint8_t> px((size_t)(tileBytes * pick.size()));
  if (!rhi::ReadBufferBlocking(dev, buf, 0, px.data(), px.size())) return false;
  if (bgra)
    for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);

  std::vector<const uint8_t*> tiles;
  for (size_t k = 0; k < pick.size(); k++) tiles.push_back(px.data() + tileBytes * k);
  ComposeContactSheet(tiles, tw_, th_, kCols, rgba, w, h);

  // The sheet carries no text of its own (no font on the CPU side), so the
  // ages go in the shot's section: one number per tile, in reading order.
  char head[160];
  std::snprintf(head, sizeof head,
                "- contact sheet: the last %.1f s, %zu tiles of %ux%u, %d per row, oldest "
                "top-left, read left to right; the last tile is the F12 frame\n"
                "- tile age (ms before F12):",
                kSpanSec, pick.size(), tw_, th_, kCols);
  note = head;
  for (size_t k = 0; k < pick.size(); k++) {
    char a[24];
    std::snprintf(a, sizeof a, "%s %.0f", k ? "," : "", (now - t_[pick[k]]) * 1000.0);
    note += a;
  }
  note += "\n";
  return true;
}

void ComposeContactSheet(const std::vector<const uint8_t*>& tiles, uint32_t tw, uint32_t th,
                         int cols, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
  constexpr uint32_t kGut = 2;
  const uint32_t n = (uint32_t)tiles.size();
  const uint32_t c = std::min<uint32_t>((uint32_t)std::max(1, cols), std::max(1u, n));
  const uint32_t r = (n + c - 1) / c;
  w = c * tw + (c - 1) * kGut;
  h = r * th + (r > 0 ? (r - 1) * kGut : 0);
  rgba.assign((size_t)w * h * 4, 0);
  for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;  // gutter: opaque black
  for (uint32_t k = 0; k < n; k++) {
    const uint32_t x0 = (k % c) * (tw + kGut), y0 = (k / c) * (th + kGut);
    for (uint32_t y = 0; y < th; y++) {
      const uint8_t* s = tiles[k] + (size_t)y * tw * 4;
      uint8_t* d = rgba.data() + ((size_t)(y0 + y) * w + x0) * 4;
      for (uint32_t x = 0; x < tw; x++) {
        d[x * 4 + 0] = s[x * 4 + 0];
        d[x * 4 + 1] = s[x * 4 + 1];
        d[x * 4 + 2] = s[x * 4 + 2];
        d[x * 4 + 3] = 255;  // the swapchain's alpha means nothing in a PNG
      }
    }
  }
}
