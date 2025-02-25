// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/vr/windows/compositor_base.h"

#include "base/bind.h"
#include "base/trace_event/common/trace_event_common.h"
#include "ui/gfx/geometry/angle_conversions.h"
#include "ui/gfx/transform.h"

#if defined(OS_WIN)
#include "device/vr/windows/d3d11_texture_helper.h"
#endif

namespace {
// Number of frames to use for sliding averages for pose timings,
// as used for estimating prediction times.
constexpr unsigned kSlidingAverageSize = 5;
}  // namespace

namespace device {

mojom::XRFrameDataPtr XRDeviceAbstraction::GetNextFrameData() {
  return nullptr;
}
mojom::XRGamepadDataPtr XRDeviceAbstraction::GetNextGamepadData() {
  return nullptr;
}
void XRDeviceAbstraction::OnSessionStart() {}
void XRDeviceAbstraction::HandleDeviceLost() {}
bool XRDeviceAbstraction::PreComposite() {
  return true;
}
void XRDeviceAbstraction::OnLayerBoundsChanged() {}

XRCompositorCommon::OutstandingFrame::OutstandingFrame() = default;
XRCompositorCommon::OutstandingFrame::~OutstandingFrame() = default;

XRCompositorCommon::XRCompositorCommon()
    : base::Thread("WindowsXRCompositor"),
      main_thread_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      webxr_js_time_(kSlidingAverageSize),
      webxr_gpu_time_(kSlidingAverageSize),
      presentation_binding_(this),
      frame_data_binding_(this),
      gamepad_provider_(this),
      overlay_binding_(this) {
  DCHECK(main_thread_task_runner_);
}

XRCompositorCommon::~XRCompositorCommon() {
  // Since we derive from base::Thread, all derived classes must call Stop() in
  // their destructor so the thread gets torn down before any members that may
  // be in use on the thread get torn down.
}

void XRCompositorCommon::ClearPendingFrame() {
  pending_frame_.reset();
  // Send frame data to outstanding requests.
  if (delayed_get_frame_data_callback_ &&
      (webxr_visible_ || on_webxr_submitted_)) {
    // If WebXR is not visible, but the browser wants to know when it submits a
    // frame, we allow the renderer to receive poses.
    std::move(delayed_get_frame_data_callback_).Run();
  }
}

void XRCompositorCommon::SubmitFrameMissing(int16_t frame_index,
                                            const gpu::SyncToken& sync_token) {
  TRACE_EVENT_INSTANT0("xr", "SubmitFrameMissing", TRACE_EVENT_SCOPE_THREAD);
  if (pending_frame_) {
    // WebXR for this frame is hidden.
    pending_frame_->waiting_for_webxr_ = false;
  }
  webxr_has_pose_ = false;
  MaybeCompositeAndSubmit();
}

void XRCompositorCommon::SubmitFrame(int16_t frame_index,
                                     const gpu::MailboxHolder& mailbox,
                                     base::TimeDelta time_waited) {
  NOTREACHED();
}

void XRCompositorCommon::SubmitFrameDrawnIntoTexture(
    int16_t frame_index,
    const gpu::SyncToken& sync_token,
    base::TimeDelta time_waited) {
  // Not currently implemented for Windows.
  NOTREACHED();
}

void XRCompositorCommon::SubmitFrameWithTextureHandle(
    int16_t frame_index,
    mojo::ScopedHandle texture_handle) {
  TRACE_EVENT1("xr", "SubmitFrameWithTextureHandle", "frameIndex", frame_index);
  webxr_has_pose_ = false;
  // Tell the browser that WebXR has submitted a frame.
  if (on_webxr_submitted_)
    std::move(on_webxr_submitted_).Run();

  if (!pending_frame_ || pending_frame_->frame_data_->frame_id != frame_index) {
    // We weren't expecting a submitted frame.  This can happen if WebXR was
    // hidden by an overlay for some time.
    if (submit_client_) {
      submit_client_->OnSubmitFrameTransferred(false);
      submit_client_->OnSubmitFrameRendered();
      TRACE_EVENT1("xr", "SubmitFrameTransferred", "success", false);
    }
    return;
  }

  pending_frame_->waiting_for_webxr_ = false;
  pending_frame_->submit_frame_time_ = base::TimeTicks::Now();

#if defined(OS_WIN)
  MojoPlatformHandle platform_handle;
  platform_handle.struct_size = sizeof(platform_handle);
  MojoResult result = MojoUnwrapPlatformHandle(texture_handle.release().value(),
                                               nullptr, &platform_handle);
  if (result == MOJO_RESULT_OK) {
    texture_helper_.SetSourceTexture(
        base::win::ScopedHandle(
            reinterpret_cast<HANDLE>(platform_handle.value)),
        left_webxr_bounds_, right_webxr_bounds_);
    pending_frame_->webxr_submitted_ = true;
  }

  // Regardless of success - try to composite what we have.
  MaybeCompositeAndSubmit();
#endif
}

void XRCompositorCommon::CleanUp() {
  submit_client_ = nullptr;
  webxr_has_pose_ = false;
  presentation_binding_.Close();
  frame_data_binding_.Close();
  gamepad_provider_.Close();
  overlay_binding_.Close();
  StopRuntime();
}

void XRCompositorCommon::RequestGamepadProvider(
    mojom::IsolatedXRGamepadProviderRequest request) {
  gamepad_provider_.Close();
  gamepad_provider_.Bind(std::move(request));
}

void XRCompositorCommon::RequestOverlay(
    mojom::ImmersiveOverlayRequest request) {
  overlay_binding_.Close();
  overlay_binding_.Bind(std::move(request));

  // WebXR is visible and overlay hidden by default until the overlay overrides
  // this.
  SetOverlayAndWebXRVisibility(false, true);
}

void XRCompositorCommon::UpdateLayerBounds(int16_t frame_id,
                                           const gfx::RectF& left_bounds,
                                           const gfx::RectF& right_bounds,
                                           const gfx::Size& source_size) {
  // Bounds are updated instantly, rather than waiting for frame_id.  This works
  // since blink always passes the current frame_id when updating the bounds.
  // Ignoring the frame_id keeps the logic simpler, so this can more easily
  // merge with vr_shell_gl eventually.
  left_webxr_bounds_ = left_bounds;
  right_webxr_bounds_ = right_bounds;
  source_size_ = source_size;

  OnLayerBoundsChanged();
}

void XRCompositorCommon::RequestSession(
    base::OnceCallback<void()> on_presentation_ended,
    mojom::XRRuntimeSessionOptionsPtr options,
    RequestSessionCallback callback) {
  DCHECK(options->immersive);
  webxr_has_pose_ = false;
  presentation_binding_.Close();
  frame_data_binding_.Close();

  if (!StartRuntime()) {
    TRACE_EVENT_INSTANT0("xr", "Failed to start runtime",
                         TRACE_EVENT_SCOPE_THREAD);
    main_thread_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), false, nullptr));
    return;
  }

  // If on_presentation_ended_ is not already null, we won't call to notify the
  // runtime that that session has completed.  This is ok because the XRRuntime
  // knows it has requested a new session, and isn't expecting that callback to
  // be called.
  on_presentation_ended_ = std::move(on_presentation_ended);

  device::mojom::XRPresentationProviderPtr presentation_provider;
  device::mojom::XRFrameDataProviderPtr frame_data_provider;
  presentation_binding_.Bind(mojo::MakeRequest(&presentation_provider));
  frame_data_binding_.Bind(mojo::MakeRequest(&frame_data_provider));

  device::mojom::XRPresentationTransportOptionsPtr transport_options =
      device::mojom::XRPresentationTransportOptions::New();
  transport_options->transport_method =
      device::mojom::XRPresentationTransportMethod::SUBMIT_AS_TEXTURE_HANDLE;
  // Only set boolean options that we need. Default is false, and we should be
  // able to safely ignore ones that our implementation doesn't care about.
  transport_options->wait_for_transfer_notification = true;

  OnSessionStart();

  auto submit_frame_sink = device::mojom::XRPresentationConnection::New();
  submit_frame_sink->provider = presentation_provider.PassInterface();
  submit_frame_sink->client_request = mojo::MakeRequest(&submit_client_);
  submit_frame_sink->transport_options = std::move(transport_options);

  auto session = device::mojom::XRSession::New();
  session->data_provider = frame_data_provider.PassInterface();
  session->submit_frame_sink = std::move(submit_frame_sink);

  main_thread_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(std::move(callback), true, std::move(session)));
  is_presenting_ = true;

  texture_helper_.SetSourceAndOverlayVisible(webxr_visible_, overlay_visible_);
}

void XRCompositorCommon::ExitPresent() {
  TRACE_EVENT_INSTANT0("xr", "ExitPresent", TRACE_EVENT_SCOPE_THREAD);
  is_presenting_ = false;
  webxr_has_pose_ = false;
  presentation_binding_.Close();
  frame_data_binding_.Close();
  submit_client_ = nullptr;
  StopRuntime();

  pending_frame_.reset();
  delayed_get_frame_data_callback_.Reset();

  // Reset webxr_visible_ for subsequent presentations.
  webxr_visible_ = true;

  // Push out one more controller update so we don't have stale controllers.
  UpdateControllerState();

  // Kill outstanding overlays:
  overlay_visible_ = false;
  overlay_binding_.Close();

  texture_helper_.SetSourceAndOverlayVisible(false, false);

  if (on_presentation_ended_) {
    main_thread_task_runner_->PostTask(FROM_HERE,
                                       std::move(on_presentation_ended_));
  }
}

void XRCompositorCommon::UpdateControllerState() {
  if (!gamepad_callback_) {
    // Nobody is listening to updates, so bail early.
    return;
  }

  if (!is_presenting_ || !webxr_visible_) {
    std::move(gamepad_callback_).Run(nullptr);
    return;
  }

  std::move(gamepad_callback_).Run(GetNextGamepadData());
}

void XRCompositorCommon::RequestUpdate(
    mojom::IsolatedXRGamepadProvider::RequestUpdateCallback callback) {
  // Save the callback to resolve next time we update (typically on vsync).
  gamepad_callback_ = std::move(callback);

  // If we aren't presenting, reply now saying that we have no controllers.
  if (!is_presenting_) {
    UpdateControllerState();
  }
}

void XRCompositorCommon::Init() {}

void XRCompositorCommon::StartPendingFrame() {
  if (!pending_frame_) {
    pending_frame_.emplace();
    pending_frame_->waiting_for_webxr_ = webxr_visible_;
    pending_frame_->waiting_for_overlay_ = overlay_visible_;
    pending_frame_->frame_data_ = GetNextFrameData();
  }
}

void XRCompositorCommon::GetFrameData(
    mojom::XRFrameDataProvider::GetFrameDataCallback callback) {
  TRACE_EVENT0("xr", "GetFrameData");
  if (!is_presenting_) {
    return;
  }

  // If we've already given out a pose for the current frame, or aren't visible,
  // delay giving out a pose until the next frame we are visible.
  // However, if we aren't visible and the browser is waiting to learn that
  // WebXR has submitted a frame, we can give out a pose as though we are
  // visible.
  if ((!webxr_visible_ && !on_webxr_submitted_) || webxr_has_pose_) {
    // There should only be one outstanding GetFrameData call at a time.  We
    // shouldn't get new ones until this resolves or presentation ends/restarts.
    if (delayed_get_frame_data_callback_) {
      mojo::ReportBadMessage("Multiple outstanding GetFrameData calls");
    }
    delayed_get_frame_data_callback_ =
        base::BindOnce(&XRCompositorCommon::GetFrameData,
                       base::Unretained(this), std::move(callback));
    return;
  }

  StartPendingFrame();
  webxr_has_pose_ = true;
  pending_frame_->webxr_has_pose_ = true;
  pending_frame_->sent_frame_data_time_ = base::TimeTicks::Now();

  // Yield here to let the event queue process pending mojo messages,
  // specifically the next gamepad callback request that's likely to
  // have been sent during WaitGetPoses.
  task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&XRCompositorCommon::GetControllerDataAndSendFrameData,
                     base::Unretained(this), std::move(callback),
                     pending_frame_->frame_data_.Clone()));

  next_frame_id_ += 1;
  if (next_frame_id_ < 0) {
    next_frame_id_ = 0;
  }
}

void XRCompositorCommon::GetControllerDataAndSendFrameData(
    XRFrameDataProvider::GetFrameDataCallback callback,
    mojom::XRFrameDataPtr frame_data) {
  TRACE_EVENT0("xr", "GetControllerDataAndSendFrameData");
  // Update gamepad controllers.
  UpdateControllerState();

  // We have posted a message to allow other calls to get through, and now state
  // may have changed.  WebXR may not be presenting any more, or may be hidden.
  std::move(callback).Run(is_presenting_ &&
                                  (webxr_visible_ || on_webxr_submitted_)
                              ? std::move(frame_data)
                              : mojom::XRFrameData::New());
}

void XRCompositorCommon::SubmitOverlayTexture(
    int16_t frame_id,
    mojo::ScopedHandle texture_handle,
    const gfx::RectF& left_bounds,
    const gfx::RectF& right_bounds,
    SubmitOverlayTextureCallback overlay_submit_callback) {
  TRACE_EVENT_INSTANT0("xr", "SubmitOverlay", TRACE_EVENT_SCOPE_THREAD);
  DCHECK(overlay_visible_);
  overlay_submit_callback_ = std::move(overlay_submit_callback);
  if (!pending_frame_) {
    // We may stop presenting while there is a pending SubmitOverlayTexture
    // outstanding.  If we get an overlay submit we weren't expecting, just
    // ignore it.
    DCHECK(!is_presenting_);
    std::move(overlay_submit_callback_).Run(false);
    return;
  }

  pending_frame_->waiting_for_overlay_ = false;

#if defined(OS_WIN)
  MojoPlatformHandle platform_handle;
  platform_handle.struct_size = sizeof(platform_handle);
  MojoResult result = MojoUnwrapPlatformHandle(texture_handle.release().value(),
                                               nullptr, &platform_handle);
  if (result == MOJO_RESULT_OK) {
    texture_helper_.SetOverlayTexture(
        base::win::ScopedHandle(
            reinterpret_cast<HANDLE>(platform_handle.value)),
        left_bounds, right_bounds);
    pending_frame_->overlay_submitted_ = true;
  } else {
    std::move(overlay_submit_callback_).Run(false);
  }

  // Regardless of success - try to composite what we have.
  MaybeCompositeAndSubmit();
#endif
}

void XRCompositorCommon::RequestNextOverlayPose(
    RequestNextOverlayPoseCallback callback) {
  // We will only request poses while the overlay is visible.
  DCHECK(overlay_visible_);
  TRACE_EVENT_INSTANT0("xr", "RequestOverlayPose", TRACE_EVENT_SCOPE_THREAD);

  // Ensure we have a pending frame.
  StartPendingFrame();
  pending_frame_->overlay_has_pose_ = true;
  std::move(callback).Run(pending_frame_->frame_data_.Clone());
}

void XRCompositorCommon::SetOverlayAndWebXRVisibility(bool overlay_visible,
                                                      bool webxr_visible) {
  TRACE_EVENT_INSTANT2("xr", "SetOverlayAndWebXRVisibility",
                       TRACE_EVENT_SCOPE_THREAD, "overlay", overlay_visible,
                       "webxr", webxr_visible);
  // Update state.
  webxr_visible_ = webxr_visible;
  overlay_visible_ = overlay_visible;
  if (pending_frame_) {
    pending_frame_->waiting_for_webxr_ =
        pending_frame_->waiting_for_webxr_ && webxr_visible;
    pending_frame_->waiting_for_overlay_ =
        pending_frame_->waiting_for_overlay_ && overlay_visible;
  }

  // Update texture helper.
  texture_helper_.SetSourceAndOverlayVisible(webxr_visible, overlay_visible);

  // Maybe composite and submit if we have a pending that is now valid to
  // submit.
  MaybeCompositeAndSubmit();
}

void XRCompositorCommon::RequestNotificationOnWebXrSubmitted(
    RequestNotificationOnWebXrSubmittedCallback callback) {
  on_webxr_submitted_ = std::move(callback);
}

void XRCompositorCommon::MaybeCompositeAndSubmit() {
  if (!pending_frame_) {
    // There is no outstanding frame, nor frame to composite, but there may be
    // pending GetFrameData calls, so ClearPendingFrame() to respond to them.
    ClearPendingFrame();
    return;
  }

  // Check if we have obtained all layers (overlay and webxr) that we need.
  if (pending_frame_->waiting_for_webxr_ ||
      pending_frame_->waiting_for_overlay_) {
    // Haven't received submits from all layers.
    return;
  }

  bool no_submit = false;
  if (!(pending_frame_->webxr_submitted_ && webxr_visible_) &&
      !(pending_frame_->overlay_submitted_ && overlay_visible_)) {
    // Nothing visible was submitted - we can't composite/submit to headset.
    no_submit = true;
  }

  bool copy_successful;

  // If so, tell texture helper to composite, then grab the output texture, and
  // submit. If we submitted, set up the next frame, and send outstanding pose
  // requests.
  if (no_submit) {
    copy_successful = false;
    texture_helper_.CleanupNoSubmit();
  } else {
    copy_successful = texture_helper_.UpdateBackbufferSizes() &&
                      PreComposite() && texture_helper_.CompositeToBackBuffer();
    if (copy_successful) {
      pending_frame_->frame_ready_time_ = base::TimeTicks::Now();
      if (!SubmitCompositedFrame()) {
        ExitPresent();
      }
    }
  }

  if (pending_frame_->webxr_submitted_ && copy_successful) {
    // We've submitted a frame.
    webxr_js_time_.AddSample(pending_frame_->submit_frame_time_ -
                             pending_frame_->sent_frame_data_time_);
    webxr_gpu_time_.AddSample(pending_frame_->frame_ready_time_ -
                              pending_frame_->submit_frame_time_);

    TRACE_EVENT_INSTANT2(
        "gpu", "WebXR frame time (ms)", TRACE_EVENT_SCOPE_THREAD, "javascript",
        webxr_js_time_.GetAverage().InMillisecondsF(), "rendering",
        webxr_gpu_time_.GetAverage().InMillisecondsF());
    fps_meter_.AddFrame(base::TimeTicks::Now());
    TRACE_COUNTER1("gpu", "WebXR FPS", fps_meter_.GetFPS());
  }

  if (pending_frame_->webxr_submitted_ && submit_client_) {
    // Tell WebVR that we are done with the texture (if we got a texture)
    submit_client_->OnSubmitFrameTransferred(copy_successful);
    submit_client_->OnSubmitFrameRendered();
    TRACE_EVENT1("xr", "SubmitFrameTransferred", "success", copy_successful);
  }

  if (pending_frame_->overlay_submitted_ && overlay_submit_callback_) {
    // Tell the browser/overlay that we are done with its texture so it can be
    // reused.
    std::move(overlay_submit_callback_).Run(copy_successful);
  }

  ClearPendingFrame();
}

}  // namespace device
