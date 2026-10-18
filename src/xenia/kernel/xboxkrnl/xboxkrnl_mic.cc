/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_system.h"
#include "xenia/apu/mic_device.h"
#include "xenia/emulator.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_private.h"
#include "xenia/xbox.h"

DEFINE_bool(allow_mic_initialization, false,
            "Connect an emulated USB microphone, fed from the host. See the "
            "mic_* options under APU.",
            "Kernel");

namespace xe {
namespace kernel {
namespace xboxkrnl {

// The USB microphone driver entry point the XDK XMic library calls. The
// request block layout and protocol are documented in apu/mic_device.h.
dword_result_t MicDeviceRequest_entry(lpvoid_t request_ptr) {
  if (!request_ptr) {
    return X_STATUS_INVALID_PARAMETER;
  }
  auto mic = kernel_state()->emulator()->audio_system()->mic_device();
  if (!cvars::allow_mic_initialization || !mic) {
    uint8_t* request = request_ptr.as<uint8_t*>();
    if (xe::load_and_swap<uint16_t>(request) == 0) {  // GetStatus.
      xe::store_and_swap<uint32_t>(request + 4, 0);   // Not connected.
      return X_STATUS_SUCCESS;
    }
    return X_STATUS_DEVICE_NOT_CONNECTED;
  }
  return mic->Request(request_ptr.guest_address());
}
DECLARE_XBOXKRNL_EXPORT1(MicDeviceRequest, kAudio, kImplemented);

}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe

DECLARE_XBOXKRNL_EMPTY_REGISTER_EXPORTS(Mic);
