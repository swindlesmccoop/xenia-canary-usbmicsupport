/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/mic_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_string(mic_device, "",
              "Host capture device for the emulated USB microphone. Empty "
              "uses the system default.",
              "APU");
DEFINE_path(mic_input_file, "",
            "WAV file to feed the emulated USB microphone instead of a host "
            "capture device. Plays once on a wall-clock timeline, then "
            "silence.",
            "APU");
DEFINE_path(mic_input_file_trigger, "",
            "If set, mic_input_file stays silent until this host path exists. "
            "If the file holds a steady-clock (CLOCK_MONOTONIC) time in "
            "microseconds, sample 0 of the WAV plays at that time.",
            "APU");
DEFINE_int32(mic_input_file_offset_ms, 0,
             "Positive adds leading silence before mic_input_file, negative "
             "starts that far into it.",
             "APU");

namespace xe {
namespace apu {

namespace {

MicSourceFactory& host_factory() {
  static MicSourceFactory factory;
  return factory;
}

// Decodes a RIFF WAV (PCM 16/24/32-bit or float32, any channel count) to
// mono float.
bool LoadWav(const std::filesystem::path& path, std::vector<float>& out,
             uint32_t& out_rate) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  auto u16 = [&](size_t o) {
    return static_cast<uint16_t>(data[o] | data[o + 1] << 8);
  };
  auto u32 = [&](size_t o) {
    return static_cast<uint32_t>(u16(o) | u16(o + 2) << 16);
  };
  if (data.size() < 12 || std::memcmp(data.data(), "RIFF", 4) ||
      std::memcmp(data.data() + 8, "WAVE", 4)) {
    return false;
  }

  uint16_t format = 0, channels = 0, bits = 0;
  uint32_t rate = 0;
  size_t pcm_offset = 0, pcm_size = 0;
  for (size_t o = 12; o + 8 <= data.size();) {
    uint32_t size = u32(o + 4);
    if (!std::memcmp(data.data() + o, "fmt ", 4) && size >= 16) {
      format = u16(o + 8);
      channels = u16(o + 10);
      rate = u32(o + 12);
      bits = u16(o + 22);
      if (format == 0xFFFE && size >= 26) {
        format = u16(o + 32);  // WAVE_FORMAT_EXTENSIBLE sub-format.
      }
    } else if (!std::memcmp(data.data() + o, "data", 4)) {
      pcm_offset = o + 8;
      pcm_size = std::min<size_t>(size, data.size() - pcm_offset);
    }
    o += 8 + size + (size & 1);
  }
  bool supported = (format == 1 && (bits == 16 || bits == 24 || bits == 32)) ||
                   (format == 3 && bits == 32);
  if (!pcm_offset || !channels || !rate || !supported) {
    return false;
  }

  size_t bytes = bits / 8;
  size_t frames = pcm_size / (bytes * channels);
  out.resize(frames);
  for (size_t f = 0; f < frames; ++f) {
    float sum = 0.0f;
    for (size_t c = 0; c < channels; ++c) {
      size_t o = pcm_offset + (f * channels + c) * bytes;
      float v;
      if (format == 3) {
        uint32_t raw = u32(o);
        std::memcpy(&v, &raw, 4);
      } else if (bits == 16) {
        v = static_cast<int16_t>(u16(o)) / 32768.0f;
      } else if (bits == 24) {
        int32_t s = static_cast<int32_t>(data[o] << 8 | data[o + 1] << 16 |
                                         data[o + 2] << 24);
        v = (s >> 8) / 8388608.0f;
      } else {
        v = static_cast<int32_t>(u32(o)) / 2147483648.0f;
      }
      sum += v;
    }
    out[f] = sum / channels;
  }
  out_rate = rate;
  return true;
}

// Plays a WAV on a wall-clock timeline: sample 0 sounds at start_us_ on the
// steady clock, so the guest's polling pattern cannot drift it.
class WavFileMicSource : public MicSource {
 public:
  explicit WavFileMicSource(std::filesystem::path path)
      : path_(std::move(path)) {}

  bool Start(uint32_t sample_rate) override {
    std::vector<float> source;
    uint32_t source_rate;
    if (!LoadWav(path_, source, source_rate)) {
      XELOGE("Mic: cannot read {} (need RIFF WAV, PCM or float32)",
             path_.string());
      return false;
    }
    // Linear resample to the rate the guest picked.
    double step = static_cast<double>(source_rate) / sample_rate;
    size_t count = static_cast<size_t>(source.size() / step);
    samples_.resize(count);
    for (size_t i = 0; i < count; ++i) {
      double pos = i * step;
      size_t i0 = static_cast<size_t>(pos);
      size_t i1 = std::min(i0 + 1, source.size() - 1);
      float frac = static_cast<float>(pos - i0);
      float v = source[i0] + (source[i1] - source[i0]) * frac;
      samples_[i] =
          static_cast<int16_t>(std::clamp(v * 32767.0f, -32768.0f, 32767.0f));
    }
    rate_ = sample_rate;
    start_us_ = 0;
    XELOGI("Mic: playing {} ({} Hz -> {} Hz, {:.1f} s){}", path_.string(),
           source_rate, sample_rate, static_cast<double>(count) / sample_rate,
           cvars::mic_input_file_trigger.empty()
               ? ""
               : ", waiting for trigger file");
    return true;
  }

  void Stop() override { samples_.clear(); }

  void Read(int16_t* out, size_t count) override {
    uint64_t now = SteadyMicroseconds();
    PollTrigger(now);
    if (!start_us_) {
      std::memset(out, 0, count * sizeof(int16_t));
      return;
    }
    int64_t elapsed_us =
        static_cast<int64_t>(now - start_us_) -
        static_cast<int64_t>(cvars::mic_input_file_offset_ms) * 1000;
    int64_t position = elapsed_us * static_cast<int64_t>(rate_) / 1000000 -
                       static_cast<int64_t>(count);
    for (size_t i = 0; i < count; ++i, ++position) {
      out[i] =
          (position >= 0 && static_cast<size_t>(position) < samples_.size())
              ? samples_[static_cast<size_t>(position)]
              : 0;
    }
  }

 private:
  static uint64_t SteadyMicroseconds() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  // No trigger: start at the first read. With a trigger file: start when it
  // appears, at the steady-clock (CLOCK_MONOTONIC) microsecond it contains,
  // or immediately if it holds no number. The file is re-read whenever it
  // changes, so a script can re-align or sweep the timing mid-song.
  void PollTrigger(uint64_t now) {
    if (cvars::mic_input_file_trigger.empty()) {
      if (!start_us_) {
        start_us_ = now;
      }
      return;
    }
    if (++trigger_polls_ % 16) {
      return;
    }
    std::error_code ec;
    auto mtime =
        std::filesystem::last_write_time(cvars::mic_input_file_trigger, ec);
    if (ec || mtime == trigger_mtime_) {
      return;
    }
    trigger_mtime_ = mtime;
    std::ifstream trigger(cvars::mic_input_file_trigger);
    uint64_t at = 0;
    trigger >> at;
    start_us_ = at ? at : now;
    XELOGI("Mic: trigger file read, sample 0 at {} ({:+.3f} s from now)",
           start_us_,
           (static_cast<int64_t>(start_us_) - static_cast<int64_t>(now)) / 1e6);
  }

  std::filesystem::path path_;
  std::vector<int16_t> samples_;
  uint32_t rate_ = 0;
  uint64_t start_us_ = 0;
  uint32_t trigger_polls_ = 0;
  std::filesystem::file_time_type trigger_mtime_;
};

}  // namespace

void RegisterHostMicSourceFactory(MicSourceFactory factory) {
  host_factory() = std::move(factory);
}

std::unique_ptr<MicSource> CreateMicSource() {
  if (!cvars::mic_input_file.empty()) {
    return std::make_unique<WavFileMicSource>(cvars::mic_input_file);
  }
  if (host_factory()) {
    return host_factory()(cvars::mic_device);
  }
  XELOGW("Mic: no host capture backend, the microphone will be silent");
  return nullptr;
}

}  // namespace apu
}  // namespace xe
