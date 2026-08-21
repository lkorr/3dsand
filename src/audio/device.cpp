// The ONE translation unit that compiles the miniaudio implementation.
//
// NOMINMAX matters here specifically: miniaudio pulls in windows.h, whose
// min/max macros would break std::min/std::max in the xyzpan headers included
// downstream. (sandvox already defines it target-wide, but this file must not
// depend on that -- it is the file that would break first.)
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

// Decoding stays ON: audio::Library uses ma_decode_file for the .wav assets.
// Encoding is off -- nothing in sandvox writes audio files.
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#include <miniaudio.h>

#include "audio/device.h"

#include <cstdio>
#include <cstring>

#include "audio/world.h"

namespace audio {

struct Device::Impl {
  ma_device device{};
  AudioWorld* world = nullptr;
  bool initialized = false;
  bool started = false;
};

namespace {

void DataCallback(ma_device* device, void* output, const void* /*input*/,
                  ma_uint32 frameCount) {
  auto* impl = (Device::Impl*)device->pUserData;
  float* out = (float*)output;
  if (impl == nullptr || impl->world == nullptr) {
    std::memset(out, 0, sizeof(float) * 2 * frameCount);
    return;
  }
  impl->world->Render(out, (int)frameCount);
}

}  // namespace

Device::Device() : impl_(new Impl()) {}

Device::~Device() {
  Stop();
  if (impl_ != nullptr) {
    if (impl_->initialized) ma_device_uninit(&impl_->device);
    delete impl_;
    impl_ = nullptr;
  }
}

bool Device::Init(unsigned preferredRate) {
  if (impl_ == nullptr || impl_->initialized) return impl_ != nullptr && impl_->initialized;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 2;
  config.sampleRate = preferredRate;
  config.dataCallback = DataCallback;
  config.pUserData = impl_;
  if (ma_device_init(nullptr, &config, &impl_->device) != MA_SUCCESS) {
    std::printf("[audio] no playback device; running silent\n");
    return false;
  }
  impl_->initialized = true;
  return true;
}

double Device::SampleRate() const {
  if (impl_ == nullptr || !impl_->initialized) return 48000.0;
  return impl_->device.sampleRate != 0 ? (double)impl_->device.sampleRate : 48000.0;
}

bool Device::Start(AudioWorld* world) {
  if (impl_ == nullptr || !impl_->initialized) return false;
  impl_->world = world;
  if (ma_device_start(&impl_->device) != MA_SUCCESS) {
    std::printf("[audio] device failed to start; running silent\n");
    return false;
  }
  impl_->started = true;
  return true;
}

void Device::Stop() {
  if (impl_ == nullptr || !impl_->started) return;
  ma_device_stop(&impl_->device);
  impl_->started = false;
  // Drop the world pointer only after the device has stopped, so the callback
  // cannot be mid-Render against an object the caller is about to destroy.
  impl_->world = nullptr;
}

bool Device::Running() const { return impl_ != nullptr && impl_->started; }

}  // namespace audio
