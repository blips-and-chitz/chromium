// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/offline_items_collection/core/throttled_offline_content_provider.h"

#include <utility>

#include "base/bind.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "components/offline_items_collection/core/offline_item.h"

namespace offline_items_collection {
namespace {
const int kDelayBetweenUpdatesMs = 1000;
}  // namespace

ThrottledOfflineContentProvider::ThrottledOfflineContentProvider(
    OfflineContentProvider* provider)
    : ThrottledOfflineContentProvider(
          base::TimeDelta::FromMilliseconds(kDelayBetweenUpdatesMs),
          provider) {}

ThrottledOfflineContentProvider::ThrottledOfflineContentProvider(
    const base::TimeDelta& delay_between_updates,
    OfflineContentProvider* provider)
    : delay_between_updates_(delay_between_updates),
      last_update_time_(base::TimeTicks::Now()),
      update_queued_(false),
      wrapped_provider_(provider),
      weak_ptr_factory_(this) {
  DCHECK(wrapped_provider_);
  wrapped_provider_->AddObserver(this);
}

ThrottledOfflineContentProvider::~ThrottledOfflineContentProvider() {
  wrapped_provider_->RemoveObserver(this);
}

void ThrottledOfflineContentProvider::OpenItem(LaunchLocation location,
                                               const ContentId& id) {
  wrapped_provider_->OpenItem(location, id);
  FlushUpdates();
}

void ThrottledOfflineContentProvider::RemoveItem(const ContentId& id) {
  wrapped_provider_->RemoveItem(id);
  FlushUpdates();
}

void ThrottledOfflineContentProvider::CancelDownload(const ContentId& id) {
  wrapped_provider_->CancelDownload(id);
  FlushUpdates();
}

void ThrottledOfflineContentProvider::PauseDownload(const ContentId& id) {
  wrapped_provider_->PauseDownload(id);
  FlushUpdates();
}

void ThrottledOfflineContentProvider::ResumeDownload(const ContentId& id,
                                                     bool has_user_gesture) {
  wrapped_provider_->ResumeDownload(id, has_user_gesture);
  FlushUpdates();
}

void ThrottledOfflineContentProvider::GetItemById(const ContentId& id,
                                                  SingleItemCallback callback) {
  wrapped_provider_->GetItemById(
      id, base::BindOnce(&ThrottledOfflineContentProvider::OnGetItemByIdDone,
                         weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void ThrottledOfflineContentProvider::GetAllItems(
    MultipleItemCallback callback) {
  wrapped_provider_->GetAllItems(
      base::BindOnce(&ThrottledOfflineContentProvider::OnGetAllItemsDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void ThrottledOfflineContentProvider::OnGetAllItemsDone(
    MultipleItemCallback callback,
    const OfflineItemList& items) {
  for (const auto item : items)
    UpdateItemIfPresent(item);
  std::move(callback).Run(items);
}

void ThrottledOfflineContentProvider::OnGetItemByIdDone(
    SingleItemCallback callback,
    const base::Optional<OfflineItem>& item) {
  if (item.has_value())
    UpdateItemIfPresent(item.value());
  std::move(callback).Run(item);
}

void ThrottledOfflineContentProvider::GetVisualsForItem(
    const ContentId& id,
    VisualsCallback callback) {
  wrapped_provider_->GetVisualsForItem(id, std::move(callback));
}

void ThrottledOfflineContentProvider::GetShareInfoForItem(
    const ContentId& id,
    ShareCallback callback) {
  wrapped_provider_->GetShareInfoForItem(id, std::move(callback));
}

void ThrottledOfflineContentProvider::RenameItem(const ContentId& id,
                                                 const std::string& name,
                                                 RenameCallback callback) {
  wrapped_provider_->RenameItem(id, name, std::move(callback));
}

void ThrottledOfflineContentProvider::AddObserver(
    OfflineContentProvider::Observer* observer) {
  DCHECK(observer);
  observers_.AddObserver(observer);
}

void ThrottledOfflineContentProvider::RemoveObserver(
    OfflineContentProvider::Observer* observer) {
  observers_.RemoveObserver(observer);
}

void ThrottledOfflineContentProvider::OnItemsAdded(
    const OfflineItemList& items) {
  for (auto& observer : observers_)
    observer.OnItemsAdded(items);
}

void ThrottledOfflineContentProvider::OnItemRemoved(const ContentId& id) {
  updates_.erase(id);
  for (auto& observer : observers_)
    observer.OnItemRemoved(id);
}

void ThrottledOfflineContentProvider::OnItemUpdated(const OfflineItem& item) {
  updates_[item.id] = item;

  // If we already queued an update, we're throttling, just wait until the
  // update passes through.
  if (update_queued_)
    return;

  // If we haven't sent an update recently, let the update go through.
  base::TimeDelta current_delay = base::TimeTicks::Now() - last_update_time_;
  if (current_delay >= delay_between_updates_) {
    FlushUpdates();
    return;
  }

  // Queue the update so we wait for the proper amount of time before notifying
  // observers.
  update_queued_ = true;
  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&ThrottledOfflineContentProvider::FlushUpdates,
                     weak_ptr_factory_.GetWeakPtr()),
      delay_between_updates_ - current_delay);
}

void ThrottledOfflineContentProvider::UpdateItemIfPresent(
    const OfflineItem& item) {
  auto it = updates_.find(item.id);
  if (it != updates_.end())
    it->second = item;
}

void ThrottledOfflineContentProvider::FlushUpdates() {
  last_update_time_ = base::TimeTicks::Now();
  update_queued_ = false;

  OfflineItemMap updates = std::move(updates_);
  for (auto item_pair : updates) {
    for (auto& observer : observers_)
      observer.OnItemUpdated(item_pair.second);
  }
}

}  // namespace offline_items_collection
