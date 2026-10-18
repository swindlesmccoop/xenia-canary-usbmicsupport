/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_SDL_SDL_MIC_SOURCE_H_
#define XENIA_APU_SDL_SDL_MIC_SOURCE_H_

#include <deque>
#include <mutex>
#include <string>

#include "xenia/apu/mic_source.h"

namespace xe {
namespace apu {
namespace sdl {

// Live host capture through SDL (PulseAudio/PipeWire/ALSA on Linux).
class SDLMicSource : public MicSource {
 public:
  explicit SDLMicSource(std::string device_name);
  ~SDLMicSource() override;

  // Makes SDL capture the host backend for the emulated USB microphone.
  static void Register();

  bool Start(uint32_t sample_rate) override;
  void Stop() override;
  void Read(int16_t* out, size_t count) override;

 private:
  static void SDLCallback(void* userdata, uint8_t* stream, int len);

  std::string device_name_;
  uint32_t device_ = 0;
  bool sdl_initialized_ = false;
  size_t max_buffered_ = 0;
  size_t underrun_samples_ = 0;
  size_t read_samples_ = 0;

  std::mutex lock_;
  std::deque<int16_t> buffer_;
};

}  // namespace sdl
}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_SDL_SDL_MIC_SOURCE_H_
