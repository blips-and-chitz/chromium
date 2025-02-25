// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_CHILD_REQUEST_EXTRA_DATA_H_
#define CONTENT_CHILD_REQUEST_EXTRA_DATA_H_

#include <utility>

#include "base/compiler_specific.h"
#include "base/macros.h"
#include "content/common/content_export.h"
#include "content/common/navigation_params.h"
#include "content/public/common/url_loader_throttle.h"
#include "content/renderer/loader/frame_request_blocker.h"
#include "content/renderer/loader/navigation_response_override_parameters.h"
#include "content/renderer/loader/web_url_loader_impl.h"
#include "third_party/blink/public/platform/web_string.h"
#include "third_party/blink/public/platform/web_url_request.h"

namespace network {
struct ResourceRequest;
}

namespace content {

// Can be used by callers to store extra data on every ResourceRequest
// which will be incorporated into the ResourceHostMsg_RequestResource message
// sent by ResourceDispatcher.
class CONTENT_EXPORT RequestExtraData : public blink::WebURLRequest::ExtraData {
 public:
  RequestExtraData();
  ~RequestExtraData() override;

  // |custom_user_agent| is used to communicate an overriding custom user agent
  // to |RenderViewImpl::willSendRequest()|; set to a null string to indicate no
  // override and an empty string to indicate that there should be no user
  // agent.
  const blink::WebString& custom_user_agent() const {
    return custom_user_agent_;
  }
  void set_custom_user_agent(const blink::WebString& custom_user_agent) {
    custom_user_agent_ = custom_user_agent;
  }

  // PlzNavigate: |navigation_response_override| is used to override certain
  // parameters of navigation requests.
  std::unique_ptr<NavigationResponseOverrideParameters>
  TakeNavigationResponseOverrideOwnership() {
    return std::move(navigation_response_override_);
  }

  void set_navigation_response_override(
      std::unique_ptr<NavigationResponseOverrideParameters> response_override) {
    navigation_response_override_ = std::move(response_override);
  }

  // The request is for a prefetch-only client (i.e. running NoStatePrefetch)
  // and should use LOAD_PREFETCH network flags.
  bool is_for_no_state_prefetch() const { return is_for_no_state_prefetch_; }
  void set_is_for_no_state_prefetch(bool prefetch) {
    is_for_no_state_prefetch_ = prefetch;
  }

  // Copy of the settings value determining if mixed plugin content should be
  // blocked.
  bool block_mixed_plugin_content() const {
    return block_mixed_plugin_content_;
  }
  void set_block_mixed_plugin_content(bool block_mixed_plugin_content) {
    block_mixed_plugin_content_ = block_mixed_plugin_content;
  }
  std::vector<std::unique_ptr<URLLoaderThrottle>> TakeURLLoaderThrottles() {
    return std::move(url_loader_throttles_);
  }
  void set_url_loader_throttles(
      std::vector<std::unique_ptr<URLLoaderThrottle>> throttles) {
    url_loader_throttles_ = std::move(throttles);
  }
  void set_frame_request_blocker(
      scoped_refptr<FrameRequestBlocker> frame_request_blocker) {
    frame_request_blocker_ = frame_request_blocker;
  }
  scoped_refptr<FrameRequestBlocker> frame_request_blocker() {
    return frame_request_blocker_;
  }

  void CopyToResourceRequest(network::ResourceRequest* request) const;

 private:
  blink::WebString custom_user_agent_;
  std::unique_ptr<NavigationResponseOverrideParameters>
      navigation_response_override_;
  bool is_for_no_state_prefetch_ = false;
  bool block_mixed_plugin_content_ = false;
  std::vector<std::unique_ptr<URLLoaderThrottle>> url_loader_throttles_;
  scoped_refptr<FrameRequestBlocker> frame_request_blocker_;

  DISALLOW_COPY_AND_ASSIGN(RequestExtraData);
};

}  // namespace content

#endif  // CONTENT_CHILD_REQUEST_EXTRA_DATA_H_
