/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_MIC_DEVICE_H_
#define XENIA_APU_MIC_DEVICE_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "xenia/apu/mic_source.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

// Emulates the USB audio-class microphone behind the kernel's
// MicDeviceRequest export (device type 0x6000 in XamUserGetDeviceContext),
// as used by the XDK XMic library.
//
// Every request except GetStatus is asynchronous: the kernel returns
// STATUS_PENDING and later calls the request's completion routine as
// completion(request, status) or, for GetData,
// completion(request, status, packet_status[], packet_length[]).
// A request that does not return STATUS_PENDING is final and its completion
// must not run. Audio is isochronous: one packet per millisecond of
// little-endian 16-bit mono PCM.
class MicDevice {
 public:
  MicDevice(Memory* memory, cpu::Processor* processor);
  ~MicDevice();

  X_STATUS Setup(kernel::KernelState* kernel_state);
  void Shutdown();

  X_STATUS Request(uint32_t request_ptr);

 private:
  void WorkerThreadMain();
  X_STATUS HandleControl(uint32_t request_ptr);
  void CompleteData(uint32_t request_ptr, X_STATUS status);
  void Complete(uint32_t request_ptr, X_STATUS status);
  int16_t HandleVolume(uint8_t request, int16_t value);

  Memory* memory_;
  cpu::Processor* processor_;

  std::atomic<bool> running_ = {false};
  kernel::object_ref<kernel::XHostThread> worker_thread_;

  std::mutex lock_;
  std::condition_variable work_cv_;
  std::deque<uint32_t> control_queue_;
  std::deque<uint32_t> data_queue_;

  bool streaming_ = false;
  uint32_t sample_rate_ = 48000;
  uint32_t packet_bytes_ = 96;
  int16_t volume_ = 0;  // 1/256 dB, USB audio class units.
  std::unique_ptr<MicSource> source_;
  bool source_started_ = false;

  // Wall-clock position of the isochronous stream.
  uint64_t stream_us_ = 0;

  // Guest scratch for the per-packet status/length arrays of GetData.
  uint32_t packet_info_ptr_ = 0;

  // Delivered-level telemetry for mic_log_levels.
  double level_sum_ = 0.0;
  uint64_t level_samples_ = 0;
};

}  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_MIC_DEVICE_H_
