// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_APP_LIST_TEST_TEST_APP_LIST_CLIENT_H_
#define ASH_APP_LIST_TEST_TEST_APP_LIST_CLIENT_H_

#include <string>

#include "ash/public/interfaces/app_list.mojom.h"
#include "base/macros.h"
#include "mojo/public/cpp/bindings/binding.h"

namespace ash {

// A test implementation of AppListClient that records function call counts.
// Registers itself as the presenter for the app list on construction.
class TestAppListClient : public mojom::AppListClient {
 public:
  TestAppListClient();
  ~TestAppListClient() override;

  mojom::AppListClientPtr CreateInterfacePtrAndBind();

  // ash::mojom::AppListClient:
  void StartSearch(const base::string16& trimmed_query) override {}
  void OpenSearchResult(const std::string& result_id,
                        int event_flags,
                        ash::mojom::AppListLaunchedFrom launched_from,
                        ash::mojom::AppListLaunchType launch_type,
                        int suggestion_index) override {}
  void InvokeSearchResultAction(const std::string& result_id,
                                int action_index,
                                int event_flags) override {}
  void GetSearchResultContextMenuModel(
      const std::string& result_id,
      GetContextMenuModelCallback callback) override;
  void SearchResultContextMenuItemSelected(const std::string& result_id,
                                           int command_id,
                                           int event_flags) override {}
  void ViewClosing() override {}
  void ViewShown(int64_t display_id) override {}
  void ActivateItem(const std::string& id, int event_flags) override {}
  void GetContextMenuModel(const std::string& id,
                           GetContextMenuModelCallback callback) override;
  void ContextMenuItemSelected(const std::string& id,
                               int command_id,
                               int event_flags) override {}
  void OnAppListTargetVisibilityChanged(bool visible) override {}
  void OnAppListVisibilityChanged(bool visible) override {}
  void OnFolderCreated(mojom::AppListItemMetadataPtr item) override {}
  void OnFolderDeleted(mojom::AppListItemMetadataPtr item) override {}
  void OnItemUpdated(mojom::AppListItemMetadataPtr item) override {}
  void OnPageBreakItemAdded(const std::string& id,
                            const syncer::StringOrdinal& position) override {}
  void OnPageBreakItemDeleted(const std::string& id) override {}
  void GetNavigableContentsFactory(
      mojo::PendingReceiver<content::mojom::NavigableContentsFactory> receiver)
      override {}
  void OnSearchResultVisibilityChanged(const std::string& id,
                                       bool visibility) override {}

 private:
  mojo::Binding<mojom::AppListClient> binding_;

  DISALLOW_COPY_AND_ASSIGN(TestAppListClient);
};

}  // namespace ash

#endif  // ASH_APP_LIST_TEST_TEST_APP_LIST_CLIENT_H_
