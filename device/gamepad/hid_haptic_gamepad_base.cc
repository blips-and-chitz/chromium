// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/gamepad/hid_haptic_gamepad_base.h"

namespace device {

namespace {

const size_t kBitsPerByte = 8;

void MagnitudeToBytes(double magnitude,
                      size_t report_size_bits,
                      uint32_t logical_min,
                      uint32_t logical_max,
                      std::vector<uint8_t>* bytes) {
  DCHECK(bytes);
  DCHECK_EQ(report_size_bits % kBitsPerByte, 0U);
  bytes->clear();
  if (logical_min >= logical_max)
    return;
  // If the vibration actuator on the device is only on or off, ensure it will
  // be on for any non-zero vibration magnitude.
  if (logical_min == 0 && logical_max == 1)
    magnitude = (magnitude > 0.0) ? 1.0 : 0.0;
  uint32_t scaled_magnitude = static_cast<uint32_t>(
      magnitude * (logical_max - logical_min) + logical_min);
  size_t remaining_bits = report_size_bits;
  // Fields larger than one byte are stored in little-endian byte order.
  while (remaining_bits > 0) {
    bytes->push_back(scaled_magnitude & 0xff);
    remaining_bits -= kBitsPerByte;
    scaled_magnitude >>= kBitsPerByte;
  }
}
}  // namespace

// Supported HID gamepads.
HidHapticGamepadBase::HapticReportData kHapticReportData[] = {
    // XSkills Gamecube USB adapter
    {0x0b43, 0x0005, 0x00, 4, 3, 3, 1 * kBitsPerByte, 0, 1},
    // Analog game controller
    {0x6666, 0x9401, 0x05, 5, 1, 3, 2 * kBitsPerByte, 0, 0xffff},
    // Stadia controller
    {0x18d1, 0x9400, 0x05, 5, 1, 3, 2 * kBitsPerByte, 0, 0xffff},
};
size_t kHapticReportDataLength = base::size(kHapticReportData);

HidHapticGamepadBase::HidHapticGamepadBase(const HapticReportData& data)
    : report_id_(data.report_id),
      report_length_bytes_(data.report_length_bytes),
      strong_offset_bytes_(data.strong_offset_bytes),
      weak_offset_bytes_(data.weak_offset_bytes),
      report_size_bits_(data.report_size_bits),
      logical_min_(data.logical_min),
      logical_max_(data.logical_max) {}

HidHapticGamepadBase::~HidHapticGamepadBase() = default;

// static
bool HidHapticGamepadBase::IsHidHaptic(uint16_t vendor_id,
                                       uint16_t product_id) {
  const auto* begin = kHapticReportData;
  const auto* end = kHapticReportData + kHapticReportDataLength;
  const auto* find_it =
      std::find_if(begin, end, [=](const HapticReportData& d) {
        return d.vendor_id == vendor_id && d.product_id == product_id;
      });
  return find_it != end;
}

// static
const HidHapticGamepadBase::HapticReportData*
HidHapticGamepadBase::GetHapticReportData(uint16_t vendor_id,
                                          uint16_t product_id) {
  const auto* begin = kHapticReportData;
  const auto* end = kHapticReportData + kHapticReportDataLength;
  const auto* find_it =
      std::find_if(begin, end, [=](const HapticReportData& d) {
        return d.vendor_id == vendor_id && d.product_id == product_id;
      });
  return find_it == end ? nullptr : &*find_it;
}

void HidHapticGamepadBase::SetVibration(double strong_magnitude,
                                        double weak_magnitude) {
  std::vector<uint8_t> control_report(report_length_bytes_);
  control_report[0] = report_id_;
  if (strong_offset_bytes_ == weak_offset_bytes_) {
    // Single channel vibration. Combine both channels into a single magnitude.
    std::vector<uint8_t> vibration_bytes;
    double vibration_magnitude =
        std::min(strong_magnitude + weak_magnitude, 1.0);
    MagnitudeToBytes(vibration_magnitude, report_size_bits_, logical_min_,
                     logical_max_, &vibration_bytes);
    // Vibration magnitude must not overwrite the report ID.
    DCHECK(report_id_ == 0x00 || strong_offset_bytes_ > 0);
    // Vibration magnitude must not overrun the report buffer.
    DCHECK_LE(strong_offset_bytes_ + vibration_bytes.size(),
              report_length_bytes_);
    std::copy(vibration_bytes.begin(), vibration_bytes.end(),
              control_report.begin() + strong_offset_bytes_);
  } else {
    // Dual channel vibration.
    std::vector<uint8_t> left_bytes;
    std::vector<uint8_t> right_bytes;
    MagnitudeToBytes(strong_magnitude, report_size_bits_, logical_min_,
                     logical_max_, &left_bytes);
    MagnitudeToBytes(weak_magnitude, report_size_bits_, logical_min_,
                     logical_max_, &right_bytes);
    // Vibration magnitude must not overwrite the report ID.
    DCHECK(report_id_ == 0x00 || strong_offset_bytes_ > 0);
    DCHECK(report_id_ == 0x00 || weak_offset_bytes_ > 0);
    // Vibration magnitude must not overrun the report buffer.
    DCHECK_LE(strong_offset_bytes_ + left_bytes.size(), report_length_bytes_);
    DCHECK_LE(weak_offset_bytes_ + right_bytes.size(), report_length_bytes_);
    // The strong and weak vibration magnitude fields must not overlap.
    DCHECK(strong_offset_bytes_ + left_bytes.size() <= weak_offset_bytes_ ||
           weak_offset_bytes_ + right_bytes.size() <= strong_offset_bytes_);
    std::copy(left_bytes.begin(), left_bytes.end(),
              control_report.begin() + strong_offset_bytes_);
    std::copy(right_bytes.begin(), right_bytes.end(),
              control_report.begin() + weak_offset_bytes_);
  }
  WriteOutputReport(control_report.data(), report_length_bytes_);
}

}  // namespace device
