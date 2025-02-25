// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_GPU_TEST_VIDEO_PLAYER_VIDEO_PLAYER_TEST_ENVIRONMENT_H_
#define MEDIA_GPU_TEST_VIDEO_PLAYER_VIDEO_PLAYER_TEST_ENVIRONMENT_H_

#include <memory>

#include "base/at_exit.h"
#include "base/test/scoped_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(USE_OZONE)
namespace ui {
class OzoneGpuTestHelper;
}
#endif

namespace media {
namespace test {

class Video;

// Test environment for video decode tests. Performs setup and teardown once for
// the entire test run.
class VideoPlayerTestEnvironment : public ::testing::Environment {
 public:
  explicit VideoPlayerTestEnvironment(const Video* video);

  // Set up the video decode test environment, only called once.
  void SetUp() override;
  // Tear down the video decode test environment, only called once.
  void TearDown() override;

  std::unique_ptr<base::test::ScopedTaskEnvironment> task_environment_;
  const Video* video_ = nullptr;
  bool enable_validator_ = true;
  bool output_frames_ = false;

 private:
  // An exit manager is required to run callbacks on shutdown.
  base::AtExitManager at_exit_manager;

#if defined(USE_OZONE)
  std::unique_ptr<ui::OzoneGpuTestHelper> gpu_helper_;
#endif
};
}  // namespace test
}  // namespace media

#endif  // MEDIA_GPU_TEST_VIDEO_PLAYER_VIDEO_PLAYER_TEST_ENVIRONMENT_H_
