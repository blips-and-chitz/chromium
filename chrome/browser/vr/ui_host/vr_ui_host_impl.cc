// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/vr/ui_host/vr_ui_host_impl.h"

#include <memory>

#include "base/task/post_task.h"
#include "chrome/browser/content_settings/tab_specific_content_settings.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "chrome/browser/media/webrtc/media_stream_capture_indicator.h"
#include "chrome/browser/permissions/permission_manager.h"
#include "chrome/browser/permissions/permission_result.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_finder.h"
#include "chrome/browser/vr/metrics/session_metrics_helper.h"
#include "chrome/browser/vr/service/browser_xr_runtime.h"
#include "chrome/browser/vr/service/xr_runtime_manager.h"
#include "chrome/browser/vr/vr_tab_helper.h"
#include "chrome/browser/vr/win/vr_browser_renderer_thread_win.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/common/service_manager_connection.h"
#include "services/device/public/mojom/constants.mojom.h"
#include "services/service_manager/public/cpp/connector.h"
#include "ui/base/l10n/l10n_util.h"

namespace vr {

namespace {
static constexpr base::TimeDelta kPermissionPromptTimeout =
    base::TimeDelta::FromSeconds(5);

static constexpr base::TimeDelta kPollCapturingStateInterval =
    base::TimeDelta::FromSecondsD(0.2);

const CapturingStateModel g_default_capturing_state;
}  // namespace

VRUiHostImpl::VRUiHostImpl(device::mojom::XRDeviceId device_id,
                           device::mojom::XRCompositorHostPtr compositor)
    : compositor_(std::move(compositor)),
      main_thread_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      weak_ptr_factory_(this) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DVLOG(1) << __func__;

  BrowserXRRuntime* runtime =
      XRRuntimeManager::GetInstance()->GetRuntime(device_id);
  if (runtime) {
    runtime->AddObserver(this);
  }

  auto* connector =
      content::ServiceManagerConnection::GetForProcess()->GetConnector();
  connector->BindInterface(device::mojom::kServiceName, &geolocation_config_);
}

VRUiHostImpl::~VRUiHostImpl() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DVLOG(1) << __func__;

  // We don't call BrowserXRRuntime::RemoveObserver, because if we are being
  // destroyed, it means the corresponding device has been removed from
  // XRRuntimeManager, and the BrowserXRRuntime has been destroyed.
  if (web_contents_)
    SetWebXRWebContents(nullptr);
}

// static
std::unique_ptr<VRUiHost> VRUiHostImpl::Create(
    device::mojom::XRDeviceId device_id,
    device::mojom::XRCompositorHostPtr compositor) {
  DVLOG(1) << __func__;
  return std::make_unique<VRUiHostImpl>(device_id, std::move(compositor));
}

bool IsValidInfo(device::mojom::VRDisplayInfoPtr& info) {
  // Numeric properties are validated elsewhere, but we expect a stereo headset.
  if (!info)
    return false;
  if (!info->leftEye)
    return false;
  if (!info->rightEye)
    return false;
  return true;
}

void VRUiHostImpl::SetWebXRWebContents(content::WebContents* contents) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  if (!IsValidInfo(info_)) {
    XRRuntimeManager::ExitImmersivePresentation();
    return;
  }

  // Eventually the contents will be used to poll for permissions, or determine
  // what overlays should show.

  // permission_request_manager_ is an unowned pointer; it's owned by
  // WebContents. If the WebContents change, make sure we unregister any
  // pre-existing observers. We only have a non-null permission_request_manager_
  // if we successfully added an observer.
  if (permission_request_manager_) {
    permission_request_manager_->RemoveObserver(this);
    permission_request_manager_ = nullptr;
  }

  if (web_contents_ != contents) {
    if (web_contents_) {
      auto* metrics_helper =
          SessionMetricsHelper::FromWebContents(web_contents_);
      metrics_helper->SetWebVREnabled(false);
      metrics_helper->SetVRActive(false);
      if (Browser* browser = chrome::FindBrowserWithWebContents(web_contents_))
        browser->GetBubbleManager()->RemoveBubbleManagerObserver(this);
    }
    if (contents) {
      auto* metrics_helper = SessionMetricsHelper::FromWebContents(contents);
      if (!metrics_helper) {
        metrics_helper = SessionMetricsHelper::CreateForWebContents(
            contents, Mode::kWebXrVrPresentation);
      } else {
        metrics_helper->SetWebVREnabled(true);
        metrics_helper->SetVRActive(true);
      }
      metrics_helper->RecordVrStartAction(VrStartAction::kPresentationRequest);
      if (Browser* browser = chrome::FindBrowserWithWebContents(contents))
        browser->GetBubbleManager()->AddBubbleManagerObserver(this);
    }
  }

  if (web_contents_)
    VrTabHelper::SetIsContentDisplayedInHeadset(web_contents_, false);
  if (contents)
    VrTabHelper::SetIsContentDisplayedInHeadset(contents, true);

  web_contents_ = contents;
  if (contents) {
    StartUiRendering();
    InitCapturingStates();
    ui_rendering_thread_->SetWebXrPresenting(true);

    PollCapturingState();

    PermissionRequestManager::CreateForWebContents(contents);
    permission_request_manager_ =
        PermissionRequestManager::FromWebContents(contents);
    // Attaching a permission request manager to WebContents can fail, so a
    // DCHECK would be inappropriate here. If it fails, the user won't get
    // notified about permission prompts, but other than that the session would
    // work normally.
    if (permission_request_manager_) {
      permission_request_manager_->AddObserver(this);

      // There might already be a visible permission bubble from before
      // we registered the observer, show the HMD message now in that case.
      if (permission_request_manager_->IsBubbleVisible())
        OnBubbleAdded();
    } else {
      DVLOG(1) << __func__ << ": No PermissionRequestManager";
    }
  } else {
    poll_capturing_state_task_.Cancel();

    if (ui_rendering_thread_)
      ui_rendering_thread_->SetWebXrPresenting(false);
    StopUiRendering();
  }
}

void VRUiHostImpl::SetVRDisplayInfo(
    device::mojom::VRDisplayInfoPtr display_info) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DVLOG(1) << __func__;

  if (!IsValidInfo(display_info)) {
    XRRuntimeManager::ExitImmersivePresentation();
    return;
  }

  info_ = std::move(display_info);
  if (ui_rendering_thread_) {
    ui_rendering_thread_->SetVRDisplayInfo(info_.Clone());
  }
}

void VRUiHostImpl::StartUiRendering() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DVLOG(1) << __func__;

  DCHECK(info_);
  ui_rendering_thread_ =
      std::make_unique<VRBrowserRendererThreadWin>(compositor_.get());
  ui_rendering_thread_->SetVRDisplayInfo(info_.Clone());
}

void VRUiHostImpl::StopUiRendering() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DVLOG(1) << __func__;

  ui_rendering_thread_ = nullptr;
}

void VRUiHostImpl::SetLocationInfoOnUi() {
  GURL gurl;
  if (web_contents_) {
    content::NavigationEntry* entry =
        web_contents_->GetController().GetVisibleEntry();
    if (entry) {
      gurl = entry->GetVirtualURL();
    }
  }
  // TODO(https://crbug.com/905375): The below call should eventually be
  // rewritten to take a LocationBarState and not just GURL. See
  // VRBrowserRendererThreadWin::StartOverlay() also.
  ui_rendering_thread_->SetLocationInfo(gurl);
}

void VRUiHostImpl::OnBubbleAdded() {
  if (!ui_rendering_thread_) {
    DVLOG(1) << __func__ << ": no ui_rendering_thread_";
    return;
  }

  SetLocationInfoOnUi();

  if (indicators_visible_) {
    indicators_visible_ = false;
    ui_rendering_thread_->SetIndicatorsVisible(false);
  }

  ui_rendering_thread_->SetVisibleExternalPromptNotification(
      ExternalPromptNotificationType::kPromptGenericPermission);

  is_external_prompt_showing_in_headset_ = true;
  external_prompt_timeout_task_.Reset(
      base::BindRepeating(&VRUiHostImpl::RemoveHeadsetNotificationPrompt,
                          weak_ptr_factory_.GetWeakPtr()));
  main_thread_task_runner_->PostDelayedTask(
      FROM_HERE, external_prompt_timeout_task_.callback(),
      kPermissionPromptTimeout);
}

void VRUiHostImpl::OnBubbleRemoved() {
  external_prompt_timeout_task_.Cancel();
  RemoveHeadsetNotificationPrompt();
}

void VRUiHostImpl::OnBubbleNeverShown(BubbleReference bubble) {}

void VRUiHostImpl::OnBubbleClosed(BubbleReference bubble,
                                  BubbleCloseReason reason) {
  OnBubbleRemoved();
}

void VRUiHostImpl::OnBubbleShown(BubbleReference bubble) {
  OnBubbleAdded();
}

void VRUiHostImpl::RemoveHeadsetNotificationPrompt() {
  if (!is_external_prompt_showing_in_headset_)
    return;
  is_external_prompt_showing_in_headset_ = false;
  ui_rendering_thread_->SetVisibleExternalPromptNotification(
      ExternalPromptNotificationType::kPromptNone);
  indicators_shown_start_time_ = base::Time::Now();
}

void VRUiHostImpl::InitCapturingStates() {
  active_capturing_ = g_default_capturing_state;
  potential_capturing_ = g_default_capturing_state;

  DCHECK(web_contents_);
  PermissionManager* permission_manager = PermissionManager::Get(
      Profile::FromBrowserContext(web_contents_->GetBrowserContext()));
  const GURL& origin = web_contents_->GetLastCommittedURL();
  content::RenderFrameHost* rfh = web_contents_->GetMainFrame();
  potential_capturing_.audio_capture_enabled =
      permission_manager
          ->GetPermissionStatusForFrame(
              ContentSettingsType::CONTENT_SETTINGS_TYPE_MEDIASTREAM_MIC, rfh,
              origin)
          .content_setting == CONTENT_SETTING_ALLOW;
  potential_capturing_.video_capture_enabled =
      permission_manager
          ->GetPermissionStatusForFrame(
              ContentSettingsType::CONTENT_SETTINGS_TYPE_MEDIASTREAM_CAMERA,
              rfh, origin)
          .content_setting == CONTENT_SETTING_ALLOW;
  potential_capturing_.location_access_enabled =
      permission_manager
          ->GetPermissionStatusForFrame(
              ContentSettingsType::CONTENT_SETTINGS_TYPE_GEOLOCATION, rfh,
              origin)
          .content_setting == CONTENT_SETTING_ALLOW;

  indicators_shown_start_time_ = base::Time::Now();
  indicators_visible_ = false;
}

void VRUiHostImpl::PollCapturingState() {
  poll_capturing_state_task_.Reset(base::BindRepeating(
      &VRUiHostImpl::PollCapturingState, base::Unretained(this)));
  main_thread_task_runner_->PostDelayedTask(
      FROM_HERE, poll_capturing_state_task_.callback(),
      kPollCapturingStateInterval);

  // Microphone, Camera, location.
  CapturingStateModel active_capturing = active_capturing_;
  TabSpecificContentSettings* settings =
      TabSpecificContentSettings::FromWebContents(web_contents_);
  if (settings) {
    const ContentSettingsUsagesState& usages_state =
        settings->geolocation_usages_state();
    if (!usages_state.state_map().empty()) {
      unsigned int state_flags = 0;
      usages_state.GetDetailedInfo(nullptr, &state_flags);
      active_capturing.location_access_enabled = !!(
          state_flags & ContentSettingsUsagesState::TABSTATE_HAS_ANY_ALLOWED);
    }
    active_capturing.audio_capture_enabled =
        (settings->GetMicrophoneCameraState() &
         TabSpecificContentSettings::MICROPHONE_ACCESSED) &&
        !(settings->GetMicrophoneCameraState() &
          TabSpecificContentSettings::MICROPHONE_BLOCKED);
    active_capturing.video_capture_enabled =
        (settings->GetMicrophoneCameraState() &
         TabSpecificContentSettings::CAMERA_ACCESSED) &
        !(settings->GetMicrophoneCameraState() &
          TabSpecificContentSettings::CAMERA_BLOCKED);
  }

  // Screen capture, bluetooth.
  scoped_refptr<MediaStreamCaptureIndicator> indicator =
      MediaCaptureDevicesDispatcher::GetInstance()
          ->GetMediaStreamCaptureIndicator();
  active_capturing.screen_capture_enabled =
      indicator->IsBeingMirrored(web_contents_);
  active_capturing.bluetooth_connected =
      web_contents_->IsConnectedToBluetoothDevice();

  if (active_capturing_ != active_capturing) {
    indicators_shown_start_time_ = base::Time::Now();
  }

  active_capturing_ = active_capturing;
  ui_rendering_thread_->SetCapturingState(
      active_capturing_, g_default_capturing_state, potential_capturing_);

  if (indicators_shown_start_time_ + kPermissionPromptTimeout >
      base::Time::Now()) {
    if (!indicators_visible_ && !is_external_prompt_showing_in_headset_) {
      indicators_visible_ = true;
      ui_rendering_thread_->SetIndicatorsVisible(true);
    }
  } else {
    if (indicators_visible_) {
      indicators_visible_ = false;
      ui_rendering_thread_->SetIndicatorsVisible(false);
    }
  }
}

}  // namespace vr
