// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_PUBLIC_CPP_APP_LIST_APP_LIST_TYPES_H_
#define ASH_PUBLIC_CPP_APP_LIST_APP_LIST_TYPES_H_

#include <vector>

#include "ash/public/cpp/ash_public_export.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/range/range.h"

namespace ash {

// Id of OEM folder in app list.
ASH_PUBLIC_EXPORT extern const char kOemFolderId[];

// All possible states of the app list.
// Note: Do not change the order of these as they are used for metrics.
enum class AppListState {
  kStateApps = 0,
  kStateSearchResults,
  kStateStart_DEPRECATED,
  kStateEmbeddedAssistant,
  // Add new values here.

  kInvalidState,               // Don't use over IPC
  kStateLast = kInvalidState,  // Don't use over IPC
};

// The status of the app list model.
enum class AppListModelStatus {
  kStatusNormal,
  kStatusSyncing,  // Syncing apps or installing synced apps.
};

// Type of the search result, which is set in Chrome.
enum class SearchResultType {
  kUnknown,         // Unknown type. Don't use over IPC
  kInstalledApp,    // Installed apps.
  kPlayStoreApp,    // Installable apps from PlayStore.
  kInstantApp,      // Instant apps.
  kInternalApp,     // Chrome OS apps.
  kWebStoreApp,     // Installable apps from WebStore.
  kWebStoreSearch,  // A search query in WebStore.
  kOmnibox,         // Results from Omnibox.
  kLauncher,        // Results from launcher search (currently only from Files).
  kAnswerCard,      // WebContents based answer card.
  kPlayStoreReinstallApp,  // Reinstall recommendations from PlayStore.
  kArcAppShortcut,         // ARC++ app shortcuts.
  // Add new values here.
};

// How the result should be displayed. Do not change the order of these as
// they are used for metrics.
enum SearchResultDisplayType {
  kNone = 0,
  kList,
  kTile,
  kRecommendation,
  kCard,
  // Add new values here.

  kLast,  // Don't use over IPC
};

// Actions for OmniBox zero state suggestion.
enum OmniBoxZeroStateAction {
  // Removes the zero state suggestion.
  kRemoveSuggestion = 0,
  // Appends the suggestion to search box query.
  kAppendSuggestion,
  // kZeroStateActionMax is always last.
  kZeroStateActionMax
};

// Returns OmniBoxZeroStateAction mapped for |button_index|.
ASH_PUBLIC_EXPORT OmniBoxZeroStateAction
GetOmniBoxZeroStateAction(int button_index);

// A tagged range in search result text.
struct ASH_PUBLIC_EXPORT SearchResultTag {
  // Similar to ACMatchClassification::Style, the style values are not
  // mutually exclusive.
  enum Style {
    NONE = 0,
    URL = 1 << 0,
    MATCH = 1 << 1,
    DIM = 1 << 2,
  };

  SearchResultTag();
  SearchResultTag(int styles, uint32_t start, uint32_t end);

  int styles;
  gfx::Range range;
};
using SearchResultTags = std::vector<SearchResultTag>;

// Data representing an action that can be performed on this search result.
// An action could be represented as an icon set or as a blue button with
// a label. Icon set is chosen if label text is empty. Otherwise, a blue
// button with the label text will be used.
struct ASH_PUBLIC_EXPORT SearchResultAction {
  SearchResultAction();
  SearchResultAction(const gfx::ImageSkia& image,
                     const base::string16& tooltip_text,
                     bool visible_on_hover);
  SearchResultAction(const SearchResultAction& other);
  ~SearchResultAction();

  gfx::ImageSkia image;
  base::string16 tooltip_text;
  // Visible when button or its parent row in hover state.
  bool visible_on_hover;
};
using SearchResultActions = std::vector<SearchResultAction>;

}  // namespace ash

#endif  // ASH_PUBLIC_CPP_APP_LIST_APP_LIST_TYPES_H_
