// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_RENDER_PIPELINE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_RENDER_PIPELINE_H_

#include "third_party/blink/renderer/modules/webgpu/dawn_object.h"

namespace blink {

class GPURenderPipeline : public DawnObject<DawnRenderPipeline> {
  DEFINE_WRAPPERTYPEINFO();

 public:
  static GPURenderPipeline* Create(GPUDevice* device,
                                   DawnRenderPipeline render_pipeline);
  explicit GPURenderPipeline(GPUDevice* device,
                             DawnRenderPipeline render_pipeline);
  ~GPURenderPipeline() override;

 private:
  DISALLOW_COPY_AND_ASSIGN(GPURenderPipeline);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_RENDER_PIPELINE_H_
