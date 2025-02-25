// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_MAC_STAGING_WATCHER_H_
#define CHROME_COMMON_MAC_STAGING_WATCHER_H_

#import <Foundation/Foundation.h>

// Chrome update works by staging a copy of Chrome near to the current bundle,
// and then applying it when Chrome restarts to apply the update. Currently,
// this state of "update pending" is indicated outside of Keystone by a key in
// the CFPreferences.

using StagingKeyChangedObserver = void (^)(BOOL stagingKeySet);

// An object to observe the staging key. It can be used in either of two ways:
// 1. To wait for the staging key to clear.
// 2. To notify when the staging key changes state.
@interface CrStagingKeyWatcher : NSObject

// On macOS 10.11 and earlier, polling is used, and |pollingTime| specifies the
// frequency of the polling. On macOS 10.12 and later, no polling is performed
// and |pollingTime| is ignored.
- (instancetype)initWithPollingTime:(NSTimeInterval)pollingTime;

// Returns a boolean indicating whether or not the staging key is set.
- (BOOL)isStagingKeySet;

// Sleeps until the staging key is clear. If there is no staging key set,
// returns immediately.
- (void)waitForStagingKeyToClear;

// Sets a block to be called when the staging key changes, and starts observing.
// Only one observer may be set for a given CrStagingKeyWatcher; calling this
// method again replaces the current observer.
- (void)setStagingKeyChangedObserver:(StagingKeyChangedObserver)block;

@end

@interface CrStagingKeyWatcher (TestingInterface)

// The designated initializer. Allows a non-default NSUserDefaults to be
// specified.
- (instancetype)initWithUserDefaults:(NSUserDefaults*)defaults
                         pollingTime:(NSTimeInterval)pollingTime;

// KVO works for NSUserDefaults on 10.12 and later. This method turns off the
// use of KVO to allow the macOS 10.11 and earlier code path to be tested on
// 10.12 and later.
- (void)disableKVOForTesting;

// Returns whether the last call to -waitForStagingKeyToClear blocked or
// returned immediately.
- (BOOL)lastWaitWasBlockedForTesting;

// Returns the NSUserDefaults key that is used to indicate staging. The value to
// be used is an array of strings, each string being a file path to the bundle.
+ (NSString*)stagingKeyForTesting;

@end

#endif  // CHROME_COMMON_MAC_STAGING_WATCHER_H_
