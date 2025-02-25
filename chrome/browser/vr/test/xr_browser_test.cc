// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>

#include "base/base_paths.h"
#include "base/bind.h"
#include "base/callback.h"
#include "base/command_line.h"
#include "base/debug/debugger.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/vr/test/xr_browser_test.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test_utils.h"
#include "url/gurl.h"

namespace vr {

constexpr base::TimeDelta XrBrowserTestBase::kPollCheckIntervalShort;
constexpr base::TimeDelta XrBrowserTestBase::kPollCheckIntervalLong;
constexpr base::TimeDelta XrBrowserTestBase::kPollTimeoutShort;
constexpr base::TimeDelta XrBrowserTestBase::kPollTimeoutMedium;
constexpr base::TimeDelta XrBrowserTestBase::kPollTimeoutLong;
constexpr char XrBrowserTestBase::kVrOverrideEnvVar[];
constexpr char XrBrowserTestBase::kVrOverrideVal[];
constexpr char XrBrowserTestBase::kVrConfigPathEnvVar[];
constexpr char XrBrowserTestBase::kVrConfigPathVal[];
constexpr char XrBrowserTestBase::kVrLogPathEnvVar[];
constexpr char XrBrowserTestBase::kVrLogPathVal[];
constexpr char XrBrowserTestBase::kTestFileDir[];
const std::vector<std::string> XrBrowserTestBase::kRequiredTestSwitches{
    "enable-gpu", "enable-pixel-output-in-tests"};
const std::vector<std::pair<std::string, std::string>>
    XrBrowserTestBase::kRequiredTestSwitchesWithValues{
        std::pair<std::string, std::string>("test-launcher-jobs", "1")};

XrBrowserTestBase::XrBrowserTestBase() : env_(base::Environment::Create()) {}

XrBrowserTestBase::~XrBrowserTestBase() = default;

base::FilePath::StringType UTF8ToWideIfNecessary(std::string input) {
#ifdef OS_WIN
  return base::UTF8ToWide(input);
#else
  return input;
#endif  // OS_WIN
}

std::string WideToUTF8IfNecessary(base::FilePath::StringType input) {
#ifdef OS_WIN
  return base::WideToUTF8(input);
#else
  return input;
#endif  // OS_Win
}

// Returns an std::string consisting of the given path relative to the test
// executable's path, e.g. if the executable is in out/Debug and the given path
// is "test", the returned string should be out/Debug/test.
std::string MakeExecutableRelative(const char* path) {
  base::FilePath executable_path;
  EXPECT_TRUE(
      base::PathService::Get(base::BasePathKey::FILE_EXE, &executable_path));
  executable_path = executable_path.DirName();
  // We need an std::string that is an absolute file path, which requires
  // platform-specific logic since Windows uses std::wstring instead of
  // std::string for FilePaths, but SetVar only accepts std::string.
  return WideToUTF8IfNecessary(
      base::MakeAbsoluteFilePath(
          executable_path.Append(base::FilePath(UTF8ToWideIfNecessary(path))))
          .value());
}

void XrBrowserTestBase::SetUp() {
  // Check whether the required flags were passed to the test - without these,
  // we can fail in ways that are non-obvious, so fail more explicitly here if
  // they aren't present.
  auto* cmd_line = base::CommandLine::ForCurrentProcess();
  for (auto req_switch : kRequiredTestSwitches) {
    ASSERT_TRUE(cmd_line->HasSwitch(req_switch))
        << "Missing switch " << req_switch << " required to run tests properly";
  }
  for (auto req_switch_pair : kRequiredTestSwitchesWithValues) {
    ASSERT_TRUE(cmd_line->HasSwitch(req_switch_pair.first))
        << "Missing switch " << req_switch_pair.first
        << " required to run tests properly";
    ASSERT_TRUE(cmd_line->GetSwitchValueASCII(req_switch_pair.first) ==
                req_switch_pair.second)
        << "Have required switch " << req_switch_pair.first
        << ", but not required value " << req_switch_pair.second;
  }

  // Set the environment variable to use the mock OpenVR client.
  ASSERT_TRUE(
      env_->SetVar(kVrOverrideEnvVar, MakeExecutableRelative(kVrOverrideVal)))
      << "Failed to set OpenVR mock client location environment variable";
  ASSERT_TRUE(env_->SetVar(kVrConfigPathEnvVar,
                           MakeExecutableRelative(kVrConfigPathVal)))
      << "Failed to set OpenVR config location environment variable";
  ASSERT_TRUE(
      env_->SetVar(kVrLogPathEnvVar, MakeExecutableRelative(kVrLogPathVal)))
      << "Failed to set OpenVR log location environment variable";

  // Set any command line flags that subclasses have set, e.g. enabling WebVR
  // and OpenVR support.
  for (const auto& switch_string : append_switches_) {
    cmd_line->AppendSwitch(switch_string);
  }
  scoped_feature_list_.InitWithFeatures(enable_features_, disable_features_);

  InProcessBrowserTest::SetUp();
}

GURL XrBrowserTestBase::GetFileUrlForHtmlTestFile(
    const std::string& test_name) {
  return ui_test_utils::GetTestUrl(
      base::FilePath(FILE_PATH_LITERAL("xr/e2e_test_files/html")),
      base::FilePath(UTF8ToWideIfNecessary(test_name + ".html")));
}

GURL XrBrowserTestBase::GetEmbeddedServerUrlForHtmlTestFile(
    const std::string& test_name) {
  // GetURL requires that the path start with /.
  return GetEmbeddedServer()->GetURL(std::string("/") + kTestFileDir +
                                     test_name + ".html");
}

net::EmbeddedTestServer* XrBrowserTestBase::GetEmbeddedServer() {
  if (server_ == nullptr) {
    server_ = std::make_unique<net::EmbeddedTestServer>(
        net::EmbeddedTestServer::Type::TYPE_HTTPS);
    // We need to serve from the root in order for the inclusion of the
    // test harness from //third_party to work.
    server_->ServeFilesFromSourceDirectory(".");
    EXPECT_TRUE(server_->Start()) << "Failed to start embedded test server";
  }
  return server_.get();
}

content::WebContents* XrBrowserTestBase::GetCurrentWebContents() {
  return browser()->tab_strip_model()->GetActiveWebContents();
}

void XrBrowserTestBase::LoadUrlAndAwaitInitialization(const GURL& url) {
  ui_test_utils::NavigateToURL(browser(), url);
  ASSERT_TRUE(PollJavaScriptBoolean("isInitializationComplete()",
                                    kPollTimeoutMedium,
                                    GetCurrentWebContents()))
      << "Timed out waiting for JavaScript test initialization.";
}

void XrBrowserTestBase::RunJavaScriptOrFail(
    const std::string& js_expression,
    content::WebContents* web_contents) {
  ASSERT_TRUE(content::ExecuteScript(web_contents, js_expression))
      << "Failed to run given JavaScript: " << js_expression;
}

bool XrBrowserTestBase::RunJavaScriptAndExtractBoolOrFail(
    const std::string& js_expression,
    content::WebContents* web_contents) {
  bool result;
  DLOG(ERROR) << "Run JavaScript: " << js_expression;
  EXPECT_TRUE(content::ExecuteScriptAndExtractBool(
      web_contents,
      "window.domAutomationController.send(" + js_expression + ")", &result))
      << "Failed to run given JavaScript for bool: " << js_expression;
  return result;
}

std::string XrBrowserTestBase::RunJavaScriptAndExtractStringOrFail(
    const std::string& js_expression,
    content::WebContents* web_contents) {
  std::string result;
  EXPECT_TRUE(content::ExecuteScriptAndExtractString(
      web_contents,
      "window.domAutomationController.send(" + js_expression + ")", &result))
      << "Failed to run given JavaScript for string: " << js_expression;
  return result;
}

bool XrBrowserTestBase::PollJavaScriptBoolean(
    const std::string& bool_expression,
    const base::TimeDelta& timeout,
    content::WebContents* web_contents) {
  bool result = false;
  base::RunLoop wait_loop(base::RunLoop::Type::kNestableTasksAllowed);
  // Lambda used because otherwise BindRepeating gets confused about which
  // version of RunJavaScriptAndExtractBoolOrFail to use.
  BlockOnCondition(base::BindRepeating(
                       [](XrBrowserTestBase* base, std::string expression,
                          content::WebContents* contents) {
                         return base->RunJavaScriptAndExtractBoolOrFail(
                             expression, contents);
                       },
                       this, bool_expression, web_contents),
                   &result, &wait_loop, base::Time::Now(), timeout);
  wait_loop.Run();
  return result;
}

void XrBrowserTestBase::PollJavaScriptBooleanOrFail(
    const std::string& bool_expression,
    const base::TimeDelta& timeout,
    content::WebContents* web_contents) {
  ASSERT_TRUE(PollJavaScriptBoolean(bool_expression, timeout, web_contents))
      << "Timed out polling JavaScript boolean expression: " << bool_expression;
}

void XrBrowserTestBase::BlockOnCondition(
    base::RepeatingCallback<bool()> condition,
    bool* result,
    base::RunLoop* wait_loop,
    const base::Time& start_time,
    const base::TimeDelta& timeout,
    const base::TimeDelta& period) {
  if (!*result) {
    *result = condition.Run();
  }

  if (*result) {
    if (wait_loop->running()) {
      wait_loop->Quit();
      return;
    }
    // In the case where the condition is met fast enough that the given
    // RunLoop hasn't started yet, spin until it's available.
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::BindOnce(&XrBrowserTestBase::BlockOnCondition,
                       base::Unretained(this), std::move(condition),
                       base::Unretained(result), base::Unretained(wait_loop),
                       start_time, timeout, period));
    return;
  }

  if (base::Time::Now() - start_time > timeout &&
      !base::debug::BeingDebugged()) {
    wait_loop->Quit();
    return;
  }

  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&XrBrowserTestBase::BlockOnCondition,
                     base::Unretained(this), std::move(condition),
                     base::Unretained(result), base::Unretained(wait_loop),
                     start_time, timeout, period),
      period);
}

void XrBrowserTestBase::WaitOnJavaScriptStep(
    content::WebContents* web_contents) {
  // Make sure we aren't trying to wait on a JavaScript test step without the
  // code to do so.
  bool code_available = RunJavaScriptAndExtractBoolOrFail(
      "typeof javascriptDone !== 'undefined'", web_contents);
  ASSERT_TRUE(code_available) << "Attempted to wait on a JavaScript test step "
                              << "without the code to do so. You either forgot "
                              << "to import webxr_e2e.js or "
                              << "are incorrectly using a C++ function.";

  // Actually wait for the step to finish.
  bool success =
      PollJavaScriptBoolean("javascriptDone", kPollTimeoutLong, web_contents);

  // Check what state we're in to make sure javascriptDone wasn't called
  // because the test failed.
  XrBrowserTestBase::TestStatus test_status = CheckTestStatus(web_contents);
  if (!success || test_status == XrBrowserTestBase::TestStatus::STATUS_FAILED) {
    // Failure states: Either polling failed or polling succeeded, but because
    // the test failed.
    std::string reason;
    if (!success) {
      reason = "Timed out waiting for JavaScript step to finish.";
    } else {
      reason =
          "JavaScript testharness reported failure while waiting for "
          "JavaScript step to finish";
    }

    std::string result_string =
        RunJavaScriptAndExtractStringOrFail("resultString", web_contents);
    if (result_string.empty()) {
      reason +=
          " Did not obtain specific failure reason from JavaScript "
          "testharness.";
    } else {
      reason +=
          " JavaScript testharness reported failure reason: " + result_string;
    }
    FAIL() << reason;
  }

  // Reset the synchronization boolean.
  RunJavaScriptOrFail("javascriptDone = false", web_contents);
}

void XrBrowserTestBase::ExecuteStepAndWait(const std::string& step_function,
                                           content::WebContents* web_contents) {
  RunJavaScriptOrFail(step_function, web_contents);
  WaitOnJavaScriptStep(web_contents);
}

XrBrowserTestBase::TestStatus XrBrowserTestBase::CheckTestStatus(
    content::WebContents* web_contents) {
  std::string result_string =
      RunJavaScriptAndExtractStringOrFail("resultString", web_contents);
  bool test_passed =
      RunJavaScriptAndExtractBoolOrFail("testPassed", web_contents);
  if (test_passed) {
    return XrBrowserTestBase::TestStatus::STATUS_PASSED;
  } else if (!test_passed && result_string.empty()) {
    return XrBrowserTestBase::TestStatus::STATUS_RUNNING;
  }
  // !test_passed && result_string != ""
  return XrBrowserTestBase::TestStatus::STATUS_FAILED;
}

void XrBrowserTestBase::EndTest(content::WebContents* web_contents) {
  switch (CheckTestStatus(web_contents)) {
    case XrBrowserTestBase::TestStatus::STATUS_PASSED:
      break;
    case XrBrowserTestBase::TestStatus::STATUS_FAILED:
      FAIL() << "JavaScript testharness failed with reason: "
             << RunJavaScriptAndExtractStringOrFail("resultString",
                                                    web_contents);
      break;
    case XrBrowserTestBase::TestStatus::STATUS_RUNNING:
      FAIL() << "Attempted to end test in C++ without finishing in JavaScript.";
      break;
    default:
      FAIL() << "Received unknown test status.";
  }
}

void XrBrowserTestBase::AssertNoJavaScriptErrors(
    content::WebContents* web_contents) {
  if (CheckTestStatus(web_contents) ==
      XrBrowserTestBase::TestStatus::STATUS_FAILED) {
    FAIL() << "JavaScript testharness failed with reason: "
           << RunJavaScriptAndExtractStringOrFail("resultString", web_contents);
  }
}

void XrBrowserTestBase::RunJavaScriptOrFail(const std::string& js_expression) {
  RunJavaScriptOrFail(js_expression, GetCurrentWebContents());
}

bool XrBrowserTestBase::RunJavaScriptAndExtractBoolOrFail(
    const std::string& js_expression) {
  return RunJavaScriptAndExtractBoolOrFail(js_expression,
                                           GetCurrentWebContents());
}

std::string XrBrowserTestBase::RunJavaScriptAndExtractStringOrFail(
    const std::string& js_expression) {
  return RunJavaScriptAndExtractStringOrFail(js_expression,
                                             GetCurrentWebContents());
}

bool XrBrowserTestBase::PollJavaScriptBoolean(
    const std::string& bool_expression,
    const base::TimeDelta& timeout) {
  return PollJavaScriptBoolean(bool_expression, timeout,
                               GetCurrentWebContents());
}

void XrBrowserTestBase::PollJavaScriptBooleanOrFail(
    const std::string& bool_expression,
    const base::TimeDelta& timeout) {
  PollJavaScriptBooleanOrFail(bool_expression, timeout,
                              GetCurrentWebContents());
}

void XrBrowserTestBase::WaitOnJavaScriptStep() {
  WaitOnJavaScriptStep(GetCurrentWebContents());
}

void XrBrowserTestBase::ExecuteStepAndWait(const std::string& step_function) {
  ExecuteStepAndWait(step_function, GetCurrentWebContents());
}

void XrBrowserTestBase::EndTest() {
  EndTest(GetCurrentWebContents());
}

void XrBrowserTestBase::AssertNoJavaScriptErrors() {
  AssertNoJavaScriptErrors(GetCurrentWebContents());
}

}  // namespace vr
