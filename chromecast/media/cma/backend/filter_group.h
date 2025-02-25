// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMECAST_MEDIA_CMA_BACKEND_FILTER_GROUP_H_
#define CHROMECAST_MEDIA_CMA_BACKEND_FILTER_GROUP_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/macros.h"
#include "base/memory/aligned_memory.h"
#include "base/values.h"
#include "chromecast/public/media/media_pipeline_backend.h"
#include "chromecast/public/volume_control.h"

namespace media {
class AudioBus;
}  // namespace media

namespace chromecast {
namespace media {
class MixerInput;
class PostProcessingPipeline;

// FilterGroup mixes MixerInputs and/or FilterGroups, mixes their outputs, and
// applies DSP to them.

// FilterGroups are added at construction. These cannot be removed.

// InputQueues are added with AddActiveInput(), then cleared when
// MixAndFilter() is called (they must be added each time data is queried).
class FilterGroup {
 public:
  // |num_channels| indicates number of input audio channels.
  // |name| is used for debug printing
  // |pipeline| - processing pipeline.
  FilterGroup(int num_channels,
              const std::string& name,
              std::unique_ptr<PostProcessingPipeline> pipeline);

  ~FilterGroup();

  // |input| will be recursively mixed into this FilterGroup's input buffer when
  // MixAndFilter() is called. Registering a FilterGroup as an input to more
  // than one FilterGroup will result in incorrect behavior.
  void AddMixedInput(FilterGroup* input);

  // Recursively sets the sample rate of the post-processors and FilterGroups.
  // This should only be called externally on the output node of the FilterGroup
  // tree.
  // The output rate of this group will be |output_samples_per_second|.
  // The output block size, i.e. the number of frames written in each call to
  // MixAndFilter() of this group will be |output_frames_per_write|.
  // Groups that feed this group may receive different values due to resampling.
  // After calling Initialize(), input_samples_per_second() and
  // input_frames_per_write() may be called to determine the input rate/size.
  void Initialize(int output_samples_per_second, int output_frames_per_write);

  // Adds/removes |input| from |active_inputs_|.
  void AddInput(MixerInput* input);
  void RemoveInput(MixerInput* input);

  // Mixes all active inputs and passes them through the audio filter.
  // Returns the largest volume of all streams with data.
  //         return value will be zero IFF there is no data and
  //         the PostProcessingPipeline is not ringing.
  float MixAndFilter(
      int num_frames,
      MediaPipelineBackend::AudioDecoder::RenderingDelay rendering_delay);

  // Gets the current delay of this filter group's AudioPostProcessors.
  // (Not recursive).
  int64_t GetRenderingDelayMicroseconds();

  // Gets the delay of this FilterGroup and all downstream FilterGroups.
  // Computed recursively when MixAndFilter() is called.
  MediaPipelineBackend::AudioDecoder::RenderingDelay
  GetRenderingDelayToOutput();

  // Retrieves a pointer to the output buffer. This will crash if called before
  // MixAndFilter(), and the data & memory location may change each time
  // MixAndFilter() is called.
  float* GetOutputBuffer();

  // Get the last used volume.
  float last_volume() const { return last_volume_; }

  std::string name() const { return name_; }

  // Returns number of audio output channels from the filter group.
  int GetOutputChannelCount() const;

  // Returns the expected sample rate for inputs to this group.
  int GetInputSampleRate() const { return input_samples_per_second_; }

  // Sends configuration string |config| to all post processors with the given
  // |name|.
  void SetPostProcessorConfig(const std::string& name,
                              const std::string& config);

  // Sets the active channel for post processors.
  void UpdatePlayoutChannel(int playout_channel);

  // Get content type
  AudioContentType content_type() const { return content_type_; }

  // Recursively print the layout of the pipeline.
  void PrintTopology() const;

  // Add |stream_type| to the list of streams this processor handles.
  void AddStreamType(const std::string& stream_type);

  int input_frames_per_write() const { return input_frames_per_write_; }
  int input_samples_per_second() const { return input_samples_per_second_; }

 private:
  // Resizes temp_buffers_ and mixed_.
  void ResizeBuffers();
  void AddTempBuffer(int num_channels, int num_frames);

  const int num_channels_;
  const std::string name_;
  std::vector<FilterGroup*> mixed_inputs_;
  std::vector<std::string> stream_types_;
  base::flat_set<MixerInput*> active_inputs_;

  int playout_channel_selection_ = kChannelAll;
  int output_samples_per_second_ = 0;
  int input_samples_per_second_ = 0;
  int output_frames_per_write_ = 0;
  int input_frames_per_write_ = 0;
  int frames_zeroed_ = 0;
  float last_volume_ = 0.0;
  double delay_seconds_ = 0;
  MediaPipelineBackend::AudioDecoder::RenderingDelay rendering_delay_to_output_;
  AudioContentType content_type_ = AudioContentType::kMedia;

  // Buffers that hold audio data while it is mixed.
  // These are kept as members of this class to minimize copies and
  // allocations.
  std::vector<std::unique_ptr<::media::AudioBus>> temp_buffers_;
  std::unique_ptr<::media::AudioBus> mixed_;

  // Interleaved data must be aligned to 16 bytes.
  std::unique_ptr<float, base::AlignedFreeDeleter> interleaved_;

  std::unique_ptr<PostProcessingPipeline> post_processing_pipeline_;

  DISALLOW_COPY_AND_ASSIGN(FilterGroup);
};

}  // namespace media
}  // namespace chromecast

#endif  // CHROMECAST_MEDIA_CMA_BACKEND_FILTER_GROUP_H_
