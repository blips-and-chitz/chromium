// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_ARC_TRACING_ARC_TRACING_GRAPHICS_MODEL_H_
#define CHROME_BROWSER_CHROMEOS_ARC_TRACING_ARC_TRACING_GRAPHICS_MODEL_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/values.h"
#include "chrome/browser/chromeos/arc/tracing/arc_cpu_model.h"

namespace arc {

class ArcTracingModel;

// Graphic buffers events model. It is build from the generic |ArcTracingModel|
// and contains only events that describe life-cycle of graphics buffers across
// Android and Chrome. It also includes top level graphics events in Chrome and
// Android. Events in this model have type and timestamp and grouped per each
// view, which is defined by Activity name and Android task id.
// View events are kept separately per individual view and each view may own
// multiple graphics buffers. Following is the structure of events:
// |android_top_level_| - top level rendering events from Android
// |chrome_top_level_| - top level rendering events from Chrome.
// |view_buffers_| - map views to buffer events.
// -- view1
//    -- buffer_1
//       ...
//    -- buffer_n (usually 4 buffers per view)
// -- view2
//       ...
// In normal conditions events are expected to follow the pattern when events
// appear in predefined order. Breaking this sequence usually indicates missing
// frame, junk or another problem with rendering.
class ArcTracingGraphicsModel {
 public:
  enum class BufferEventType {
    kNone,  // 0

    // Surface flinger events.
    kBufferQueueDequeueStart = 100,  // 100
    kBufferQueueDequeueDone,         // 101
    kBufferQueueQueueStart,          // 102
    kBufferQueueQueueDone,           // 103
    kBufferQueueAcquire,             // 104
    kBufferQueueReleased,            // 105
    kBufferFillJank,                 // 106,

    // Wayland exo events
    kExoSurfaceAttach = 200,  // 200
    kExoProduceResource,      // 201
    kExoBound,                // 202
    kExoPendingQuery,         // 203
    kExoReleased,             // 204
    kExoJank,                 // 205

    // Chrome events
    kChromeBarrierOrder = 300,  // 300
    kChromeBarrierFlush,        // 301

    // Android Surface Flinger top level events.
    kVsync = 400,                      // 400
    kSurfaceFlingerInvalidationStart,  // 401
    kSurfaceFlingerInvalidationDone,   // 402
    kSurfaceFlingerCompositionStart,   // 403
    kSurfaceFlingerCompositionDone,    // 404
    kSurfaceFlingerCompositionJank,    // 405,

    // Chrome OS top level events.
    kChromeOSDraw = 500,        // 500
    kChromeOSSwap,              // 501
    kChromeOSWaitForAck,        // 502
    kChromeOSPresentationDone,  // 503
    kChromeOSSwapDone,          // 504
    kChromeOSJank,              // 505,
  };

  struct BufferEvent {
    BufferEvent(BufferEventType type, int64_t timestamp);

    bool operator==(const BufferEvent& other) const;

    BufferEventType type;
    int64_t timestamp;
  };

  struct ViewId {
    ViewId(int task_id, const std::string& activity);

    bool operator<(const ViewId& other) const;
    bool operator==(const ViewId& other) const;

    int task_id;
    std::string activity;
  };

  using BufferEvents = std::vector<BufferEvent>;

  class EventsContainer {
   public:
    EventsContainer();
    ~EventsContainer();

    void Reset();

    bool operator==(const EventsContainer& other) const;

    std::vector<BufferEvents>& buffer_events() { return buffer_events_; }
    const std::vector<BufferEvents>& buffer_events() const {
      return buffer_events_;
    }
    BufferEvents& global_events() { return global_events_; }
    const BufferEvents& global_events() const { return global_events_; }

   private:
    // Events associated with particular graphics buffer.
    std::vector<BufferEvents> buffer_events_;
    // Global events that do not belong to any graphics buffer.
    BufferEvents global_events_;

    DISALLOW_COPY_AND_ASSIGN(EventsContainer);
  };

  using ViewMap = std::map<ViewId, EventsContainer>;

  ArcTracingGraphicsModel();
  ~ArcTracingGraphicsModel();

  // Builds the model from the common tracing model |common_model|.
  bool Build(const ArcTracingModel& common_model);

  // Serializes the model to |base::DictionaryValue|, this can be passed to
  // javascript for rendering.
  std::unique_ptr<base::DictionaryValue> Serialize() const;
  // Serializes the model to Json string.
  std::string SerializeToJson() const;
  // Loads the model from Json string.
  bool LoadFromJson(const std::string& json_data);
  // Loads the model from |base::DictionaryValue|.
  bool LoadFromValue(const base::DictionaryValue& root);

  uint64_t duration() const { return duration_; }

  const ViewMap& view_buffers() const { return view_buffers_; }

  const EventsContainer& android_top_level() const {
    return android_top_level_;
  }

  const EventsContainer& chrome_top_level() const { return chrome_top_level_; }

  ArcCpuModel& cpu_model() { return cpu_model_; }
  const ArcCpuModel& cpu_model() const { return cpu_model_; }

 private:
  // Normalizes timestamp for all events by subtracting the timestamp of the
  // earliest event.
  void NormalizeTimestamps();

  // Resets whole model.
  void Reset();

  // Extracts task id from the Chrome buffer name. Returns -1 if task id cannot
  // be extracted.
  int GetTaskIdFromBufferName(const std::string& chrome_buffer_name) const;

  ViewMap view_buffers_;
  // To avoid overlapping events are stored interlaced.
  EventsContainer chrome_top_level_;
  EventsContainer android_top_level_;
  // Total duration of this model.
  uint32_t duration_ = 0;
  // Map Chrome buffer id to task id.
  std::map<std::string, int> chrome_buffer_id_to_task_id_;
  // CPU event model.
  ArcCpuModel cpu_model_;

  DISALLOW_COPY_AND_ASSIGN(ArcTracingGraphicsModel);
};

std::ostream& operator<<(std::ostream& os,
                         ArcTracingGraphicsModel::BufferEventType);

}  // namespace arc

#endif  // CHROME_BROWSER_CHROMEOS_ARC_TRACING_ARC_TRACING_GRAPHICS_MODEL_H_
