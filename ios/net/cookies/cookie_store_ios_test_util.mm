// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/net/cookies/cookie_store_ios_test_util.h"

#import <Foundation/Foundation.h>

#include <memory>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/run_loop.h"
#include "base/test/test_simple_task_runner.h"
#include "base/threading/thread_task_runner_handle.h"
#import "ios/net/cookies/cookie_store_ios.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_options.h"
#include "testing/gtest/include/gtest/gtest.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace net {

#pragma mark -
#pragma mark TestPersistentCookieStore

TestPersistentCookieStore::TestPersistentCookieStore()
    : kTestCookieURL("http://foo.google.com/bar"), flushed_(false) {}

TestPersistentCookieStore::~TestPersistentCookieStore() = default;

#pragma mark -
#pragma mark TestPersistentCookieStore methods

void TestPersistentCookieStore::RunLoadedCallback() {
  std::vector<std::unique_ptr<net::CanonicalCookie>> cookies;
  net::CookieOptions options;
  options.set_include_httponly();

  std::unique_ptr<net::CanonicalCookie> cookie(net::CanonicalCookie::Create(
      kTestCookieURL, "a=b", base::Time::Now(), options));
  cookies.push_back(std::move(cookie));

  std::unique_ptr<net::CanonicalCookie> bad_canonical_cookie(
      std::make_unique<net::CanonicalCookie>(
          "name", "\x81r\xe4\xbd\xa0\xe5\xa5\xbd", "domain", "/path/",
          base::Time(),  // creation
          base::Time(),  // expires
          base::Time(),  // last accessed
          false,         // secure
          false,         // httponly
          net::CookieSameSite::DEFAULT_MODE, net::COOKIE_PRIORITY_DEFAULT));
  cookies.push_back(std::move(bad_canonical_cookie));
  std::move(loaded_callback_).Run(std::move(cookies));
}

bool TestPersistentCookieStore::flushed() {
  return flushed_;
}

#pragma mark -
#pragma mark Private methods

void TestPersistentCookieStore::Load(LoadedCallback loaded_callback,
                                     const NetLogWithSource& /* net_log */) {
  loaded_callback_ = std::move(loaded_callback);
}

void TestPersistentCookieStore::LoadCookiesForKey(
    const std::string& key,
    LoadedCallback loaded_callback) {
  loaded_callback_ = std::move(loaded_callback);
}

void TestPersistentCookieStore::AddCookie(const net::CanonicalCookie& cc) {}

void TestPersistentCookieStore::UpdateCookieAccessTime(
    const net::CanonicalCookie& cc) {}

void TestPersistentCookieStore::DeleteCookie(const net::CanonicalCookie& cc) {}

void TestPersistentCookieStore::SetForceKeepSessionState() {}

void TestPersistentCookieStore::SetBeforeCommitCallback(
    base::RepeatingClosure callback) {}

void TestPersistentCookieStore::Flush(base::OnceClosure callback) {
  flushed_ = true;
}

#pragma mark -
#pragma mark TestCookieStoreIOSClient

TestCookieStoreIOSClient::TestCookieStoreIOSClient() {}

#pragma mark -
#pragma mark TestCookieStoreIOSClient methods

scoped_refptr<base::SequencedTaskRunner>
TestCookieStoreIOSClient::GetTaskRunner() const {
  return base::ThreadTaskRunnerHandle::Get();
}

#pragma mark -
#pragma mark ScopedTestingCookieStoreIOSClient

ScopedTestingCookieStoreIOSClient::ScopedTestingCookieStoreIOSClient(
    std::unique_ptr<CookieStoreIOSClient> cookie_store_client)
    : cookie_store_client_(std::move(cookie_store_client)),
      original_client_(GetCookieStoreIOSClient()) {
  SetCookieStoreIOSClient(cookie_store_client_.get());
}

ScopedTestingCookieStoreIOSClient::~ScopedTestingCookieStoreIOSClient() {
  SetCookieStoreIOSClient(original_client_);
}

//------------------------------------------------------------------------------

void RecordCookieChanges(std::vector<net::CanonicalCookie>* out_cookies,
                         std::vector<bool>* out_removes,
                         const net::CanonicalCookie& cookie,
                         net::CookieChangeCause cause) {
  DCHECK(out_cookies);
  out_cookies->push_back(cookie);
  if (out_removes)
    out_removes->push_back(net::CookieChangeCauseIsDeletion(cause));
}

void SetCookie(const std::string& cookie_line,
               const GURL& url,
               net::CookieStore* store) {
  net::CookieOptions options;
  options.set_include_httponly();
  store->SetCookieWithOptionsAsync(url, cookie_line, options,
                                   base::DoNothing());
  net::CookieStoreIOS::NotifySystemCookiesChanged();
  // Wait until the flush is posted.
  base::RunLoop().RunUntilIdle();
}

void ClearCookies() {
  NSHTTPCookieStorage* store = [NSHTTPCookieStorage sharedHTTPCookieStorage];
  [store setCookieAcceptPolicy:NSHTTPCookieAcceptPolicyAlways];
  NSArray* cookies = [store cookies];
  for (NSHTTPCookie* cookie in cookies)
    [store deleteCookie:cookie];
  EXPECT_EQ(0u, [[store cookies] count]);
}

}  // namespace net
