#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gpu/rhi.h"

// THE BUG REPORT'S FRAME RING (F12, ui/bugreport.h): the last second of the
// presented frame, kept on the GPU at quarter size so F12 can file it as ONE
// contact-sheet picture instead of a single still.
//
// Why a sheet and not a GIF: whoever reads the ticket (an agent, usually)
// sees an image file as one still -- a GIF arrives as its first frame. A 4x4
// grid of the last second, oldest top-left, is the whole sequence in one read.
//
// Cost: the frame renders into a copyable texture rather than straight into
// the swapchain (whose images have no TRANSFER_SRC usage), which then costs
// one full-size blit to present, plus a quarter-size blit into the ring every
// 1/kTiles s. No readback until F12 -- the ring's one blocking read happens
// on the key press, the same one-frame hitch the still already took.
// SANDVOX_NO_BUG_RING=1 turns the whole thing off (the still still works).
class BugFrameRing {
 public:
  static constexpr int kTiles = 16;          // 4x4 sheet
  static constexpr int kCols = 4;
  static constexpr double kSpanSec = 1.0;    // how far back the sheet reaches

  // (Re)create the tiles for a `w` x `h` frame of `fmt`. A size change drops
  // what was held -- tiles of two sizes do not go on one sheet.
  void Ensure(const rhi::Device& dev, uint32_t w, uint32_t h, rhi::TextureFormat fmt);
  // Is a tile due at `now` (seconds)? The F12 frame is captured regardless.
  bool Due(double now) const;
  // Record a scaled copy of `src` (CopySrc) into the next tile.
  void Capture(const rhi::CommandEncoder& enc, const rhi::TextureView& src, double now);
  // Forget everything held (the frames before a resize, or before a pause).
  void Clear();

  // F12: read the tiles of the last kSpanSec before `now` back (BLOCKING --
  // key press only) and lay them out as one RGBA8 sheet, oldest first.
  // `bgra` = the frame format is BGRA8 (swizzled to RGBA here). `note` gets
  // the markdown lines naming each tile's age. False = nothing held.
  bool ReadSheet(const rhi::Device& dev, const rhi::Queue& queue, bool bgra, double now,
                 std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, std::string& note);

  uint32_t TileW() const { return tw_; }
  uint32_t TileH() const { return th_; }

 private:
  uint32_t fw_ = 0, fh_ = 0, tw_ = 0, th_ = 0;
  rhi::TextureFormat fmt_ = rhi::TextureFormat::Undefined;
  rhi::Texture tex_[kTiles];
  rhi::TextureView view_[kTiles];
  double t_[kTiles] = {};
  bool held_[kTiles] = {};
  int head_ = 0;
  double last_ = -1e9;
};

// Lay `n` tiles (each `tw` x `th` RGBA8, top row first, given oldest first)
// out as a kCols-wide grid with a 2 px dark gutter. Pure; the `bugreport-md`
// gate checks it.
void ComposeContactSheet(const std::vector<const uint8_t*>& tiles, uint32_t tw, uint32_t th,
                         int cols, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);
