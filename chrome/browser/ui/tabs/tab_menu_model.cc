// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/tabs/tab_menu_model.h"

#include "base/command_line.h"
#include "base/metrics/user_metrics.h"
#include "chrome/browser/browser_features.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/send_tab_to_self/send_tab_to_self_desktop_util.h"
#include "chrome/browser/send_tab_to_self/send_tab_to_self_util.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/ui/tabs/existing_tab_group_sub_menu_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_delegate.h"
#include "chrome/browser/ui/tabs/tab_utils.h"
#include "chrome/common/chrome_features.h"
#include "chrome/grit/generated_resources.h"
#include "ui/base/l10n/l10n_util.h"

using base::UserMetricsAction;

TabMenuModel::TabMenuModel(ui::SimpleMenuModel::Delegate* delegate,
                           TabStripModel* tab_strip,
                           int index)
    : ui::SimpleMenuModel(delegate) {
  Build(tab_strip, index);
}

TabMenuModel::~TabMenuModel() {}

void TabMenuModel::Build(TabStripModel* tab_strip, int index) {
  std::vector<int> affected_indices =
      tab_strip->IsTabSelected(index)
          ? tab_strip->selection_model().selected_indices()
          : std::vector<int>{index};
  int num_affected_tabs = affected_indices.size();
  AddItemWithStringId(TabStripModel::CommandNewTab, IDS_TAB_CXMENU_NEWTAB);
  if (base::FeatureList::IsEnabled(features::kTabGroups)) {
    AddItemWithStringId(TabStripModel::CommandAddToNewGroup,
                        IDS_TAB_CXMENU_ADD_TAB_TO_NEW_GROUP);

    // Create submenu with existing groups
    if (ExistingTabGroupSubMenuModel::ShouldShowSubmenu(tab_strip, index)) {
      add_to_existing_group_submenu_ =
          std::make_unique<ExistingTabGroupSubMenuModel>(tab_strip, index);
      AddSubMenuWithStringId(TabStripModel::CommandAddToExistingGroup,
                             IDS_TAB_CXMENU_ADD_TAB_TO_EXISTING_GROUP,
                             add_to_existing_group_submenu_.get());
    }

    for (size_t index = 0; index < affected_indices.size(); index++) {
      if (tab_strip->GetTabGroupForTab(affected_indices[index]) != nullptr) {
        AddItemWithStringId(TabStripModel::CommandRemoveFromGroup,
                            IDS_TAB_CXMENU_REMOVE_TAB_FROM_GROUP);
        break;
      }
    }
  }
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItemWithStringId(TabStripModel::CommandReload, IDS_TAB_CXMENU_RELOAD);
  AddItemWithStringId(TabStripModel::CommandDuplicate,
                      IDS_TAB_CXMENU_DUPLICATE);
  bool will_pin = tab_strip->WillContextMenuPin(index);
  AddItem(TabStripModel::CommandTogglePinned,
          will_pin ? l10n_util::GetPluralStringFUTF16(IDS_TAB_CXMENU_PIN_TAB,
                                                      num_affected_tabs)
                   : l10n_util::GetPluralStringFUTF16(IDS_TAB_CXMENU_UNPIN_TAB,
                                                      num_affected_tabs));
  if (base::FeatureList::IsEnabled(features::kFocusMode)) {
    // TODO(crbug.com/941577): Allow Focus Mode in Incognito and Guest Session.
    if (!tab_strip->profile()->IsOffTheRecord()) {
      AddItemWithStringId(TabStripModel::CommandFocusMode,
                          IDS_TAB_CXMENU_FOCUS_THIS_TAB);
    }
  }
  const bool will_mute =
      !chrome::AreAllSitesMuted(*tab_strip, affected_indices);
  AddItem(TabStripModel::CommandToggleSiteMuted,
          will_mute ? l10n_util::GetPluralStringFUTF16(
                          IDS_TAB_CXMENU_SOUND_MUTE_SITE, num_affected_tabs)
                    : l10n_util::GetPluralStringFUTF16(
                          IDS_TAB_CXMENU_SOUND_UNMUTE_SITE, num_affected_tabs));

  if (send_tab_to_self::ShouldOfferFeature(
          tab_strip->profile(), tab_strip->GetWebContentsAt(index))) {
    send_tab_to_self::RecordSendTabToSelfClickResult(
        send_tab_to_self::kTabMenu, SendTabToSelfClickResult::kShowItem);
    AddSeparator(ui::NORMAL_SEPARATOR);
    AddItemWithStringIdAndIcon(TabStripModel::CommandSendTabToSelf,
                               IDS_CONTEXT_MENU_SEND_TAB_TO_SELF,
                               *send_tab_to_self::GetImageSkia());
  }
  AddSeparator(ui::NORMAL_SEPARATOR);
  AddItem(TabStripModel::CommandCloseTab,
          l10n_util::GetPluralStringFUTF16(IDS_TAB_CXMENU_CLOSETAB,
                                           num_affected_tabs));
  AddItemWithStringId(TabStripModel::CommandCloseOtherTabs,
                      IDS_TAB_CXMENU_CLOSEOTHERTABS);
  AddItemWithStringId(TabStripModel::CommandCloseTabsToRight,
                      IDS_TAB_CXMENU_CLOSETABSTORIGHT);
  AddSeparator(ui::NORMAL_SEPARATOR);

  const bool is_window = tab_strip->delegate()->GetRestoreTabType() ==
      TabStripModelDelegate::RESTORE_WINDOW;
  AddItemWithStringId(TabStripModel::CommandRestoreTab,
                      is_window ? IDS_RESTORE_WINDOW : IDS_RESTORE_TAB);
  AddItemWithStringId(TabStripModel::CommandBookmarkAllTabs,
                      IDS_TAB_CXMENU_BOOKMARK_ALL_TABS);
}
