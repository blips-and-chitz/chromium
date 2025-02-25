// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_SHADER_MODULE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_SHADER_MODULE_H_

#include "third_party/blink/renderer/modules/webgpu/dawn_object.h"

namespace blink {

class GPUShaderModule : public DawnObject<DawnShaderModule> {
  DEFINE_WRAPPERTYPEINFO();

 public:
  static GPUShaderModule* Create(GPUDevice* device,
                                 DawnShaderModule shader_module);
  explicit GPUShaderModule(GPUDevice* device, DawnShaderModule shader_module);
  ~GPUShaderModule() override;

 private:
  DISALLOW_COPY_AND_ASSIGN(GPUShaderModule);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_WEBGPU_GPU_SHADER_MODULE_H_
