// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_MEDIA_CONTROLS_SHARED_HELPER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_MEDIA_CONTROLS_SHARED_HELPER_H_

#include "base/optional.h"
#include "third_party/blink/renderer/platform/wtf/allocator.h"

namespace blink {

class HTMLMediaElement;

class MediaControlsSharedHelpers final {
  STATIC_ONLY(MediaControlsSharedHelpers);

 public:
  static base::Optional<unsigned> GetCurrentBufferedTimeRange(
      HTMLMediaElement& media_element);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_MEDIA_CONTROLS_MEDIA_CONTROLS_SHARED_HELPER_H_
