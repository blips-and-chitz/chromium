// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_PERFORMANCE_MANAGER_GRAPH_SYSTEM_NODE_IMPL_H_
#define CHROME_BROWSER_PERFORMANCE_MANAGER_GRAPH_SYSTEM_NODE_IMPL_H_

#include <cstdint>
#include <memory>
#include <vector>

#include "base/macros.h"
#include "base/process/process_handle.h"
#include "base/time/time.h"
#include "chrome/browser/performance_manager/graph/node_base.h"

namespace performance_manager {

// TODO(siggi): In the end game, this should be private implementation detail
//     of the performance measurement graph decorator. It's here for now because
//     there's still a thread hop to get the measurement results into the graph.
struct ProcessResourceMeasurement {
  ProcessResourceMeasurement();

  // Identifies the process associated with this measurement.
  base::ProcessId pid;

  // The cumulative CPU usage accrued to this process from its start.
  base::TimeDelta cpu_usage;

  // The private memory footprint of the process.
  uint32_t private_footprint_kb = 0;
};

struct ProcessResourceMeasurementBatch {
  ProcessResourceMeasurementBatch();
  ~ProcessResourceMeasurementBatch();

  // These times bracket the capture of the entire dump, e.g. each distinct
  // measurement is captured somewhere between |batch_started_time| and
  // |batch_ended_time|.
  base::TimeTicks batch_started_time;
  base::TimeTicks batch_ended_time;

  std::vector<ProcessResourceMeasurement> measurements;
};

class SystemNodeImpl : public TypedNodeBase<SystemNodeImpl> {
 public:
  static constexpr resource_coordinator::CoordinationUnitType Type() {
    return resource_coordinator::CoordinationUnitType::kSystem;
  }

  explicit SystemNodeImpl(Graph* graph);
  ~SystemNodeImpl() override;

  void OnProcessCPUUsageReady();
  void DistributeMeasurementBatch(
      std::unique_ptr<ProcessResourceMeasurementBatch> measurement_batch);

  // Accessors for the start/end times bracketing when the last performance
  // measurement occurred.
  base::TimeTicks last_measurement_start_time() const {
    return last_measurement_start_time_;
  }
  base::TimeTicks last_measurement_end_time() const {
    return last_measurement_end_time_;
  }

 private:
  base::TimeTicks last_measurement_start_time_;
  base::TimeTicks last_measurement_end_time_;

  // CoordinationUnitInterface implementation:
  void OnEventReceived(resource_coordinator::mojom::Event event) override;

  DISALLOW_COPY_AND_ASSIGN(SystemNodeImpl);
};

}  // namespace performance_manager

#endif  // CHROME_BROWSER_PERFORMANCE_MANAGER_GRAPH_SYSTEM_NODE_IMPL_H_
