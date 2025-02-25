// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/image_fetcher/core/cached_image_fetcher.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/metrics/histogram_macros.h"
#include "base/task/post_task.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "base/timer/elapsed_timer.h"
#include "components/image_fetcher/core/cache/cached_image_fetcher_metrics_reporter.h"
#include "components/image_fetcher/core/cache/image_cache.h"
#include "components/image_fetcher/core/image_decoder.h"
#include "components/image_fetcher/core/request_metadata.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image.h"
#include "ui/gfx/image/image_skia.h"

namespace image_fetcher {

struct CachedImageFetcherRequest {
  // The url to be fetched.
  const GURL url;

  const ImageFetcherParams params;

  // Analytic events below.

  // True if there was a cache hit during the fetch sequence.
  bool cache_hit_before_network_request;

  // The start time of the fetch sequence.
  const base::Time start_time;
};

namespace {

void DataCallbackIfPresent(ImageDataFetcherCallback data_callback,
                           const std::string& image_data,
                           const image_fetcher::RequestMetadata& metadata) {
  if (data_callback.is_null()) {
    return;
  }
  std::move(data_callback).Run(image_data, metadata);
}

void ImageCallbackIfPresent(ImageFetcherCallback image_callback,
                            const gfx::Image& image,
                            const image_fetcher::RequestMetadata& metadata) {
  if (image_callback.is_null()) {
    return;
  }
  std::move(image_callback).Run(image, metadata);
}

std::string EncodeSkBitmapToPNG(const std::string& uma_client_name,
                                const SkBitmap& bitmap) {
  std::vector<unsigned char> encoded_data;
  bool result = gfx::PNGCodec::Encode(
      static_cast<const unsigned char*>(bitmap.getPixels()),
      gfx::PNGCodec::FORMAT_RGBA, gfx::Size(bitmap.width(), bitmap.height()),
      static_cast<int>(bitmap.rowBytes()), /* discard_transparency */ false,
      std::vector<gfx::PNGCodec::Comment>(), &encoded_data);
  if (!result) {
    CachedImageFetcherMetricsReporter::ReportEvent(
        uma_client_name, CachedImageFetcherEvent::kTranscodingError);
    return "";
  } else {
    return std::string(encoded_data.begin(), encoded_data.end());
  }
}

}  // namespace

CachedImageFetcher::CachedImageFetcher(ImageFetcher* image_fetcher,
                                       scoped_refptr<ImageCache> image_cache,
                                       bool read_only)
    : image_fetcher_(image_fetcher),
      image_cache_(image_cache),
      read_only_(read_only),
      weak_ptr_factory_(this) {
  DCHECK(image_fetcher_);
  DCHECK(image_cache_);
}

CachedImageFetcher::~CachedImageFetcher() = default;

ImageDecoder* CachedImageFetcher::GetImageDecoder() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return image_fetcher_->GetImageDecoder();
}

void CachedImageFetcher::FetchImageAndData(
    const GURL& image_url,
    ImageDataFetcherCallback image_data_callback,
    ImageFetcherCallback image_callback,
    ImageFetcherParams params) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // TODO(wylieb): Inject a clock for better testability.
  CachedImageFetcherRequest request = {
      image_url, std::move(params),
      /* cache_hit_before_network_request */ false,
      /* start_time */ base::Time::Now()};

  CachedImageFetcherMetricsReporter::ReportEvent(
      request.params.uma_client_name(), CachedImageFetcherEvent::kImageRequest);

  if (params.skip_disk_cache_read()) {
    EnqueueFetchImageFromNetwork(request, std::move(image_data_callback),
                                 std::move(image_callback));
  } else {
    // First, try to load the image from the cache, then try the network.
    image_cache_->LoadImage(
        read_only_, image_url.spec(),
        base::BindOnce(&CachedImageFetcher::OnImageFetchedFromCache,
                       weak_ptr_factory_.GetWeakPtr(), std::move(request),
                       std::move(image_data_callback),
                       std::move(image_callback)));
  }
}

void CachedImageFetcher::OnImageFetchedFromCache(
    CachedImageFetcherRequest request,
    ImageDataFetcherCallback image_data_callback,
    ImageFetcherCallback image_callback,
    std::string image_data) {
  if (image_data.empty()) {
    CachedImageFetcherMetricsReporter::ReportEvent(
        request.params.uma_client_name(), CachedImageFetcherEvent::kCacheMiss);

    // Fetching from the DB failed, start a network fetch.
    EnqueueFetchImageFromNetwork(std::move(request),
                                 std::move(image_data_callback),
                                 std::move(image_callback));
  } else {
    DataCallbackIfPresent(std::move(image_data_callback), image_data,
                          RequestMetadata());
    CachedImageFetcherMetricsReporter::ReportEvent(
        request.params.uma_client_name(), CachedImageFetcherEvent::kCacheHit);

    // Only continue with decoding if the user actually asked for an image.
    if (!image_callback.is_null()) {
      GetImageDecoder()->DecodeImage(
          image_data, gfx::Size(),
          base::BindRepeating(&CachedImageFetcher::OnImageDecodedFromCache,
                              weak_ptr_factory_.GetWeakPtr(),
                              std::move(request),
                              base::Passed(std::move(image_data_callback)),
                              base::Passed(std::move(image_callback))));
    }
  }
}

void CachedImageFetcher::OnImageDecodedFromCache(
    CachedImageFetcherRequest request,
    ImageDataFetcherCallback image_data_callback,
    ImageFetcherCallback image_callback,
    const gfx::Image& image) {
  if (image.IsEmpty()) {
    // Upon failure, fetch from the network.
    request.cache_hit_before_network_request = true;
    EnqueueFetchImageFromNetwork(std::move(request),
                                 std::move(image_data_callback),
                                 std::move(image_callback));

    CachedImageFetcherMetricsReporter::ReportEvent(
        request.params.uma_client_name(),
        CachedImageFetcherEvent::kCacheDecodingError);
  } else {
    ImageCallbackIfPresent(std::move(image_callback), image, RequestMetadata());
    CachedImageFetcherMetricsReporter::ReportImageLoadFromCacheTime(
        request.params.uma_client_name(), request.start_time);
  }
}

void CachedImageFetcher::EnqueueFetchImageFromNetwork(
    CachedImageFetcherRequest request,
    ImageDataFetcherCallback image_data_callback,
    ImageFetcherCallback image_callback) {
  base::SequencedTaskRunnerHandle::Get()->PostTask(
      FROM_HERE,
      base::BindOnce(&CachedImageFetcher::FetchImageFromNetwork,
                     weak_ptr_factory_.GetWeakPtr(), std::move(request),
                     std::move(image_data_callback),
                     std::move(image_callback)));
}

void CachedImageFetcher::FetchImageFromNetwork(
    CachedImageFetcherRequest request,
    ImageDataFetcherCallback image_data_callback,
    ImageFetcherCallback image_callback) {
  const GURL& url = request.url;

  ImageDataFetcherCallback wrapper_data_callback;
  ImageFetcherCallback wrapper_image_callback;

  bool skip_transcoding = request.params.skip_transcoding();
  if (skip_transcoding) {
    wrapper_data_callback =
        base::BindOnce(&CachedImageFetcher::StoreImageDataWithoutTranscoding,
                       weak_ptr_factory_.GetWeakPtr(), std::move(request),
                       std::move(image_data_callback));
  } else {
    // Transcode the image when its downloaded from the network.
    // 1. Download the data.
    // 2. Decode the data to a gfx::Image in a utility process.
    // 3. Encode the data as a PNG in the browser process using base::PostTask.
    // 3. Cache the result.
    wrapper_data_callback = std::move(image_data_callback);
    wrapper_image_callback =
        base::BindOnce(&CachedImageFetcher::StoreImageDataWithTranscoding,
                       weak_ptr_factory_.GetWeakPtr(), std::move(request),
                       std::move(image_callback));
  }
  image_fetcher_->FetchImageAndData(url, std::move(wrapper_data_callback),
                                    std::move(wrapper_image_callback),
                                    std::move(request.params));
}

void CachedImageFetcher::StoreImageDataWithoutTranscoding(
    CachedImageFetcherRequest request,
    ImageDataFetcherCallback image_data_callback,
    const std::string& image_data,
    const RequestMetadata& request_metadata) {
  DataCallbackIfPresent(std::move(image_data_callback), image_data,
                        request_metadata);

  if (image_data.empty()) {
    CachedImageFetcherMetricsReporter::ReportEvent(
        request.params.uma_client_name(),
        CachedImageFetcherEvent::kTotalFailure);
  }

  StoreData(std::move(request), image_data);
}

void CachedImageFetcher::StoreImageDataWithTranscoding(
    CachedImageFetcherRequest request,
    ImageFetcherCallback image_callback,
    const gfx::Image& image,
    const RequestMetadata& request_metadata) {
  ImageCallbackIfPresent(std::move(image_callback), image, request_metadata);

  // Report to different histograms depending upon if there was a cache hit.
  if (request.cache_hit_before_network_request) {
    CachedImageFetcherMetricsReporter::ReportImageLoadFromNetworkAfterCacheHit(
        request.params.uma_client_name(), request.start_time);
  } else {
    CachedImageFetcherMetricsReporter::ReportImageLoadFromNetworkTime(
        request.params.uma_client_name(), request.start_time);
  }

  // Copy the image data out and store it on disk.
  const SkBitmap* bitmap = image.IsEmpty() ? nullptr : image.ToSkBitmap();
  // If the bitmap is null or otherwise not ready, skip encoding.
  if (bitmap == nullptr || bitmap->isNull() || !bitmap->readyToDraw()) {
    CachedImageFetcherMetricsReporter::ReportEvent(
        request.params.uma_client_name(),
        CachedImageFetcherEvent::kTotalFailure);
    StoreData(std::move(request), "");
  } else {
    std::string uma_client_name = request.params.uma_client_name();
    // Post a task to another thread to encode the image data downloaded.
    base::PostTaskAndReplyWithResult(
        FROM_HERE,
        base::BindOnce(&EncodeSkBitmapToPNG, uma_client_name, *bitmap),
        base::BindOnce(&CachedImageFetcher::StoreData,
                       weak_ptr_factory_.GetWeakPtr(), std::move(request)));
  }
}

void CachedImageFetcher::StoreData(CachedImageFetcherRequest request,
                                   std::string image_data) {
  std::string url = request.url.spec();
  // If the image is empty, delete the image.
  if (image_data.empty()) {
    image_cache_->DeleteImage(std::move(url));
    return;
  }

  if (!read_only_) {
    image_cache_->SaveImage(std::move(url), std::move(image_data));
  }
}

}  // namespace image_fetcher
