#pragma once
// miniaudio playback device. Owns the data callback, which forwards to
// AudioWorld::Render.
//
// Init order matters: Init() opens the device so the REAL negotiated sample
// rate is known, the caller then builds the AudioWorld and the Library at that
// rate, and Start() begins callbacks. Decoding assets at the wrong rate and
// resampling later would be both slower and worse.
//
// A failed Init is not fatal anywhere in sandvox: the game runs silent. There
// is no audio hardware in CI, and --selftest must not depend on any.

namespace audio {

class AudioWorld;

class Device {
 public:
  Device();
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  // Opens a f32 stereo playback device at `preferredRate`. Callbacks do not
  // run until Start(). False on failure (no device, no driver, CI).
  bool Init(unsigned preferredRate);

  // The rate the device actually negotiated. Valid after a successful Init.
  double SampleRate() const;

  bool Start(AudioWorld* world);
  void Stop();
  bool Running() const;

  struct Impl;  // public so the miniaudio C callback can reach it

 private:
  Impl* impl_ = nullptr;
};

}  // namespace audio
