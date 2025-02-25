// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/offline_pages/core/model/offline_page_test_utils.h"

#include "base/files/file_enumerator.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "components/offline_pages/core/offline_page_item.h"
#include "components/offline_pages/core/offline_store_utils.h"

namespace offline_pages {

namespace test_utils {

size_t GetFileCountInDirectory(const base::FilePath& directory) {
  base::FileEnumerator file_enumerator(directory, false,
                                       base::FileEnumerator::FILES);
  size_t count = 0;
  for (base::FilePath path = file_enumerator.Next(); !path.empty();
       path = file_enumerator.Next()) {
    count++;
  }
  return count;
}

}  // namespace test_utils

std::ostream& operator<<(std::ostream& out, const OfflinePageItem& item) {
  using base::Value;
  base::DictionaryValue value;
  value.SetKey("url", Value(item.url.spec()));
  value.SetKey("offline_id", Value(std::to_string(item.offline_id)));
  value.SetKey("client_id", Value(item.client_id.ToString()));
  if (!item.file_path.empty()) {
    value.SetKey("file_path", Value(item.file_path.AsUTF8Unsafe()));
  }
  if (item.file_size != 0) {
    value.SetKey("file_size", Value(std::to_string(item.file_size)));
  }
  if (!item.creation_time.is_null()) {
    value.SetKey(
        "creation_time",
        Value(std::to_string(store_utils::ToDatabaseTime(item.creation_time))));
  }
  if (!item.last_access_time.is_null()) {
    value.SetKey("creation_time",
                 Value(std::to_string(
                     store_utils::ToDatabaseTime(item.last_access_time))));
  }
  if (item.access_count != 0) {
    value.SetKey("access_count", Value(item.access_count));
  }
  if (!item.title.empty()) {
    value.SetKey("title", Value(base::UTF16ToUTF8(item.title)));
  }
  if (item.flags & OfflinePageItem::MARKED_FOR_DELETION) {
    value.SetKey("marked_for_deletion", Value(true));
  }
  if (!item.original_url_if_different.is_empty()) {
    value.SetKey("original_url_if_different",
                 Value(item.original_url_if_different.spec()));
  }
  if (!item.request_origin.empty()) {
    value.SetKey("request_origin", Value(item.request_origin));
  }
  if (item.system_download_id != 0) {
    value.SetKey("system_download_id",
                 Value(std::to_string(item.system_download_id)));
  }
  if (!item.file_missing_time.is_null()) {
    value.SetKey("file_missing_time",
                 Value(std::to_string(
                     store_utils::ToDatabaseTime(item.file_missing_time))));
  }
  if (!item.digest.empty()) {
    value.SetKey("digest", Value(item.digest));
  }

  std::string value_string;
  base::JSONWriter::Write(value, &value_string);
  return out << value_string;
}

}  // namespace offline_pages
