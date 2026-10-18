/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/sdl/sdl_mic_source.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "xenia/base/logging.h"
#include "xenia/helper/sdl/sdl_helper.h"

namespace xe {
namespace apu {
namespace sdl {

SDLMicSource::SDLMicSource(std::string device_name)
    : device_name_(std::move(device_name)) {}

SDLMicSource::~SDLMicSource() { Stop(); }

void SDLMicSource::Register() {
  RegisterHostMicSourceFactory([](const std::string& device_name) {
    return std::make_unique<SDLMicSource>(device_name);
  });
}

bool SDLMicSource::Start(uint32_t sample_rate) {
  if (!xe::helper::sdl::SDLHelper::Prepare() ||
      SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
    XELOGE("Mic: SDL audio init failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired = {};
  SDL_AudioSpec obtained = {};
  desired.freq = static_cast<int>(sample_rate);
  desired.format = AUDIO_S16SYS;
  desired.channels = 1;
  desired.samples = 256;
  desired.callback = SDLCallback;
  desired.userdata = this;
  // No allowed changes: SDL converts whatever the device gives us.
  device_ =
      SDL_OpenAudioDevice(device_name_.empty() ? nullptr : device_name_.c_str(),
                          1, &desired, &obtained, 0);
  if (!device_) {
    XELOGE("Mic: cannot open capture device '{}': {}", device_name_,
           SDL_GetError());
    int count = SDL_GetNumAudioDevices(1);
    for (int i = 0; i < count; ++i) {
      XELOGI("Mic: capture device available: '{}'",
             SDL_GetAudioDeviceName(i, 1));
    }
    return false;
  }
  // Bound latency at 100 ms; older audio is dropped.
  max_buffered_ = sample_rate / 10;
  SDL_PauseAudioDevice(device_, 0);
  XELOGI("Mic: capturing from '{}' at {} Hz",
         device_name_.empty() ? "default" : device_name_, sample_rate);
  return true;
}

void SDLMicSource::Stop() {
  if (device_) {
    SDL_CloseAudioDevice(device_);
    device_ = 0;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::lock_guard<std::mutex> guard(lock_);
  buffer_.clear();
}

void SDLMicSource::Read(int16_t* out, size_t count) {
  std::lock_guard<std::mutex> guard(lock_);
  size_t available = std::min(count, buffer_.size());
  std::copy_n(buffer_.begin(), available, out);
  buffer_.erase(buffer_.begin(), buffer_.begin() + available);
  std::memset(out + available, 0, (count - available) * sizeof(int16_t));
  underrun_samples_ += count - available;
  read_samples_ += count;
  if (read_samples_ >= 48000) {
    if (underrun_samples_) {
      XELOGW("Mic: capture underrun, {} of {} samples zero-filled",
             underrun_samples_, read_samples_);
    }
    underrun_samples_ = 0;
    read_samples_ = 0;
  }
}

void SDLMicSource::SDLCallback(void* userdata, uint8_t* stream, int len) {
  auto source = static_cast<SDLMicSource*>(userdata);
  auto samples = reinterpret_cast<const int16_t*>(stream);
  size_t count = static_cast<size_t>(len) / sizeof(int16_t);
  std::lock_guard<std::mutex> guard(source->lock_);
  source->buffer_.insert(source->buffer_.end(), samples, samples + count);
  if (source->buffer_.size() > source->max_buffered_) {
    source->buffer_.erase(source->buffer_.begin(),
                          source->buffer_.begin() +
                              (source->buffer_.size() - source->max_buffered_));
  }
}

}  // namespace sdl
}  // namespace apu
}  // namespace xe
