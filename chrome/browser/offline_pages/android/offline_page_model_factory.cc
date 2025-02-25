// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/offline_pages/offline_page_model_factory.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/memory/singleton.h"
#include "base/path_service.h"
#include "base/sequenced_task_runner.h"
#include "base/task/post_task.h"
#include "base/time/default_clock.h"
#include "chrome/browser/download/download_prefs.h"
#include "chrome/browser/offline_pages/android/cct_origin_observer.h"
#include "chrome/browser/offline_pages/android/offline_pages_download_manager_bridge.h"
#include "chrome/browser/offline_pages/download_archive_manager.h"
#include "chrome/browser/offline_pages/fresh_offline_content_observer.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_constants.h"
#include "components/keyed_service/core/simple_dependency_manager.h"
#include "components/offline_pages/core/model/offline_page_model_taskified.h"
#include "components/offline_pages/core/offline_page_metadata_store.h"

namespace offline_pages {

OfflinePageModelFactory::OfflinePageModelFactory()
    : SimpleKeyedServiceFactory("OfflinePageModel",
                                SimpleDependencyManager::GetInstance()) {}

// static
OfflinePageModelFactory* OfflinePageModelFactory::GetInstance() {
  return base::Singleton<OfflinePageModelFactory>::get();
}

// static
OfflinePageModel* OfflinePageModelFactory::GetForKey(SimpleFactoryKey* key,
                                                     PrefService* prefs) {
  return static_cast<OfflinePageModel*>(
      GetInstance()->GetServiceForKey(key, prefs, /*create=*/true));
}

// static
OfflinePageModel* OfflinePageModelFactory::GetForBrowserContext(
    content::BrowserContext* browser_context) {
  Profile* profile = Profile::FromBrowserContext(browser_context);
  return GetForKey(profile->GetSimpleFactoryKey(), profile->GetPrefs());
}

std::unique_ptr<KeyedService> OfflinePageModelFactory::BuildServiceInstanceFor(
    SimpleFactoryKey* key,
    PrefService* prefs) const {
  scoped_refptr<base::SequencedTaskRunner> background_task_runner =
      base::CreateSequencedTaskRunnerWithTraits({base::MayBlock()});

  base::FilePath store_path =
      key->path().Append(chrome::kOfflinePageMetadataDirname);
  std::unique_ptr<OfflinePageMetadataStore> metadata_store(
      new OfflinePageMetadataStore(background_task_runner, store_path));

  base::FilePath persistent_archives_dir =
      key->path().Append(chrome::kOfflinePageArchivesDirname);
  // If base::PathService::Get returns false, the temporary_archives_dir will be
  // empty, and no temporary pages will be saved during this chrome lifecycle.
  base::FilePath temporary_archives_dir;
  if (base::PathService::Get(base::DIR_CACHE, &temporary_archives_dir)) {
    temporary_archives_dir =
        temporary_archives_dir.Append(chrome::kOfflinePageArchivesDirname);
  }
  std::unique_ptr<ArchiveManager> archive_manager(new DownloadArchiveManager(
      temporary_archives_dir, persistent_archives_dir,
      DownloadPrefs::GetDefaultDownloadDirectory(), background_task_runner,
      prefs));
  auto clock = std::make_unique<base::DefaultClock>();

  std::unique_ptr<SystemDownloadManager> download_manager(
      new android::OfflinePagesDownloadManagerBridge());

  std::unique_ptr<OfflinePageModelTaskified> model =
      std::make_unique<OfflinePageModelTaskified>(
          std::move(metadata_store), std::move(archive_manager),
          std::move(download_manager), background_task_runner);

  CctOriginObserver::AttachToOfflinePageModel(model.get());

  FreshOfflineContentObserver::AttachToOfflinePageModel(model.get());

  return model;
}

}  // namespace offline_pages
