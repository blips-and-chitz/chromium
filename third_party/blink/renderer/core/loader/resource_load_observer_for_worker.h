// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_RESOURCE_LOAD_OBSERVER_FOR_WORKER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_RESOURCE_LOAD_OBSERVER_FOR_WORKER_H_

#include <inttypes.h>

#include "base/containers/span.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_load_observer.h"

namespace blink {

class CoreProbeSink;
class ResourceFetcherProperties;
class WebWorkerFetchContext;

// ResourceLoadObserver implementation associated with a worker or worklet.
class ResourceLoadObserverForWorker final : public ResourceLoadObserver {
 public:
  ResourceLoadObserverForWorker(CoreProbeSink& probe,
                                const ResourceFetcherProperties& properties,
                                scoped_refptr<WebWorkerFetchContext>);
  ~ResourceLoadObserverForWorker() override;

  // ResourceLoadObserver implementation.
  void WillSendRequest(uint64_t identifier,
                       const ResourceRequest&,
                       const ResourceResponse& redirect_response,
                       ResourceType,
                       const FetchInitiatorInfo&) override;
  void DidReceiveResponse(uint64_t identifier,
                          const ResourceRequest& request,
                          const ResourceResponse& response,
                          Resource* resource,
                          ResponseSource) override;
  void DidReceiveData(uint64_t identifier,
                      base::span<const char> chunk) override;
  void DidReceiveTransferSizeUpdate(uint64_t identifier,
                                    int transfer_size_diff) override;
  void DidDownloadToBlob(uint64_t identifier, BlobDataHandle*) override;
  void DidFinishLoading(uint64_t identifier,
                        TimeTicks finish_time,
                        int64_t encoded_data_length,
                        int64_t decoded_body_length,
                        bool should_report_corb_blocking,
                        ResponseSource) override;
  void DidFailLoading(const KURL&,
                      uint64_t identifier,
                      const ResourceError&,
                      int64_t encoded_data_length,
                      bool is_internal_request) override;
  void Trace(Visitor*) override;

 private:
  const Member<CoreProbeSink> probe_;
  const Member<const ResourceFetcherProperties> fetcher_properties_;
  const scoped_refptr<WebWorkerFetchContext> web_context_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_RESOURCE_LOAD_OBSERVER_FOR_WORKER_H_
