// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_FIDO_DEVICE_H_
#define DEVICE_FIDO_FIDO_DEVICE_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/callback.h"
#include "base/component_export.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/strings/string16.h"
#include "device/fido/authenticator_get_info_response.h"
#include "device/fido/fido_constants.h"
#include "device/fido/fido_transport_protocol.h"

namespace device {

// Device abstraction for an individual CTAP1.0/CTAP2.0 device.
//
// Devices are instantiated with an unknown protocol version. Users should call
// |DiscoverSupportedProtocolAndDeviceInfo| to determine a device's
// capabilities and initialize the instance accordingly. Instances returned by
// |FidoDeviceDiscovery| are not fully initialized.
class COMPONENT_EXPORT(DEVICE_FIDO) FidoDevice {
 public:
  using DeviceCallback =
      base::OnceCallback<void(base::Optional<std::vector<uint8_t>>)>;

  // Internal state machine states.
  enum class State {
    kInit,
    kBusy,
    kReady,
    // kMsgError occurs when the the device responds with an error indicating an
    // invalid command, parameter, or length. This is used within |FidoDevice|
    // to handle the case of a device rejecting a CTAP2 GetInfo command. It is
    // otherwise a fatal, terminal state.
    kMsgError,
    // kDeviceError indicates some error other than those covered by
    // |kMsgError|. This is a terminal state.
    kDeviceError,
  };

  FidoDevice();
  virtual ~FidoDevice();
  // Pure virtual function defined by each device type, implementing
  // the device communication transaction. The function must not immediately
  // call (i.e. hairpin) |callback|.
  virtual void DeviceTransact(std::vector<uint8_t> command,
                              DeviceCallback callback) = 0;
  virtual void Cancel() = 0;
  virtual std::string GetId() const = 0;
  virtual base::string16 GetDisplayName() const;
  virtual FidoTransportProtocol DeviceTransport() const = 0;
  virtual bool IsInPairingMode() const;
  virtual bool IsPaired() const;
  virtual base::WeakPtr<FidoDevice> GetWeakPtr() = 0;

  // Sends a speculative AuthenticatorGetInfo request to determine whether the
  // device supports the CTAP2 protocol, and initializes supported_protocol_
  // and device_info_ according to the result.
  virtual void DiscoverSupportedProtocolAndDeviceInfo(base::OnceClosure done);

  // Returns whether supported_protocol has been correctly initialized (usually
  // by calling DiscoverSupportedProtocolAndDeviceInfo).
  bool SupportedProtocolIsInitialized();
  // TODO(martinkr): Rename to "SetSupportedProtocolForTesting".
  void set_supported_protocol(ProtocolVersion supported_protocol) {
    supported_protocol_ = supported_protocol;
  }

  ProtocolVersion supported_protocol() const { return supported_protocol_; }
  const base::Optional<AuthenticatorGetInfoResponse>& device_info() const {
    return device_info_;
  }
  bool is_in_error_state() const {
    return state_ == State::kMsgError || state_ == State::kDeviceError;
  }

  State state_for_testing() const { return state_; }

 protected:
  void OnDeviceInfoReceived(base::OnceClosure done,
                            base::Optional<std::vector<uint8_t>> response);
  void SetDeviceInfo(AuthenticatorGetInfoResponse device_info);

  State state_ = State::kInit;
  ProtocolVersion supported_protocol_ = ProtocolVersion::kUnknown;
  base::Optional<AuthenticatorGetInfoResponse> device_info_;

  DISALLOW_COPY_AND_ASSIGN(FidoDevice);
};

}  // namespace device

#endif  // DEVICE_FIDO_FIDO_DEVICE_H_
