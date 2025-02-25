// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/public/provider/chrome/browser/user/special_user_provider.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

SpecialUserProvider::SpecialUserProvider() {}

SpecialUserProvider::~SpecialUserProvider() {}

bool SpecialUserProvider::IsSpecialUser() {
  return false;
}
