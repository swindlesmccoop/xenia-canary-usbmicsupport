/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_MIC_SOURCE_H_
#define XENIA_APU_MIC_SOURCE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace xe {
namespace apu {

// Host-side audio feeding the emulated USB microphone. Delivers mono signed
// 16-bit samples at the rate the guest selected.
class MicSource {
 public:
  virtual ~MicSource() = default;

  virtual bool Start(uint32_t sample_rate) = 0;
  virtual void Stop() = 0;

  // Fills `out` with the `count` samples ending now. Anything not available
  // is written as silence.
  virtual void Read(int16_t* out, size_t count) = 0;
};

using MicSourceFactory =
    std::function<std::unique_ptr<MicSource>(const std::string& device_name)>;

// Lets a platform backend (SDL) provide live capture without the APU core
// linking against it.
void RegisterHostMicSourceFactory(MicSourceFactory factory);

// Picks the source the cvars ask for: a WAV file when mic_input_file is set,
// otherwise host capture if a backend registered one. Null means silence.
std::unique_ptr<MicSource> CreateMicSource();

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_MIC_SOURCE_H_
