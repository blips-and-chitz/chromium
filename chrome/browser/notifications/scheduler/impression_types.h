// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_IMPRESSION_TYPES_H_
#define CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_IMPRESSION_TYPES_H_

#include <deque>
#include <map>

#include "base/optional.h"
#include "base/time/time.h"
#include "chrome/browser/notifications/scheduler/internal_types.h"
#include "chrome/browser/notifications/scheduler/notification_scheduler_types.h"

namespace notifications {

// Contains data to determine when a notification should be shown to the user
// and the user impression towards this notification.
//
// Life cycle:
// 1. Created after the notification is shown to the user.
// 2. |feedback| is set after the user interacts with the notification.
// 3. Notification scheduler API consumer gets the user feedback and generates
// an impression result, which may affect notification exposure.
// 4. The impression is deleted after it expires.
struct Impression {
  bool operator==(const Impression& other) const;

  // Creation timestamp.
  base::Time create_time;

  // The user feedback on the notification, each notification will have at most
  // one feedback. Sets after the user interacts with the notification.
  UserFeedback feedback = UserFeedback::kUnknown;

  // The impression type. The client of a notification type takes one or several
  // user feedbacks as input and generate a user impression, which will
  // eventually affect the rate to deliver notifications to the user.
  ImpressionResult impression = ImpressionResult::kUnknown;

  // If the user feedback is used in computing the current notification deliver
  // rate.
  bool integrated = false;

  // The task start time when this impression is generated.
  SchedulerTaskTime task_start_time = SchedulerTaskTime::kUnknown;
};

// Contains details about supression and recovery after suppression expired.
struct SuppressionInfo {
  SuppressionInfo(const base::Time& last_trigger,
                  const base::TimeDelta& duration);
  SuppressionInfo(const SuppressionInfo& other);
  ~SuppressionInfo() = default;
  bool operator==(const SuppressionInfo& other) const;

  // The last supression trigger time.
  base::Time last_trigger_time;

  // The duration for the suppression.
  base::TimeDelta duration;

  // |current_max_daily_show| will change to this after the suppression
  // expired.
  int recover_goal;
};

// Stores the global states about how often the notification can be shown
// to the user and the history of user interactions to a particular notification
// client.
struct ClientState {
  using Impressions = std::deque<Impression>;
  explicit ClientState(SchedulerClientType type);
  explicit ClientState(const ClientState& other);
  ~ClientState();

  bool operator==(const ClientState& other) const;

  // Dumps data for debugging.
  std::string DebugPrint() const;

  // The type of notification using the scheduler.
  const SchedulerClientType type;

  // The maximum number of notifications shown to the user for this type. May
  // change if the user interacts with the notification.
  int current_max_daily_show;

  // A list of user impression history. Sorted by creation time.
  Impressions impressions;

  // Suppression details, no value if there is currently no suppression.
  base::Optional<SuppressionInfo> suppression_info;
};

}  // namespace notifications

#endif  // CHROME_BROWSER_NOTIFICATIONS_SCHEDULER_IMPRESSION_TYPES_H_
