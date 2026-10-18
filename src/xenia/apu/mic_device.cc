/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/mic_device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"

DEFINE_bool(mic_apply_gain, false,
            "Apply the gain the game sets on the emulated USB microphone. Off "
            "by default: games set gain for a real dynamic mic, which clips a "
            "line-level host source.",
            "APU");
DEFINE_double(mic_input_gain_db, 0.0,
              "Extra gain in dB applied to the emulated USB microphone input.",
              "APU");
DEFINE_path(mic_dump_file, "",
            "Write the exact audio delivered to the guest by the emulated USB "
            "microphone to this file, replacing it on each launch (raw signed "
            "16-bit mono at the guest's sample rate), for diagnosing what the "
            "game hears.",
            "APU");
DEFINE_bool(mic_log_levels, false,
            "Log the level delivered to the guest by the emulated USB "
            "microphone once per second.",
            "APU");

namespace xe {
namespace apu {

namespace {

// Request types, from the XMic library's use of MicDeviceRequest.
enum : uint16_t {
  kRequestGetStatus = 0,
  kRequestStart = 1,
  kRequestControl = 2,  // USB control transfer, setup packet at +0x1C.
  kRequestSetFormat = 4,
  kRequestStop = 5,
  kRequestGetData = 7,
  kRequestGetSampleRates = 8,
};

enum : uint32_t {
  kStateConnected = 1,
  kStateStreaming = 5,
};

// Request block layout (big-endian, offsets in bytes):
//   0x00 u16 type          0x02 u16 port
//   0x04 u32 state (out)   0x0C u32 caller context (opaque to the kernel)
//   0x10 u32 completion    0x14 u32 buffer, sample rate, or result
//   0x18 u32 packet count / packet bytes; u16 transfer length for control
//   0x1C u8[8] USB setup packet for control requests
constexpr uint32_t kOffsetType = 0x00;
constexpr uint32_t kOffsetPort = 0x02;
constexpr uint32_t kOffsetState = 0x04;
constexpr uint32_t kOffsetCompletion = 0x10;
constexpr uint32_t kOffsetBuffer = 0x14;
constexpr uint32_t kOffsetCount = 0x18;
constexpr uint32_t kOffsetSetup = 0x1C;

constexpr uint32_t kMaxPackets = 64;
// Sample-rate capability bits, as the XMic library maps them in XMicStart.
constexpr uint32_t kRate32k = 0x40;
constexpr uint32_t kRate48k = 0x100;

// USB audio class volume range, 1/256 dB. Matches the range the XMic library
// hardcodes for the other (Rmc) mic type.
constexpr int16_t kVolumeMin = -0x0C00;
constexpr int16_t kVolumeMax = 0x2340;
constexpr int16_t kVolumeRes = 0x0100;

uint64_t NowMicroseconds() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

MicDevice::MicDevice(Memory* memory, cpu::Processor* processor)
    : memory_(memory), processor_(processor) {}

MicDevice::~MicDevice() { Shutdown(); }

X_STATUS MicDevice::Setup(kernel::KernelState* kernel_state) {
  packet_info_ptr_ = memory_->SystemHeapAlloc(kMaxPackets * 6);
  if (!packet_info_ptr_) {
    return X_STATUS_UNSUCCESSFUL;
  }
  running_ = true;
  worker_thread_ =
      kernel::object_ref<kernel::XHostThread>(new kernel::XHostThread(
          kernel_state, 128 * 1024, 0,
          [this]() {
            WorkerThreadMain();
            return 0;
          },
          kernel_state->GetSystemProcess()));
  worker_thread_->set_can_debugger_suspend(true);
  worker_thread_->set_name("Mic Worker");
  worker_thread_->Create();
  return X_STATUS_SUCCESS;
}

void MicDevice::Shutdown() {
  if (!running_) {
    return;
  }
  {
    std::lock_guard<std::mutex> guard(lock_);
    running_ = false;
  }
  work_cv_.notify_all();
  if (worker_thread_) {
    worker_thread_->Wait(0, 0, 0, nullptr);
    worker_thread_.reset();
  }
  if (source_) {
    source_->Stop();
    source_.reset();
  }
  if (packet_info_ptr_) {
    memory_->SystemHeapFree(packet_info_ptr_);
    packet_info_ptr_ = 0;
  }
}

X_STATUS MicDevice::Request(uint32_t request_ptr) {
  uint8_t* request = memory_->TranslateVirtual<uint8_t*>(request_ptr);
  uint16_t type = xe::load_and_swap<uint16_t>(request + kOffsetType);
  uint16_t port = xe::load_and_swap<uint16_t>(request + kOffsetPort);

  if (port != 0) {
    if (type == kRequestGetStatus) {
      xe::store_and_swap<uint32_t>(request + kOffsetState, 0);
      return X_STATUS_SUCCESS;
    }
    return X_STATUS_DEVICE_NOT_CONNECTED;
  }

  std::lock_guard<std::mutex> guard(lock_);
  switch (type) {
    case kRequestGetStatus:
      xe::store_and_swap<uint32_t>(
          request + kOffsetState,
          streaming_ ? kStateStreaming : kStateConnected);
      return X_STATUS_SUCCESS;
    case kRequestGetData: {
      uint32_t count = xe::load_and_swap<uint32_t>(request + kOffsetCount);
      if (!count || count > kMaxPackets) {
        return X_STATUS_INVALID_PARAMETER;
      }
      if (!streaming_) {
        return X_STATUS_INVALID_DEVICE_STATE;
      }
      data_queue_.push_back(request_ptr);
      break;
    }
    case kRequestStart:
    case kRequestControl:
    case kRequestSetFormat:
    case kRequestStop:
    case kRequestGetSampleRates:
      control_queue_.push_back(request_ptr);
      break;
    default:
      XELOGW("Mic: unhandled request type {:04X}", type);
      return X_STATUS_INVALID_DEVICE_REQUEST;
  }
  work_cv_.notify_all();
  return X_STATUS_PENDING;
}

void MicDevice::WorkerThreadMain() {
  std::unique_lock<std::mutex> lock(lock_);
  while (running_) {
    if (!control_queue_.empty()) {
      uint32_t request_ptr = control_queue_.front();
      control_queue_.pop_front();
      lock.unlock();
      X_STATUS status = HandleControl(request_ptr);
      Complete(request_ptr, status);
      lock.lock();
      continue;
    }

    if (!data_queue_.empty()) {
      uint32_t request_ptr = data_queue_.front();
      if (!streaming_) {
        data_queue_.pop_front();
        lock.unlock();
        CompleteData(request_ptr, X_STATUS_CANCELLED);
        lock.lock();
        continue;
      }

      // Isochronous pacing: a request of N packets covers the next N ms of
      // the stream and completes once that time has passed. After an idle
      // gap (nobody reading), jump the stream forward to now so latency
      // doesn't build up.
      uint64_t now = NowMicroseconds();
      if (!stream_us_ || now > stream_us_ + 50000) {
        stream_us_ = now;
      }
      uint32_t count = xe::load_and_swap<uint32_t>(
          memory_->TranslateVirtual<uint8_t*>(request_ptr) + kOffsetCount);
      uint64_t deadline = stream_us_ + static_cast<uint64_t>(count) * 1000;
      if (now < deadline) {
        work_cv_.wait_for(lock, std::chrono::microseconds(deadline - now));
        continue;
      }
      data_queue_.pop_front();
      stream_us_ = deadline;
      lock.unlock();
      CompleteData(request_ptr, X_STATUS_SUCCESS);
      lock.lock();
      continue;
    }

    work_cv_.wait(lock);
  }
}

X_STATUS MicDevice::HandleControl(uint32_t request_ptr) {
  uint8_t* request = memory_->TranslateVirtual<uint8_t*>(request_ptr);
  uint16_t type = xe::load_and_swap<uint16_t>(request + kOffsetType);
  uint32_t buffer = xe::load_and_swap<uint32_t>(request + kOffsetBuffer);

  switch (type) {
    case kRequestStart: {
      std::lock_guard<std::mutex> guard(lock_);
      streaming_ = true;
      stream_us_ = 0;
      XELOGI("Mic: start");
      return X_STATUS_SUCCESS;
    }
    case kRequestSetFormat: {
      uint32_t rate = buffer;
      uint32_t packet_bytes =
          xe::load_and_swap<uint32_t>(request + kOffsetCount);
      if (rate < 8000 || rate > 48000 || !packet_bytes || packet_bytes % 2 ||
          packet_bytes > 0x400) {
        XELOGE("Mic: rejecting format {} Hz, {}-byte packets", rate,
               packet_bytes);
        return X_STATUS_INVALID_PARAMETER;
      }
      sample_rate_ = rate;
      packet_bytes_ = packet_bytes;
      if (source_) {
        source_->Stop();
      }
      source_ = CreateMicSource();
      source_started_ = source_ && source_->Start(sample_rate_);
      XELOGI("Mic: {} Hz, {}-byte packets, source {}", sample_rate_,
             packet_bytes_, source_started_ ? "open" : "silent");
      return X_STATUS_SUCCESS;
    }
    case kRequestStop: {
      std::deque<uint32_t> cancelled;
      {
        std::lock_guard<std::mutex> guard(lock_);
        streaming_ = false;
        cancelled.swap(data_queue_);
      }
      for (uint32_t data_request : cancelled) {
        CompleteData(data_request, X_STATUS_CANCELLED);
      }
      if (source_) {
        source_->Stop();
        source_.reset();
      }
      source_started_ = false;
      XELOGI("Mic: stop");
      return X_STATUS_SUCCESS;
    }
    case kRequestGetSampleRates:
      xe::store_and_swap<uint32_t>(request + kOffsetBuffer,
                                   kRate32k | kRate48k);
      return X_STATUS_SUCCESS;
    case kRequestControl: {
      const uint8_t* setup = request + kOffsetSetup;
      uint8_t request_type = setup[0];
      uint8_t usb_request = setup[1];
      uint16_t value = xe::load_and_swap<uint16_t>(setup + 2);
      uint16_t length = xe::load_and_swap<uint16_t>(setup + 6);
      uint8_t* data =
          buffer ? memory_->TranslateVirtual<uint8_t*>(buffer) : nullptr;
      if (!data || !length) {
        return X_STATUS_SUCCESS;
      }
      bool device_to_host = request_type & 0x80;
      bool volume = (value >> 8) == 0x02 && length >= 2;
      if (device_to_host) {
        std::memset(data, 0, length);
        if (volume) {
          // USB control data is little-endian.
          int16_t v = HandleVolume(usb_request, 0);
          data[0] = static_cast<uint8_t>(v);
          data[1] = static_cast<uint8_t>(static_cast<uint16_t>(v) >> 8);
        }
      } else if (volume) {
        HandleVolume(usb_request, static_cast<int16_t>(data[0] | data[1] << 8));
      }
      return X_STATUS_SUCCESS;
    }
  }
  return X_STATUS_INVALID_DEVICE_REQUEST;
}

int16_t MicDevice::HandleVolume(uint8_t request, int16_t value) {
  switch (request) {
    case 0x01:  // SET_CUR.
      volume_ = std::clamp(value, kVolumeMin, kVolumeMax);
      XELOGI("Mic: game set gain {:.2f} dB{}", volume_ / 256.0,
             cvars::mic_apply_gain ? "" : " (not applied, mic_apply_gain)");
      return volume_;
    case 0x81:  // GET_CUR.
      return volume_;
    case 0x82:  // GET_MIN.
      return kVolumeMin;
    case 0x83:  // GET_MAX.
      return kVolumeMax;
    case 0x84:  // GET_RES.
      return kVolumeRes;
  }
  return 0;
}

void MicDevice::CompleteData(uint32_t request_ptr, X_STATUS status) {
  uint8_t* request = memory_->TranslateVirtual<uint8_t*>(request_ptr);
  uint32_t buffer = xe::load_and_swap<uint32_t>(request + kOffsetBuffer);
  uint32_t count = std::min(xe::load_and_swap<uint32_t>(request + kOffsetCount),
                            kMaxPackets);
  uint32_t samples_per_packet = packet_bytes_ / 2;
  size_t total = static_cast<size_t>(count) * samples_per_packet;

  if (XSUCCEEDED(status) && buffer) {
    std::vector<int16_t> samples(total, 0);
    if (source_started_) {
      source_->Read(samples.data(), total);
    }
    double gain_db = cvars::mic_input_gain_db;
    if (cvars::mic_apply_gain) {
      gain_db += volume_ / 256.0;
    }
    float gain = static_cast<float>(std::pow(10.0, gain_db / 20.0));
    uint8_t* out = memory_->TranslateVirtual<uint8_t*>(buffer);
    for (size_t i = 0; i < total; ++i) {
      float v = samples[i] * gain;
      int16_t s = static_cast<int16_t>(std::clamp(v, -32768.0f, 32767.0f));
      // USB audio is little-endian; the XMic library byte-swaps it.
      out[i * 2] = static_cast<uint8_t>(s);
      out[i * 2 + 1] = static_cast<uint8_t>(static_cast<uint16_t>(s) >> 8);
      level_sum_ += static_cast<double>(s) * s;
    }
    level_samples_ += total;
    if (!cvars::mic_dump_file.empty()) {
      static std::ofstream dump(cvars::mic_dump_file,
                                std::ios::binary | std::ios::trunc);
      dump.write(reinterpret_cast<const char*>(out), total * 2);
    }
    if (cvars::mic_log_levels && level_samples_ >= sample_rate_) {
      double rms = std::sqrt(level_sum_ / level_samples_) / 32768.0;
      XELOGI("Mic: delivered {} samples, RMS {:.1f} dBFS", level_samples_,
             rms > 0 ? 20.0 * std::log10(rms) : -120.0);
      level_sum_ = 0.0;
      level_samples_ = 0;
    }
  }

  uint8_t* info = memory_->TranslateVirtual<uint8_t*>(packet_info_ptr_);
  uint32_t lengths_ptr = packet_info_ptr_ + kMaxPackets * 4;
  for (uint32_t i = 0; i < count; ++i) {
    xe::store_and_swap<uint32_t>(info + i * 4, status);
    xe::store_and_swap<uint16_t>(info + kMaxPackets * 4 + i * 2,
                                 XSUCCEEDED(status) ? packet_bytes_ : 0);
  }

  uint32_t completion =
      xe::load_and_swap<uint32_t>(request + kOffsetCompletion);
  if (completion) {
    uint64_t args[] = {request_ptr, status, packet_info_ptr_, lengths_ptr};
    processor_->Execute(worker_thread_->thread_state(), completion, args,
                        xe::countof(args));
  }
}

void MicDevice::Complete(uint32_t request_ptr, X_STATUS status) {
  uint32_t completion = xe::load_and_swap<uint32_t>(
      memory_->TranslateVirtual<uint8_t*>(request_ptr) + kOffsetCompletion);
  if (completion) {
    uint64_t args[] = {request_ptr, status};
    processor_->Execute(worker_thread_->thread_state(), completion, args,
                        xe::countof(args));
  }
}

}  // namespace apu
}  // namespace xe
