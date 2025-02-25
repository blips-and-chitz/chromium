// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_version.h"

#include <stdint.h>
#include <memory>
#include <tuple>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/run_loop.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/simple_test_tick_clock.h"
#include "base/time/time.h"
#include "content/browser/service_worker/embedded_worker_status.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/fake_embedded_worker_instance_client.h"
#include "content/browser/service_worker/fake_service_worker.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_ping_controller.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/common/service_worker/service_worker_utils.h"
#include "content/public/common/content_features.h"
#include "content/public/test/mock_render_process_host.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_service.mojom.h"
#include "content/public/test/test_utils.h"
#include "mojo/public/cpp/bindings/strong_binding.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/service_worker/service_worker.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_event_status.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_installed_scripts_manager.mojom.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"

namespace content {
namespace service_worker_version_unittest {

base::OnceCallback<void()> VerifyCalled(
    bool* called,
    base::OnceClosure quit_closure = base::OnceClosure()) {
  return base::BindOnce(
      [](bool* called, base::OnceClosure quit_closure) {
        *called = true;
        if (!quit_closure.is_null())
          std::move(quit_closure).Run();
      },
      called, std::move(quit_closure));
}

void ObserveStatusChanges(ServiceWorkerVersion* version,
                          std::vector<ServiceWorkerVersion::Status>* statuses) {
  statuses->push_back(version->status());
  version->RegisterStatusChangeCallback(base::BindOnce(
      &ObserveStatusChanges, base::Unretained(version), statuses));
}

base::Time GetYesterday() {
  return base::Time::Now() - base::TimeDelta::FromDays(1) -
         base::TimeDelta::FromSeconds(1);
}

class TestServiceImpl : public mojom::TestService {
 public:
  static void Create(mojo::InterfaceRequest<mojom::TestService> request) {
    mojo::MakeStrongBinding(base::WrapUnique(new TestServiceImpl),
                            std::move(request));
  }

  void DoSomething(DoSomethingCallback callback) override {
    std::move(callback).Run();
  }

  void DoTerminateProcess(DoTerminateProcessCallback callback) override {
    NOTREACHED();
  }

  void DoCrashImmediately(DoCrashImmediatelyCallback callback) override {
    NOTREACHED();
  }

  void CreateFolder(CreateFolderCallback callback) override { NOTREACHED(); }

  void GetRequestorName(GetRequestorNameCallback callback) override {
    std::move(callback).Run("");
  }

  void CreateSharedBuffer(const std::string& message,
                          CreateSharedBufferCallback callback) override {
    NOTREACHED();
  }

 private:
  TestServiceImpl() {}
};

void StartWorker(ServiceWorkerVersion* version,
                 ServiceWorkerMetrics::EventType purpose) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version->StartWorker(
      purpose, ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  EXPECT_EQ(EmbeddedWorkerStatus::STARTING, version->running_status());
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
}

class ServiceWorkerVersionTest : public testing::Test {
 protected:
  struct RunningStateListener : public ServiceWorkerVersion::Observer {
    RunningStateListener() : last_status(EmbeddedWorkerStatus::STOPPED) {}
    ~RunningStateListener() override {}
    void OnRunningStateChanged(ServiceWorkerVersion* version) override {
      last_status = version->running_status();
    }
    EmbeddedWorkerStatus last_status;
  };

  struct CachedMetadataUpdateListener : public ServiceWorkerVersion::Observer {
    CachedMetadataUpdateListener() = default;
    ~CachedMetadataUpdateListener() override = default;
    void OnCachedMetadataUpdated(ServiceWorkerVersion* version,
                                 size_t size) override {
      ++updated_count;
    }
    int updated_count = 0;
  };

  ServiceWorkerVersionTest()
      : thread_bundle_(TestBrowserThreadBundle::IO_MAINLOOP) {}

  void SetUp() override {
    helper_ = GetHelper();
    helper_->context()->storage()->LazyInitializeForTest(base::DoNothing());
    base::RunLoop().RunUntilIdle();

    scope_ = GURL("https://www.example.com/test/");
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = scope_;
    registration_ = new ServiceWorkerRegistration(
        options, helper_->context()->storage()->NewRegistrationId(),
        helper_->context()->AsWeakPtr());
    version_ = new ServiceWorkerVersion(
        registration_.get(),
        GURL("https://www.example.com/test/service_worker.js"),
        blink::mojom::ScriptType::kClassic,
        helper_->context()->storage()->NewVersionId(),
        helper_->context()->AsWeakPtr());
    EXPECT_EQ(url::Origin::Create(scope_), version_->script_origin());
    std::vector<ServiceWorkerDatabase::ResourceRecord> records;
    records.push_back(WriteToDiskCacheSync(
        helper_->context()->storage(), version_->script_url(), 10,
        {} /* headers */, "I'm a body", "I'm a meta data"));
    version_->script_cache_map()->SetResources(records);
    version_->SetMainScriptHttpResponseInfo(
        EmbeddedWorkerTestHelper::CreateHttpResponseInfo());
    version_->set_fetch_handler_existence(GetFetchHandlerExistence());

    // Make the registration findable via storage functions.
    base::Optional<blink::ServiceWorkerStatusCode> status;
    base::RunLoop run_loop;
    helper_->context()->storage()->StoreRegistration(
        registration_.get(), version_.get(),
        ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
    run_loop.Run();
    ASSERT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  }

  virtual std::unique_ptr<EmbeddedWorkerTestHelper> GetHelper() {
    return std::make_unique<EmbeddedWorkerTestHelper>(base::FilePath());
  }

  void TearDown() override {
    version_ = nullptr;
    registration_ = nullptr;
    helper_.reset();
  }

  bool IsPingActivated(ServiceWorkerVersion* version) const {
    return version->ping_controller_.IsActivated();
  }

  void NotifyScriptEvaluationStart(ServiceWorkerVersion* version) {
    version->OnScriptEvaluationStart();
  }

  void SimulateDispatchEvent(ServiceWorkerMetrics::EventType event_type) {
    base::Optional<blink::ServiceWorkerStatusCode> status;
    base::RunLoop run_loop;

    // Make sure worker is running.
    version_->RunAfterStartWorker(
        event_type,
        ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
    run_loop.Run();
    EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
    EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

    // Start request, as if an event is being dispatched.
    int request_id = version_->StartRequest(event_type, base::DoNothing());
    base::RunLoop().RunUntilIdle();

    // And finish request, as if a response to the event was received.
    EXPECT_TRUE(version_->FinishRequest(request_id, true /* was_handled */));
  }

  void SetTickClockForTesting(base::SimpleTestTickClock* tick_clock) {
    version_->SetTickClockForTesting(tick_clock);
  }

  virtual ServiceWorkerVersion::FetchHandlerExistence GetFetchHandlerExistence()
      const {
    return ServiceWorkerVersion::FetchHandlerExistence::EXISTS;
  }

  ServiceWorkerRemoteProviderEndpoint ActivateWithControllee(
      int controllee_process_id = 33) {
    version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
    registration_->SetActiveVersion(version_);
    ServiceWorkerRemoteProviderEndpoint remote_endpoint;
    base::WeakPtr<ServiceWorkerProviderHost> host = CreateProviderHostForWindow(
        controllee_process_id, true /* is_parent_frame_secure */,
        helper_->context()->AsWeakPtr(), &remote_endpoint);
    host->UpdateUrls(registration_->scope(), registration_->scope());
    host->SetControllerRegistration(registration_,
                                    false /* notify_controllerchange */);
    EXPECT_TRUE(version_->HasControllee());
    EXPECT_TRUE(host->controller());
    return remote_endpoint;
  }

  TestBrowserThreadBundle thread_bundle_;
  std::unique_ptr<EmbeddedWorkerTestHelper> helper_;
  scoped_refptr<ServiceWorkerRegistration> registration_;
  scoped_refptr<ServiceWorkerVersion> version_;
  GURL scope_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ServiceWorkerVersionTest);
};

// An instance client that breaks the Mojo connection upon receiving the
// Start() message.
class FailStartInstanceClient : public FakeEmbeddedWorkerInstanceClient {
 public:
  FailStartInstanceClient(EmbeddedWorkerTestHelper* helper)
      : FakeEmbeddedWorkerInstanceClient(helper) {}

  void StartWorker(blink::mojom::EmbeddedWorkerStartParamsPtr params) override {
    // Don't save the Mojo ptrs. The connection breaks.
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(FailStartInstanceClient);
};

TEST_F(ServiceWorkerVersionTest, ConcurrentStartAndStop) {
  // Call StartWorker() multiple times.
  base::Optional<blink::ServiceWorkerStatusCode> status1;
  base::Optional<blink::ServiceWorkerStatusCode> status2;
  base::Optional<blink::ServiceWorkerStatusCode> status3;
  base::RunLoop run_loop_1;
  base::RunLoop run_loop_2;
  base::RunLoop run_loop_3;

  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status1, run_loop_1.QuitClosure()));
  EXPECT_EQ(EmbeddedWorkerStatus::STARTING, version_->running_status());
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status2, run_loop_2.QuitClosure()));
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

  // Call StartWorker() after it's started.
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status3, run_loop_3.QuitClosure()));

  run_loop_1.Run();
  run_loop_2.Run();
  run_loop_3.Run();

  // All should just succeed.
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status1.value());
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status2.value());
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status3.value());

  {
    // Call StopWorker() multiple times.
    bool has_stopped1 = false;
    bool has_stopped2 = false;
    base::RunLoop run_loop_1;
    base::RunLoop run_loop_2;

    version_->StopWorker(VerifyCalled(&has_stopped1, run_loop_1.QuitClosure()));
    version_->StopWorker(VerifyCalled(&has_stopped2, run_loop_2.QuitClosure()));

    EXPECT_EQ(EmbeddedWorkerStatus::STOPPING, version_->running_status());
    run_loop_1.Run();
    run_loop_2.Run();
    EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());

    // All StopWorker should just succeed.
    EXPECT_TRUE(has_stopped1);
    EXPECT_TRUE(has_stopped2);
  }

  // Start worker again.
  status1.reset();
  status2.reset();

  base::RunLoop run_loop_4;
  base::RunLoop run_loop_5;

  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status1, run_loop_4.QuitClosure()));

  EXPECT_EQ(EmbeddedWorkerStatus::STARTING, version_->running_status());
  run_loop_4.Run();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

  {
    // Call StopWorker()
    bool has_stopped = false;
    version_->StopWorker(VerifyCalled(&has_stopped));

    // And try calling StartWorker while StopWorker is in queue.
    version_->StartWorker(
        ServiceWorkerMetrics::EventType::UNKNOWN,
        ReceiveServiceWorkerStatus(&status2, run_loop_5.QuitClosure()));

    EXPECT_EQ(EmbeddedWorkerStatus::STOPPING, version_->running_status());
    run_loop_5.Run();
    EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

    // All should just succeed.
    EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status1.value());
    EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status2.value());
    EXPECT_TRUE(has_stopped);
  }
}

TEST_F(ServiceWorkerVersionTest, DispatchEventToStoppedWorker) {
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());

  // Dispatch an event without starting the worker.
  version_->SetStatus(ServiceWorkerVersion::INSTALLING);
  EXPECT_TRUE(version_->HasNoWork());
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::INSTALL);

  // The worker may still be handling events dispatched directly from
  // controllees. We cannot say the version doesn't handle any tasks until the
  // worker reports "No Work" (= ServiceWorkerVersion::OnRequestTermination()
  // is called).
  EXPECT_FALSE(version_->HasNoWork());

  // The worker should be now started.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

  // Stop the worker, and then dispatch an event immediately after that.
  bool has_stopped = false;
  version_->StopWorker(VerifyCalled(&has_stopped));
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::INSTALL);
  EXPECT_TRUE(has_stopped);

  // The worker may still be handling events dispatched directly from
  // controllees. We cannot say the version doesn't handle any tasks until the
  // worker reports "No Work" (= ServiceWorkerVersion::OnRequestTermination()
  // is called).
  EXPECT_FALSE(version_->HasNoWork());

  // The worker should be now started again.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, StartUnregisteredButStillLiveWorker) {
  // Start the worker.
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::UNKNOWN);
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);

  // Delete the registration.
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  helper_->context()->storage()->DeleteRegistration(
      registration_->id(), registration_->scope().GetOrigin(),
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  run_loop.Run();
  ASSERT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());

  // The live registration is marked as deleted, but still exists.
  ASSERT_TRUE(registration_->is_deleted());

  {
    // Stop the worker.
    bool has_stopped = false;
    base::RunLoop run_loop;
    version_->StopWorker(VerifyCalled(&has_stopped, run_loop.QuitClosure()));
    run_loop.Run();
    EXPECT_TRUE(has_stopped);
  }

  // Dispatch an event on the unregistered and stopped but still live worker.
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME);

  // The worker should be now started again.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, InstallAndWaitCompletion) {
  version_->SetStatus(ServiceWorkerVersion::INSTALLING);

  // Wait for the completion.
  bool status_change_called = false;
  version_->RegisterStatusChangeCallback(VerifyCalled(&status_change_called));

  // Dispatch an install event.
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::INSTALL);

  // Version's status must not have changed during installation.
  EXPECT_FALSE(status_change_called);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLING, version_->status());
}

TEST_F(ServiceWorkerVersionTest, ActivateAndWaitCompletion) {
  // TODO(mek): This test (and the one above for the install event) made more
  // sense back when ServiceWorkerVersion was responsible for updating the
  // status. Now a better version of this test should probably be added to
  // ServiceWorkerRegistrationTest instead.

  version_->SetStatus(ServiceWorkerVersion::ACTIVATING);

  // Wait for the completion.
  bool status_change_called = false;
  version_->RegisterStatusChangeCallback(VerifyCalled(&status_change_called));

  // Dispatch an activate event.
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::ACTIVATE);

  // Version's status must not have changed during activation.
  EXPECT_FALSE(status_change_called);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, version_->status());
}

TEST_F(ServiceWorkerVersionTest, RepeatedlyObserveStatusChanges) {
  EXPECT_EQ(ServiceWorkerVersion::NEW, version_->status());

  // Repeatedly observe status changes (the callback re-registers itself).
  std::vector<ServiceWorkerVersion::Status> statuses;
  version_->RegisterStatusChangeCallback(base::BindOnce(
      &ObserveStatusChanges, base::RetainedRef(version_), &statuses));

  version_->SetStatus(ServiceWorkerVersion::INSTALLING);
  version_->SetStatus(ServiceWorkerVersion::INSTALLED);
  version_->SetStatus(ServiceWorkerVersion::ACTIVATING);
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  version_->SetStatus(ServiceWorkerVersion::REDUNDANT);

  // Verify that we could successfully observe repeated status changes.
  ASSERT_EQ(5U, statuses.size());
  ASSERT_EQ(ServiceWorkerVersion::INSTALLING, statuses[0]);
  ASSERT_EQ(ServiceWorkerVersion::INSTALLED, statuses[1]);
  ASSERT_EQ(ServiceWorkerVersion::ACTIVATING, statuses[2]);
  ASSERT_EQ(ServiceWorkerVersion::ACTIVATED, statuses[3]);
  ASSERT_EQ(ServiceWorkerVersion::REDUNDANT, statuses[4]);
}

TEST_F(ServiceWorkerVersionTest, Doom) {
  // Add a controllee.
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  registration_->SetActiveVersion(version_);
  ServiceWorkerRemoteProviderEndpoint remote_endpoint;
  base::WeakPtr<ServiceWorkerProviderHost> host = CreateProviderHostForWindow(
      33 /* dummy render process id */, true /* is_parent_frame_secure */,
      helper_->context()->AsWeakPtr(), &remote_endpoint);
  host->UpdateUrls(registration_->scope(), registration_->scope());
  host->SetControllerRegistration(registration_, false);
  EXPECT_TRUE(version_->HasControllee());
  EXPECT_TRUE(host->controller());

  // Doom the version.
  version_->Doom();

  // The controllee should have been removed.
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version_->status());
  EXPECT_FALSE(version_->HasControllee());
  EXPECT_FALSE(host->controller());
}

TEST_F(ServiceWorkerVersionTest, SetDevToolsAttached) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));

  ASSERT_EQ(EmbeddedWorkerStatus::STARTING, version_->running_status());

  ASSERT_TRUE(version_->timeout_timer_.IsRunning());
  ASSERT_FALSE(version_->start_time_.is_null());
  ASSERT_FALSE(version_->skip_recording_startup_time_);

  // Simulate DevTools is attached. This should deactivate the timer for start
  // timeout, but not stop the timer itself.
  version_->SetDevToolsAttached(true);
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  EXPECT_TRUE(version_->start_time_.is_null());
  EXPECT_TRUE(version_->skip_recording_startup_time_);

  // Simulate DevTools is detached. This should reactivate the timer for start
  // timeout.
  version_->SetDevToolsAttached(false);
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  EXPECT_FALSE(version_->start_time_.is_null());
  EXPECT_TRUE(version_->skip_recording_startup_time_);

  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());
}

// Test that update isn't triggered for a non-stale worker.
TEST_F(ServiceWorkerVersionTest, StaleUpdate_FreshWorker) {
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  registration_->SetActiveVersion(version_);
  registration_->set_last_update_check(base::Time::Now());
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::PUSH);

  EXPECT_TRUE(version_->stale_time_.is_null());
  EXPECT_FALSE(version_->update_timer_.IsRunning());
}

// Test that update isn't triggered for a non-active worker.
TEST_F(ServiceWorkerVersionTest, StaleUpdate_NonActiveWorker) {
  version_->SetStatus(ServiceWorkerVersion::INSTALLING);
  registration_->SetInstallingVersion(version_);
  registration_->set_last_update_check(GetYesterday());
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::INSTALL);

  EXPECT_TRUE(version_->stale_time_.is_null());
  EXPECT_FALSE(version_->update_timer_.IsRunning());
}

// Test that staleness is detected when starting a worker.
TEST_F(ServiceWorkerVersionTest, StaleUpdate_StartWorker) {
  // Starting the worker marks it as stale.
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  registration_->SetActiveVersion(version_);
  registration_->set_last_update_check(GetYesterday());
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::PUSH);
  EXPECT_FALSE(version_->stale_time_.is_null());
  EXPECT_FALSE(version_->update_timer_.IsRunning());

  // Update is actually scheduled after the worker stops.
  bool has_stopped = false;
  base::RunLoop run_loop;
  version_->StopWorker(VerifyCalled(&has_stopped, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_TRUE(has_stopped);
  EXPECT_TRUE(version_->stale_time_.is_null());
  EXPECT_TRUE(version_->update_timer_.IsRunning());
}

// Test that staleness is detected on a running worker.
TEST_F(ServiceWorkerVersionTest, StaleUpdate_RunningWorker) {
  // Start a fresh worker.
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  registration_->SetActiveVersion(version_);
  registration_->set_last_update_check(base::Time::Now());
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::PUSH);
  EXPECT_TRUE(version_->stale_time_.is_null());

  // Simulate it running for a day. It will be marked stale.
  registration_->set_last_update_check(GetYesterday());
  version_->OnTimeoutTimer();
  EXPECT_FALSE(version_->stale_time_.is_null());
  EXPECT_FALSE(version_->update_timer_.IsRunning());

  // Simulate it running for past the wait threshold. The update will be
  // scheduled.
  version_->stale_time_ = base::TimeTicks::Now() -
                          ServiceWorkerVersion::kStartNewWorkerTimeout -
                          base::TimeDelta::FromMinutes(1);
  version_->OnTimeoutTimer();
  EXPECT_TRUE(version_->stale_time_.is_null());
  EXPECT_TRUE(version_->update_timer_.IsRunning());
}

// Test that a stream of events doesn't restart the timer.
TEST_F(ServiceWorkerVersionTest, StaleUpdate_DoNotDeferTimer) {
  // Make a stale worker.
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  registration_->SetActiveVersion(version_);
  registration_->set_last_update_check(GetYesterday());
  base::TimeTicks stale_time = base::TimeTicks::Now() -
                               ServiceWorkerVersion::kStartNewWorkerTimeout -
                               base::TimeDelta::FromMinutes(1);
  version_->stale_time_ = stale_time;

  // Stale time is not deferred.
  version_->RunAfterStartWorker(ServiceWorkerMetrics::EventType::UNKNOWN,
                                base::DoNothing());
  version_->RunAfterStartWorker(ServiceWorkerMetrics::EventType::UNKNOWN,
                                base::DoNothing());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(stale_time, version_->stale_time_);

  // Timeout triggers the update.
  version_->OnTimeoutTimer();
  EXPECT_TRUE(version_->stale_time_.is_null());
  EXPECT_TRUE(version_->update_timer_.IsRunning());

  // Update timer is not deferred.
  base::TimeTicks run_time = version_->update_timer_.desired_run_time();
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::PUSH);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::PUSH);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::PUSH);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(version_->stale_time_.is_null());
  EXPECT_EQ(run_time, version_->update_timer_.desired_run_time());
}

TEST_F(ServiceWorkerVersionTest, StartRequestWithNullContext) {
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::UNKNOWN);
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  version_->context_ = nullptr;
  version_->StartRequest(ServiceWorkerMetrics::EventType::PUSH,
                         base::DoNothing());
  // Test passes if it doesn't crash.
}

// Tests the delay mechanism for self-updating service workers, to prevent
// them from running forever (see https://crbug.com/805496).
TEST_F(ServiceWorkerVersionTest, ResetUpdateDelay) {
  const base::TimeDelta kMinute = base::TimeDelta::FromMinutes(1);
  const base::TimeDelta kNoDelay = base::TimeDelta();

  // Initialize the delay.
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  registration_->SetActiveVersion(version_);
  registration_->set_self_update_delay(kMinute);

  // Events that can be triggered by a worker should not reset the delay.
  // See the comment in ServiceWorkerVersion::StartRequestWithCustomTimeout.
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::INSTALL);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::ACTIVATE);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::MESSAGE);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(kMinute, registration_->self_update_delay());

  // Events that can only be triggered externally reset the delay.
  // Repeat the test for several such events.
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::SYNC);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(kNoDelay, registration_->self_update_delay());

  registration_->set_self_update_delay(kMinute);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::NOTIFICATION_CLICK);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(kNoDelay, registration_->self_update_delay());

  registration_->set_self_update_delay(kMinute);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::PUSH);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(kNoDelay, registration_->self_update_delay());
}

TEST_F(ServiceWorkerVersionTest, UpdateCachedMetadata) {
  CachedMetadataUpdateListener listener;
  version_->AddObserver(&listener);
  ASSERT_EQ(0, listener.updated_count);
  auto* service_worker =
      helper_->AddNewPendingServiceWorker<FakeServiceWorker>(helper_.get());
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::UNKNOWN);
  service_worker->RunUntilInitializeGlobalScope();

  // Simulate requesting SetCachedMetadata from the service worker global scope.
  std::vector<uint8_t> data{1, 2, 3};
  service_worker->host()->SetCachedMetadata(version_->script_url(), data);
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(1, listener.updated_count);

  // Simulate requesting ClearCachedMetadata from the service worker global
  // scope.
  service_worker->host()->ClearCachedMetadata(version_->script_url());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(2, listener.updated_count);
  version_->RemoveObserver(&listener);
}

TEST_F(ServiceWorkerVersionTest, RestartWorker) {
  StartWorker(version_.get(),
              ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME);
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  bool has_stopped = false;

  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->StartRequest(
      ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));

  // Restart the worker. The inflight event should have been failed.
  version_->StopWorker(VerifyCalled(&has_stopped));
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPING, version_->running_status());
  run_loop.Run();

  {
    base::Optional<blink::ServiceWorkerStatusCode> status;
    base::RunLoop run_loop;
    version_->StartWorker(
        ServiceWorkerMetrics::EventType::UNKNOWN,
        ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
    run_loop.Run();

    // The worker should have been successfully re-started after stopped.
    EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  }

  // The worker should have been stopped.
  EXPECT_TRUE(has_stopped);
  // All inflight events should have been aborted.
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorFailed, status.value());
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

  // SetAllRequestExpirations() after restarting should not crash since all
  // events should have been removed at this point: crbug.com/791451.
  version_->SetAllRequestExpirations(base::TimeTicks());
}

class DelayMessageWorker : public FakeServiceWorker {
 public:
  explicit DelayMessageWorker(EmbeddedWorkerTestHelper* helper)
      : FakeServiceWorker(helper) {}
  ~DelayMessageWorker() override = default;

  void DispatchExtendableMessageEvent(
      blink::mojom::ExtendableMessageEventPtr event,
      DispatchExtendableMessageEventCallback callback) override {
    event_ = std::move(event);
    callback_ = std::move(callback);
    if (quit_closure_)
      std::move(quit_closure_).Run();
  }

  void AbortMessageEvent() {
    std::move(callback_).Run(blink::mojom::ServiceWorkerEventStatus::ABORTED);
  }

  void RunUntilDispatchMessageEvent() {
    if (event_)
      return;
    base::RunLoop loop;
    quit_closure_ = loop.QuitClosure();
    loop.Run();
  }

 private:
  blink::mojom::ExtendableMessageEventPtr event_;
  DispatchExtendableMessageEventCallback callback_;
  base::OnceClosure quit_closure_;

  DISALLOW_COPY_AND_ASSIGN(DelayMessageWorker);
};

TEST_F(ServiceWorkerVersionTest, RequestTimeout) {
  auto* client = helper_->AddNewPendingInstanceClient<
      DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get());
  auto* worker =
      helper_->AddNewPendingServiceWorker<DelayMessageWorker>(helper_.get());

  base::Optional<blink::ServiceWorkerStatusCode> error_status;
  base::RunLoop run_loop;
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  client->UnblockStartWorker();
  StartWorker(version_.get(),
              ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME);

  // Create a request.
  int request_id = version_->StartRequest(
      ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME,
      ReceiveServiceWorkerStatus(&error_status, run_loop.QuitClosure()));

  // Dispatch a dummy event.
  version_->endpoint()->DispatchExtendableMessageEvent(
      blink::mojom::ExtendableMessageEvent::New(),
      version_->CreateSimpleEventCallback(request_id));
  worker->RunUntilDispatchMessageEvent();

  // Request callback has not completed yet.
  EXPECT_FALSE(error_status);

  // Simulate timeout.
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  version_->SetAllRequestExpirations(base::TimeTicks::Now());
  version_->timeout_timer_.user_task().Run();

  // The renderer should have received a StopWorker request.
  client->RunUntilStopWorker();

  // The request should have timed out.
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout,
            error_status.value());
  // Calling FinishRequest should be no-op, since the request timed out.
  EXPECT_FALSE(version_->FinishRequest(request_id, true /* was_handled */));

  // Simulate the renderer aborting the inflight event.
  // This should not crash: https://crbug.com/676984.
  worker->AbortMessageEvent();
  base::RunLoop().RunUntilIdle();

  // Simulate the renderer stopping the worker.
  client->UnblockStopWorker();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, RequestNowTimeout) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::SYNC);

  // Create a request that should expire Now().
  int request_id = version_->StartRequestWithCustomTimeout(
      ServiceWorkerMetrics::EventType::SYNC,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()),
      base::TimeDelta(), ServiceWorkerVersion::CONTINUE_ON_TIMEOUT);

  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  version_->timeout_timer_.user_task().Run();
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout, status.value());

  EXPECT_FALSE(version_->FinishRequest(request_id, true /* was_handled */));

  // CONTINUE_ON_TIMEOUT timeouts don't stop the service worker.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, RequestNowTimeoutKill) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::SYNC);

  // Create a request that should expire Now().
  int request_id = version_->StartRequestWithCustomTimeout(
      ServiceWorkerMetrics::EventType::SYNC,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()),
      base::TimeDelta(), ServiceWorkerVersion::KILL_ON_TIMEOUT);

  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  version_->timeout_timer_.user_task().Run();
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout, status.value());

  EXPECT_FALSE(version_->FinishRequest(request_id, true /* was_handled */));

  // KILL_ON_TIMEOUT timeouts should stop the service worker.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, RequestCustomizedTimeout) {
  base::Optional<blink::ServiceWorkerStatusCode> first_status;
  base::Optional<blink::ServiceWorkerStatusCode> second_status;
  base::RunLoop first_run_loop;
  base::RunLoop second_run_loop;

  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::SYNC);

  base::SimpleTestTickClock tick_clock;
  SetTickClockForTesting(&tick_clock);

  // Create two requests. One which times out in 10 seconds, one in 20 seconds.
  int timeout_seconds = 10;
  int first_request_id = version_->StartRequestWithCustomTimeout(
      ServiceWorkerMetrics::EventType::SYNC,
      ReceiveServiceWorkerStatus(&first_status, first_run_loop.QuitClosure()),
      base::TimeDelta::FromSeconds(2 * timeout_seconds),
      ServiceWorkerVersion::KILL_ON_TIMEOUT);

  int second_request_id = version_->StartRequestWithCustomTimeout(
      ServiceWorkerMetrics::EventType::SYNC,
      ReceiveServiceWorkerStatus(&second_status, second_run_loop.QuitClosure()),
      base::TimeDelta::FromSeconds(timeout_seconds),
      ServiceWorkerVersion::CONTINUE_ON_TIMEOUT);

  // The status should not have changed since neither task has timed out yet.
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  version_->timeout_timer_.user_task().Run();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(first_status);
  EXPECT_FALSE(second_status);

  // Now advance time until the second task timeout should expire.
  tick_clock.Advance(base::TimeDelta::FromSeconds(timeout_seconds + 1));
  version_->timeout_timer_.user_task().Run();
  second_run_loop.Run();
  EXPECT_FALSE(first_status);
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout,
            second_status.value());

  // CONTINUE_ON_TIMEOUT timeouts don't stop the service worker.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

  // Now advance time until both tasks should be expired.
  tick_clock.Advance(base::TimeDelta::FromSeconds(timeout_seconds + 1));
  version_->timeout_timer_.user_task().Run();
  first_run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout,
            first_status.value());
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout,
            second_status.value());

  EXPECT_FALSE(
      version_->FinishRequest(first_request_id, true /* was_handled */));

  EXPECT_FALSE(
      version_->FinishRequest(second_request_id, true /* was_handled */));
  base::RunLoop().RunUntilIdle();

  // KILL_ON_TIMEOUT timeouts should stop the service worker.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, MixedRequestTimeouts) {
  base::Optional<blink::ServiceWorkerStatusCode> sync_status;
  base::Optional<blink::ServiceWorkerStatusCode> fetch_status;
  base::RunLoop sync_run_loop;
  base::RunLoop fetch_run_loop;

  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  StartWorker(version_.get(),
              ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME);

  // Create a fetch request that should expire sometime later.
  int fetch_request_id = version_->StartRequest(
      ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME,
      ReceiveServiceWorkerStatus(&fetch_status, fetch_run_loop.QuitClosure()));
  // Create a request that should expire Now().
  int sync_request_id = version_->StartRequestWithCustomTimeout(
      ServiceWorkerMetrics::EventType::SYNC,
      ReceiveServiceWorkerStatus(&sync_status, sync_run_loop.QuitClosure()),
      base::TimeDelta(), ServiceWorkerVersion::CONTINUE_ON_TIMEOUT);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(sync_status);

  // Verify the sync has timed out but not the fetch.
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  version_->timeout_timer_.user_task().Run();
  sync_run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout, sync_status.value());
  EXPECT_FALSE(fetch_status);

  // Background sync timeouts don't stop the service worker.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());

  // Gracefully handle the sync event finishing after the timeout.
  EXPECT_FALSE(
      version_->FinishRequest(sync_request_id, true /* was_handled */));

  // Verify that the fetch times out later.
  version_->SetAllRequestExpirations(base::TimeTicks::Now());
  version_->timeout_timer_.user_task().Run();
  fetch_run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout,
            fetch_status.value());

  // Fetch request should no longer exist.
  EXPECT_FALSE(
      version_->FinishRequest(fetch_request_id, true /* was_handled */));
  base::RunLoop().RunUntilIdle();

  // Other timeouts do stop the service worker.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, FailToStart_RendererCrash) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  auto* client = helper_->AddNewPendingInstanceClient<
      DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get());
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  base::RunLoop().RunUntilIdle();

  // Callback has not completed yet.
  EXPECT_FALSE(status);
  EXPECT_EQ(EmbeddedWorkerStatus::STARTING, version_->running_status());

  // Simulate renderer crash: break EmbeddedWorkerInstance's Mojo connection to
  // the renderer-side client.
  client->Disconnect();
  run_loop.Run();
  // Callback completed.
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorStartWorkerFailed,
            status.value());
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());
}

TEST_F(ServiceWorkerVersionTest, FailToStart_Timeout) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;

  // Start starting the worker.
  auto* client = helper_->AddNewPendingInstanceClient<
      DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get());
  client->UnblockStopWorker();
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  base::RunLoop().RunUntilIdle();

  // Callback has not completed yet.
  EXPECT_FALSE(status);
  EXPECT_EQ(EmbeddedWorkerStatus::STARTING, version_->running_status());

  // Simulate timeout.
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  version_->start_time_ = base::TimeTicks::Now() -
                          ServiceWorkerVersion::kStartNewWorkerTimeout -
                          base::TimeDelta::FromMinutes(1);
  version_->timeout_timer_.user_task().Run();
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorTimeout, status.value());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());
}

// Test that a service worker stalled in stopping will timeout and not get in a
// sticky error state.
TEST_F(ServiceWorkerVersionTest, StallInStopping_DetachThenStart) {
  // Start a worker.
  auto* client = helper_->AddNewPendingInstanceClient<
      DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get());
  client->UnblockStartWorker();
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::UNKNOWN);

  // Try to stop the worker.
  bool has_stopped = false;
  base::RunLoop run_loop;
  version_->StopWorker(VerifyCalled(&has_stopped, run_loop.QuitClosure()));
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPING, version_->running_status());
  base::RunLoop().RunUntilIdle();

  // Worker is now stalled in stopping. Verify a fast timeout is in place.
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  EXPECT_EQ(ServiceWorkerVersion::kStopWorkerTimeout,
            version_->timeout_timer_.GetCurrentDelay());

  // Simulate timeout.
  version_->stop_time_ = base::TimeTicks::Now() -
                         ServiceWorkerVersion::kStopWorkerTimeout -
                         base::TimeDelta::FromSeconds(1);
  version_->timeout_timer_.user_task().Run();
  run_loop.Run();
  EXPECT_TRUE(has_stopped);
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());

  // Try to start the worker again. It should work.
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::UNKNOWN);

  // The timeout interval should be reset to normal.
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  EXPECT_EQ(ServiceWorkerVersion::kTimeoutTimerDelay,
            version_->timeout_timer_.GetCurrentDelay());
}

// Test that a service worker stalled in stopping with a start worker
// request queued up will timeout and restart.
TEST_F(ServiceWorkerVersionTest, StallInStopping_DetachThenRestart) {
  // Start a worker.
  auto* client = helper_->AddNewPendingInstanceClient<
      DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get());
  client->UnblockStartWorker();
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::UNKNOWN);

  // Try to stop the worker.
  bool has_stopped = false;
  version_->StopWorker(VerifyCalled(&has_stopped));
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPING, version_->running_status());

  // Worker is now stalled in stopping. Add a start worker request.
  base::Optional<blink::ServiceWorkerStatusCode> start_status;
  base::RunLoop run_loop;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&start_status, run_loop.QuitClosure()));

  // Simulate timeout. The worker should stop and get restarted.
  EXPECT_TRUE(version_->timeout_timer_.IsRunning());
  version_->stop_time_ = base::TimeTicks::Now() -
                         ServiceWorkerVersion::kStopWorkerTimeout -
                         base::TimeDelta::FromSeconds(1);
  version_->timeout_timer_.user_task().Run();
  run_loop.Run();
  EXPECT_TRUE(has_stopped);
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, start_status.value());
}

TEST_F(ServiceWorkerVersionTest, RendererCrashDuringEvent) {
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);

  auto* client =
      helper_->AddNewPendingInstanceClient<FakeEmbeddedWorkerInstanceClient>(
          helper_.get());
  StartWorker(version_.get(), ServiceWorkerMetrics::EventType::SYNC);

  base::RunLoop loop;
  blink::ServiceWorkerStatusCode status = blink::ServiceWorkerStatusCode::kOk;
  int request_id = version_->StartRequest(
      ServiceWorkerMetrics::EventType::SYNC,
      base::BindOnce(
          [](base::OnceClosure done, blink::ServiceWorkerStatusCode* out_status,
             blink::ServiceWorkerStatusCode result_status) {
            *out_status = result_status;
            std::move(done).Run();
          },
          loop.QuitClosure(), &status));

  // Simulate renderer crash: break EmbeddedWorkerInstance's Mojo connection to
  // the renderer-side client. The request callback should be called.
  client->Disconnect();
  loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorFailed, status);
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version_->running_status());

  // Request already failed, calling finish should return false.
  EXPECT_FALSE(version_->FinishRequest(request_id, true /* was_handled */));
}

TEST_F(ServiceWorkerVersionTest, PingController) {
  // Start starting an worker. Ping should not be active.
  version_->StartWorker(ServiceWorkerMetrics::EventType::UNKNOWN,
                        base::DoNothing());
  EXPECT_FALSE(IsPingActivated(version_.get()));

  // Start script evaluation. Ping should be active.
  NotifyScriptEvaluationStart(version_.get());
  EXPECT_TRUE(IsPingActivated(version_.get()));

  // Finish starting the worker. Ping should still be active.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());
  EXPECT_TRUE(IsPingActivated(version_.get()));
}

// Test starting a service worker from a disallowed origin.
TEST_F(ServiceWorkerVersionTest, BadOrigin) {
  const GURL scope("bad-origin://www.example.com/test/");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = scope;
  auto registration = base::MakeRefCounted<ServiceWorkerRegistration>(
      options, helper_->context()->storage()->NewRegistrationId(),
      helper_->context()->AsWeakPtr());
  auto version = base::MakeRefCounted<ServiceWorkerVersion>(
      registration_.get(),
      GURL("bad-origin://www.example.com/test/service_worker.js"),
      blink::mojom::ScriptType::kClassic,
      helper_->context()->storage()->NewVersionId(),
      helper_->context()->AsWeakPtr());
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorDisallowed, status.value());
}

TEST_F(ServiceWorkerVersionTest,
       ForegroundServiceWorkerCountUpdatedByControllee) {
  base::test::ScopedFeatureList scoped_list;
  scoped_list.InitAndEnableFeature(features::kServiceWorkerForegroundPriority);

  // Start the worker before we have a controllee.
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Add a controllee in a different process from the service worker.
  auto remote_endpoint = ActivateWithControllee();

  // RenderProcessHost should be notified of foreground worker.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      1,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Remove the controllee.
  remote_endpoint.host_ptr()->reset();
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(version_->HasControllee());

  // RenderProcessHost should be notified that there are no foreground workers.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());
}

TEST_F(ServiceWorkerVersionTest,
       ForegroundServiceWorkerCountNotUpdatedBySameProcessControllee) {
  base::test::ScopedFeatureList scoped_list;
  scoped_list.InitAndEnableFeature(features::kServiceWorkerForegroundPriority);

  // Start the worker before we have a controllee.
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Add a controllee in the same process as the service worker.
  auto remote_endpoint =
      ActivateWithControllee(version_->embedded_worker()->process_id());

  // RenderProcessHost should be notified of foreground worker.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());
}

TEST_F(ServiceWorkerVersionTest,
       ForegroundServiceWorkerCountUpdatedByControlleeProcessIdChange) {
  base::test::ScopedFeatureList scoped_list;
  scoped_list.InitAndEnableFeature(features::kServiceWorkerForegroundPriority);

  // Start the worker before we have a controllee.
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Add a controllee, but don't begin the navigation commit yet.  This will
  // cause the client to have an invalid process id like we see in real
  // navigations.
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  registration_->SetActiveVersion(version_);
  ServiceWorkerRemoteProviderEndpoint remote_endpoint;
  auto provider_info = blink::mojom::ServiceWorkerProviderInfoForWindow::New();
  base::WeakPtr<ServiceWorkerProviderHost> host =
      ServiceWorkerProviderHost::PreCreateNavigationHost(
          helper_->context()->AsWeakPtr(), true /* is_parent_frame_secure */,
          base::NullCallback(), &provider_info);
  remote_endpoint.BindForWindow(std::move(provider_info));
  host->UpdateUrls(registration_->scope(), registration_->scope());
  host->SetControllerRegistration(registration_,
                                  false /* notify_controllerchange */);
  EXPECT_TRUE(version_->HasControllee());
  EXPECT_TRUE(host->controller());

  // RenderProcessHost should be notified of foreground worker.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      1,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Now begin the navigation commit with the same process id used by the
  // worker. This should cause the worker to stop being considered foreground
  // priority.
  host->OnBeginNavigationCommit(version_->embedded_worker()->process_id(),
                                /* render_frame_id = */ 1);

  // RenderProcessHost should be notified of foreground worker.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());
}

TEST_F(ServiceWorkerVersionTest,
       ForegroundServiceWorkerCountUpdatedByWorkerStatus) {
  base::test::ScopedFeatureList scoped_list;
  scoped_list.InitAndEnableFeature(features::kServiceWorkerForegroundPriority);

  // Add a controllee in a different process from the service worker.
  auto remote_endpoint = ActivateWithControllee();

  // RenderProcessHost should not be notified of foreground worker yet since
  // there is no worker running.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Starting the worker should notify the RenderProcessHost of the foreground
  // worker.
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(
      1,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Stopping the worker should notify the RenderProcessHost that the foreground
  // worker has been removed.
  version_->StopWorker(base::DoNothing());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());
}

class ServiceWorkerVersionNoFetchHandlerTest : public ServiceWorkerVersionTest {
 protected:
  ServiceWorkerVersion::FetchHandlerExistence GetFetchHandlerExistence()
      const override {
    return ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST;
  }
};

TEST_F(ServiceWorkerVersionNoFetchHandlerTest,
       ForegroundServiceWorkerCountNotUpdated) {
  base::test::ScopedFeatureList scoped_list;
  scoped_list.InitAndEnableFeature(features::kServiceWorkerForegroundPriority);

  // Start the worker before we have a controllee.
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
  run_loop.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());

  // Add a controllee in a different process from the service worker.
  auto remote_endpoint = ActivateWithControllee();

  // RenderProcessHost should not be notified if the service worker does not
  // have a FetchEvent handler.
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(
      0,
      helper_->mock_render_process_host()->foreground_service_worker_count());
}

TEST_F(ServiceWorkerVersionTest, FailToStart_UseNewRendererProcess) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop_1;
  ServiceWorkerContextCore* context = helper_->context();
  int64_t id = version_->version_id();
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);

  // Start once. It should choose the "existing process".
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop_1.QuitClosure()));
  run_loop_1.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(helper_->mock_render_process_id(),
            version_->embedded_worker()->process_id());
  {
    bool has_stopped = false;
    base::RunLoop run_loop;
    version_->StopWorker(VerifyCalled(&has_stopped, run_loop.QuitClosure()));
    run_loop.Run();
    EXPECT_TRUE(has_stopped);
  }

  // Fail once.
  status.reset();
  helper_->AddPendingInstanceClient(
      std::make_unique<FailStartInstanceClient>(helper_.get()));
  base::RunLoop run_loop_2;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop_2.QuitClosure()));
  run_loop_2.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorStartWorkerFailed,
            status.value());
  EXPECT_EQ(1, context->GetVersionFailureCount(id));

  // Fail again.
  status.reset();
  helper_->AddPendingInstanceClient(
      std::make_unique<FailStartInstanceClient>(helper_.get()));
  base::RunLoop run_loop_3;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop_3.QuitClosure()));
  run_loop_3.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kErrorStartWorkerFailed,
            status.value());
  EXPECT_EQ(2, context->GetVersionFailureCount(id));

  // Succeed. It should choose the "new process".
  status.reset();
  base::RunLoop run_loop_4;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop_4.QuitClosure()));
  run_loop_4.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(helper_->new_render_process_id(),
            version_->embedded_worker()->process_id());
  EXPECT_EQ(0, context->GetVersionFailureCount(id));
  version_->StopWorker(base::DoNothing());
  base::RunLoop().RunUntilIdle();

  // Start again. It should choose the "existing process" again as we no longer
  // force creation of a new process.
  status.reset();
  base::RunLoop run_loop_5;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::UNKNOWN,
      ReceiveServiceWorkerStatus(&status, run_loop_5.QuitClosure()));
  run_loop_5.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_EQ(helper_->mock_render_process_id(),
            version_->embedded_worker()->process_id());
  version_->StopWorker(base::DoNothing());
  base::RunLoop().RunUntilIdle();
}

TEST_F(ServiceWorkerVersionTest, FailToStart_RestartStalledWorker) {
  base::Optional<blink::ServiceWorkerStatusCode> status;
  base::RunLoop run_loop;
  // Stall in starting.
  auto* client = helper_->AddNewPendingInstanceClient<
      DelayedFakeEmbeddedWorkerInstanceClient>(helper_.get());
  client->UnblockStopWorker();
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME,
      ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));

  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(status);

  // The restart logic is triggered because OnStopped is called before
  // OnStarted. So the Start message is sent again. The delayed instance client
  // was already consumed, so a default fake instance client will be created,
  // which starts normally.
  bool has_stopped = false;
  version_->StopWorker(VerifyCalled(&has_stopped));
  run_loop.Run();

  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  EXPECT_TRUE(has_stopped);
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version_->running_status());
}

class ServiceWorkerNavigationHintUMATest : public ServiceWorkerVersionTest {
 protected:
  ServiceWorkerNavigationHintUMATest() : ServiceWorkerVersionTest() {}

  void StartWorker(ServiceWorkerMetrics::EventType purpose) {
    base::Optional<blink::ServiceWorkerStatusCode> status;
    base::RunLoop run_loop;
    version_->StartWorker(
        purpose, ReceiveServiceWorkerStatus(&status, run_loop.QuitClosure()));
    run_loop.Run();
    EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status.value());
  }

  void StopWorker() {
    bool has_stopped = false;
    base::RunLoop run_loop;
    version_->StopWorker(VerifyCalled(&has_stopped, run_loop.QuitClosure()));
    run_loop.Run();
    EXPECT_TRUE(has_stopped);
  }

  static const char kStartHintPrecision[];

  base::HistogramTester histogram_tester_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ServiceWorkerNavigationHintUMATest);
};

const char ServiceWorkerNavigationHintUMATest::kStartHintPrecision[] =
    "ServiceWorker.StartHintPrecision";

TEST_F(ServiceWorkerNavigationHintUMATest, Precision) {
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  StartWorker(ServiceWorkerMetrics::EventType::NAVIGATION_HINT);
  StopWorker();
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, true, 0);
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, false, 1);

  StartWorker(ServiceWorkerMetrics::EventType::NAVIGATION_HINT);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::MESSAGE);
  StopWorker();
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, true, 0);
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, false, 2);

  StartWorker(ServiceWorkerMetrics::EventType::NAVIGATION_HINT);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME);
  StopWorker();
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, true, 1);
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, false, 2);

  StartWorker(ServiceWorkerMetrics::EventType::NAVIGATION_HINT);
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::FETCH_SUB_FRAME);
  StopWorker();
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, true, 2);
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, false, 2);
}

TEST_F(ServiceWorkerNavigationHintUMATest, ConcurrentStart) {
  version_->SetStatus(ServiceWorkerVersion::ACTIVATED);
  base::Optional<blink::ServiceWorkerStatusCode> status1;
  base::Optional<blink::ServiceWorkerStatusCode> status2;
  base::RunLoop run_loop_1;
  base::RunLoop run_loop_2;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME,
      ReceiveServiceWorkerStatus(&status1, run_loop_1.QuitClosure()));
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::NAVIGATION_HINT,
      ReceiveServiceWorkerStatus(&status2, run_loop_2.QuitClosure()));
  run_loop_1.Run();
  run_loop_2.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status1.value());
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status2.value());
  StopWorker();
  // The first purpose of starting worker was not a navigation hint.
  histogram_tester_.ExpectTotalCount(kStartHintPrecision, 0);

  status1.reset();
  status2.reset();
  base::RunLoop run_loop_3;
  base::RunLoop run_loop_4;
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::NAVIGATION_HINT,
      ReceiveServiceWorkerStatus(&status2, run_loop_4.QuitClosure()));
  version_->StartWorker(
      ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME,
      ReceiveServiceWorkerStatus(&status1, run_loop_3.QuitClosure()));
  run_loop_4.Run();
  run_loop_3.Run();
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status1.value());
  EXPECT_EQ(blink::ServiceWorkerStatusCode::kOk, status2.value());
  SimulateDispatchEvent(ServiceWorkerMetrics::EventType::FETCH_MAIN_FRAME);
  StopWorker();
  // The first purpose of starting worker was a navigation hint.
  histogram_tester_.ExpectTotalCount(kStartHintPrecision, 1);
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, true, 1);
  histogram_tester_.ExpectBucketCount(kStartHintPrecision, false, 0);
}

TEST_F(ServiceWorkerNavigationHintUMATest, StartWhileStopping) {
  StartWorker(ServiceWorkerMetrics::EventType::NAVIGATION_HINT);
  bool has_stopped = false;
  version_->StopWorker(VerifyCalled(&has_stopped));
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPING, version_->running_status());
  histogram_tester_.ExpectTotalCount(kStartHintPrecision, 0);

  StartWorker(ServiceWorkerMetrics::EventType::NAVIGATION_HINT);
  // The UMA must be recorded while restarting.
  histogram_tester_.ExpectTotalCount(kStartHintPrecision, 1);
  EXPECT_TRUE(has_stopped);
  StopWorker();
  // The UMA must be recorded when the worker stopped.
  histogram_tester_.ExpectTotalCount(kStartHintPrecision, 2);
}

}  // namespace service_worker_version_unittest
}  // namespace content
