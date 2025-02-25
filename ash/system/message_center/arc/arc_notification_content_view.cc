// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/system/message_center/arc/arc_notification_content_view.h"

#include "ash/public/cpp/ash_features.h"
#include "ash/system/message_center/arc/arc_notification_surface.h"
#include "ash/system/message_center/arc/arc_notification_view.h"
// TODO(https://crbug.com/768439): Remove nogncheck when moved to ash.
#include "ash/wm/window_util.h"  // nogncheck
#include "base/auto_reset.h"
#include "base/metrics/histogram_macros.h"
#include "components/arc/metrics/arc_metrics_constants.h"
#include "components/exo/notification_surface.h"
#include "components/exo/surface.h"
#include "ui/accessibility/ax_node_data.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/compositor/layer_animation_observer.h"
#include "ui/compositor/layer_tree_owner.h"
#include "ui/events/event_handler.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/transform.h"
#include "ui/message_center/message_center.h"
#include "ui/message_center/public/cpp/message_center_constants.h"
#include "ui/message_center/public/cpp/notification.h"
#include "ui/strings/grit/ui_strings.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/widget/root_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "ui/wm/core/window_util.h"

namespace ash {

class ArcNotificationContentView::MouseEnterExitHandler
    : public ui::EventHandler {
 public:
  explicit MouseEnterExitHandler(ArcNotificationContentView* owner)
      : owner_(owner) {
    DCHECK(owner);
  }
  ~MouseEnterExitHandler() override = default;

  // ui::EventHandler
  void OnMouseEvent(ui::MouseEvent* event) override {
    ui::EventHandler::OnMouseEvent(event);
    if (event->type() == ui::ET_MOUSE_ENTERED ||
        event->type() == ui::ET_MOUSE_EXITED) {
      owner_->UpdateControlButtonsVisibility();
    }
  }

 private:
  ArcNotificationContentView* const owner_;

  DISALLOW_COPY_AND_ASSIGN(MouseEnterExitHandler);
};

class ArcNotificationContentView::EventForwarder : public ui::EventHandler {
 public:
  explicit EventForwarder(ArcNotificationContentView* owner) : owner_(owner) {}
  ~EventForwarder() override = default;

 private:
  // Some swipes are handled by Android alone. We don't want to capture swipe
  // events if we started a swipe on the chrome side then moved into the Android
  // swipe region. So, keep track of whether swipe has been 'captured' by
  // Android.
  bool swipe_captured_ = false;

  // ui::EventHandler
  void OnEvent(ui::Event* event) override {
    // Do not forward event targeted to the floating close button so that
    // keyboard press and tap are handled properly.
    if (owner_->floating_control_buttons_widget_ && event->target() &&
        owner_->floating_control_buttons_widget_->GetNativeWindow() ==
            event->target()) {
      return;
    }

    views::Widget* widget = owner_->GetWidget();
    if (!widget)
      return;

    // Forward the events to the containing widget, except for:
    // 1. Touches, because View should no longer receive touch events.
    //    See View::OnTouchEvent.
    // 2. Tap gestures are handled on the Android side, so ignore them.
    //    See https://crbug.com/709911.
    // 3. Key events. These are already forwarded by NotificationSurface's
    //    WindowDelegate.
    if (event->IsLocatedEvent()) {
      ui::LocatedEvent* located_event = event->AsLocatedEvent();
      located_event->target()->ConvertEventToTarget(widget->GetNativeWindow(),
                                                    located_event);
      if (located_event->type() == ui::ET_MOUSE_ENTERED ||
          located_event->type() == ui::ET_MOUSE_EXITED) {
        owner_->UpdateControlButtonsVisibility();
        return;
      }

      if (located_event->type() == ui::ET_MOUSE_MOVED ||
          located_event->IsMouseWheelEvent()) {
        widget->OnMouseEvent(located_event->AsMouseEvent());
      } else if (located_event->IsScrollEvent()) {
        widget->OnScrollEvent(located_event->AsScrollEvent());
        owner_->item_->CancelPress();
      } else if (located_event->IsGestureEvent() &&
                 event->type() != ui::ET_GESTURE_TAP) {
        bool slide_handled_by_android = false;
        if ((event->type() == ui::ET_GESTURE_SCROLL_BEGIN ||
             event->type() == ui::ET_GESTURE_SCROLL_UPDATE ||
             event->type() == ui::ET_GESTURE_SCROLL_END ||
             event->type() == ui::ET_GESTURE_SWIPE) &&
            owner_->surface_) {
          gfx::RectF rect(owner_->item_->GetSwipeInputRect());
          owner_->surface_->GetContentWindow()->transform().TransformRect(
              &rect);
          gfx::Point location = located_event->location();
          views::View::ConvertPointFromWidget(owner_, &location);
          bool contains = rect.Contains(gfx::PointF(location));

          if (contains && event->type() == ui::ET_GESTURE_SCROLL_BEGIN)
            swipe_captured_ = true;

          slide_handled_by_android = contains && swipe_captured_;
        }

        if (event->type() == ui::ET_GESTURE_SCROLL_BEGIN)
          owner_->item_->CancelPress();

        if (event->type() == ui::ET_GESTURE_SCROLL_END)
          swipe_captured_ = false;

        if (slide_handled_by_android &&
            event->type() == ui::ET_GESTURE_SCROLL_BEGIN) {
          is_current_slide_handled_by_android_ = true;
          owner_->message_view_->DisableSlideForcibly(true);
        } else if (is_current_slide_handled_by_android_ &&
                   event->type() == ui::ET_GESTURE_SCROLL_END) {
          is_current_slide_handled_by_android_ = false;
          owner_->message_view_->DisableSlideForcibly(false);
        }

        widget->OnGestureEvent(located_event->AsGestureEvent());
      }

      // Records UMA when user clicks/taps on the notification surface. Note
      // that here we cannot determine which actions are performed since
      // mouse/gesture events are directly forwarded to Android side.
      // Interactions with the notification itself e.g. toggling notification
      // settings are being captured as well, while clicks/taps on the close
      // button won't reach this. Interactions from keyboard are handled
      // separately in ArcNotificationItemImpl.
      if (event->type() == ui::ET_MOUSE_RELEASED ||
          event->type() == ui::ET_GESTURE_TAP) {
        UMA_HISTOGRAM_ENUMERATION(
            "Arc.UserInteraction",
            arc::UserInteractionType::NOTIFICATION_INTERACTION);
      }
    }

    // If AXTree is attached to notification content view, notification surface
    // always gets focus. Tab key events are consumed by the surface, and tab
    // focus traversal gets stuck at Android notification. To prevent it, always
    // pass tab key event to focus manager of content view.
    // TODO(yawano): include elements inside Android notification in tab focus
    // traversal rather than skipping them.
    if (owner_->surface_ &&
        owner_->surface_->GetAXTreeId() != ui::AXTreeIDUnknown() &&
        event->IsKeyEvent()) {
      ui::KeyEvent* key_event = event->AsKeyEvent();
      if (key_event->key_code() == ui::VKEY_TAB &&
          (key_event->flags() == ui::EF_NONE ||
           key_event->flags() == ui::EF_SHIFT_DOWN)) {
        widget->GetFocusManager()->OnKeyEvent(*key_event);
      }
    }
  }

  ArcNotificationContentView* const owner_;
  bool is_current_slide_handled_by_android_ = false;

  DISALLOW_COPY_AND_ASSIGN(EventForwarder);
};

class ArcNotificationContentView::SlideHelper {
 public:
  explicit SlideHelper(ArcNotificationContentView* owner) : owner_(owner) {
    // Reset opacity to 1 to handle to case when the surface is sliding before
    // getting managed by this class, e.g. sliding in a popup before showing
    // in a message center view.
    if (owner_->surface_) {
      DCHECK(owner_->surface_->GetWindow());
      owner_->surface_->GetWindow()->layer()->SetOpacity(1.0f);
    }
  }
  virtual ~SlideHelper() = default;

  void Update(base::Optional<bool> slide_in_progress) {
    if (slide_in_progress.has_value())
      slide_in_progress_ = slide_in_progress.value();

    const bool has_animation =
        GetSlideOutLayer()->GetAnimator()->is_animating();
    const bool has_transform = !GetSlideOutLayer()->transform().IsIdentity();
    const bool moving = (slide_in_progress_ && has_transform) || has_animation;

    if (moving_ == moving)
      return;
    moving_ = moving;

    if (moving_)
      owner_->ShowCopiedSurface();
    else
      owner_->HideCopiedSurface();
  }

 private:
  // This is a temporary hack to address https://crbug.com/718965
  ui::Layer* GetSlideOutLayer() {
    ui::Layer* layer = owner_->parent()->layer();
    return layer ? layer : owner_->GetWidget()->GetLayer();
  }

  ArcNotificationContentView* const owner_;
  bool slide_in_progress_ = false;
  bool moving_ = false;

  DISALLOW_COPY_AND_ASSIGN(SlideHelper);
};

// static, for ArcNotificationContentView::GetClassName().
const char ArcNotificationContentView::kViewClassName[] =
    "ArcNotificationContentView";

ArcNotificationContentView::ArcNotificationContentView(
    ArcNotificationItem* item,
    const message_center::Notification& notification,
    message_center::MessageView* message_view)
    : item_(item),
      notification_key_(item->GetNotificationKey()),
      event_forwarder_(new EventForwarder(this)),
      mouse_enter_exit_handler_(new MouseEnterExitHandler(this)),
      message_view_(message_view),
      control_buttons_view_(message_view) {
  DCHECK(message_view);

  // kNotificationWidth must be 360, since this value is separately defiend in
  // ArcNotificationWrapperView class in Android side.
  DCHECK_EQ(360, message_center::kNotificationWidth);

  SetFocusBehavior(FocusBehavior::ALWAYS);
  set_notify_enter_exit_on_child(true);

  item_->IncrementWindowRefCount();
  item_->AddObserver(this);

  auto* surface_manager = ArcNotificationSurfaceManager::Get();
  if (surface_manager) {
    surface_manager->AddObserver(this);
    ArcNotificationSurface* surface =
        surface_manager->GetArcSurface(notification_key_);
    if (surface)
      OnNotificationSurfaceAdded(surface);
  }

  // Creates the control_buttons_view_, which collects all control buttons into
  // a horizontal box.
  control_buttons_view_.set_owned_by_client();
  Update(notification);

  // Create a layer as an anchor to insert surface copy during a slide.
  SetPaintToLayer();
  // SetFillsBoundsOpaquely causes overdraw and has performance implications.
  // See the comment in this method and --show-overdraw-feedback for detail.
  layer()->SetFillsBoundsOpaquely(false);
  UpdatePreferredSize();
}

ArcNotificationContentView::~ArcNotificationContentView() {
  SetSurface(nullptr);

  auto* surface_manager = ArcNotificationSurfaceManager::Get();
  if (surface_manager)
    surface_manager->RemoveObserver(this);
  if (item_) {
    item_->RemoveObserver(this);
    item_->DecrementWindowRefCount();
  }
}

const char* ArcNotificationContentView::GetClassName() const {
  return kViewClassName;
}

void ArcNotificationContentView::Update(
    const message_center::Notification& notification) {
  control_buttons_view_.ShowSettingsButton(
      notification.should_show_settings_button());
  control_buttons_view_.ShowCloseButton(!notification.pinned());
  control_buttons_view_.ShowSnoozeButton(
      notification.should_show_snooze_button());
  UpdateControlButtonsVisibility();

  accessible_name_ = notification.accessible_name();
  UpdateSnapshot();
}

message_center::NotificationControlButtonsView*
ArcNotificationContentView::GetControlButtonsView() {
  // |control_buttons_view_| is hosted in |floating_control_buttons_widget_| and
  // should not be used when there is no |floating_control_buttons_widget_|.
  return floating_control_buttons_widget_ ? &control_buttons_view_ : nullptr;
}

void ArcNotificationContentView::UpdateControlButtonsVisibility() {
  if (!control_buttons_view_.parent())
    return;

  // If the visibility change is ongoing, skip this method to prevent an
  // infinite loop.
  if (updating_control_buttons_visibility_)
    return;

  DCHECK(floating_control_buttons_widget_);

  const bool target_visibility =
      control_buttons_view_.IsAnyButtonFocused() ||
      (message_view_->GetMode() != message_center::MessageView::Mode::SETTING &&
       IsMouseHovered());

  if (target_visibility == floating_control_buttons_widget_->IsVisible())
    return;

  // Add the guard to prevent an infinite loop. Changing visibility may generate
  // an event and it may call thie method again.
  base::AutoReset<bool> reset(&updating_control_buttons_visibility_, true);

  if (target_visibility)
    floating_control_buttons_widget_->Show();
  else
    floating_control_buttons_widget_->Hide();
}

void ArcNotificationContentView::UpdateCornerRadius(int top_radius,
                                                    int bottom_radius) {
  bool force_update =
      top_radius_ != top_radius || bottom_radius_ != bottom_radius;

  top_radius_ = top_radius;
  bottom_radius_ = bottom_radius;

  if (GetWidget())
    UpdateMask(force_update);
}

void ArcNotificationContentView::OnSlideChanged(bool in_progress) {
  if (slide_helper_)
    slide_helper_->Update(in_progress);
}

void ArcNotificationContentView::OnContainerAnimationStarted() {
  ShowCopiedSurface();
}

void ArcNotificationContentView::OnContainerAnimationEnded() {
  HideCopiedSurface();
}

void ArcNotificationContentView::MaybeCreateFloatingControlButtons() {
  // Floating close button is a transient child of |surface_| and also part
  // of the hosting widget's focus chain. It could only be created when both
  // are present. Further, if we are being destroyed (|item_| is null), don't
  // create the control buttons.
  if (!surface_ || !GetWidget() || !item_)
    return;

  DCHECK(!control_buttons_view_.parent());
  DCHECK(!floating_control_buttons_widget_);

  views::Widget::InitParams params(views::Widget::InitParams::TYPE_CONTROL);
  params.opacity = views::Widget::InitParams::TRANSLUCENT_WINDOW;
  params.ownership = views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET;
  params.parent = surface_->GetWindow();

  floating_control_buttons_widget_.reset(new views::Widget);
  floating_control_buttons_widget_->Init(params);
  floating_control_buttons_widget_->SetContentsView(&control_buttons_view_);
  floating_control_buttons_widget_->GetNativeWindow()->AddPreTargetHandler(
      mouse_enter_exit_handler_.get());

  // Put the close button into the focus chain.
  floating_control_buttons_widget_->SetFocusTraversableParent(
      GetWidget()->GetFocusTraversable());
  floating_control_buttons_widget_->SetFocusTraversableParentView(this);

  Layout();
}

void ArcNotificationContentView::SetSurface(ArcNotificationSurface* surface) {
  if (surface_ == surface)
    return;

  if (floating_control_buttons_widget_) {
    floating_control_buttons_widget_->GetNativeWindow()->RemovePreTargetHandler(
        mouse_enter_exit_handler_.get());
  }

  // Reset |floating_control_buttons_widget_| when |surface_| is changed.
  floating_control_buttons_widget_.reset();

  if (surface_) {
    DCHECK(surface_->GetWindow());
    DCHECK(surface_->GetContentWindow());
    surface_->GetContentWindow()->RemoveObserver(this);
    surface_->GetWindow()->RemovePreTargetHandler(event_forwarder_.get());

    if (surface_->GetAttachedHost() == this) {
      DCHECK_EQ(this, surface_->GetAttachedHost());
      surface_->Detach();
    }
  }

  surface_ = surface;

  if (surface_) {
    DCHECK(surface_->GetWindow());
    DCHECK(surface_->GetContentWindow());
    surface_->GetContentWindow()->AddObserver(this);
    surface_->GetWindow()->AddPreTargetHandler(event_forwarder_.get());

    if (GetWidget()) {
      // Force to detach the surface.
      if (surface_->IsAttached()) {
        // The attached host must not be this. Since if it is, this should
        // already be detached above.
        DCHECK_NE(this, surface_->GetAttachedHost());
        surface_->Detach();
      }
      AttachSurface();

      if (activate_on_attach_) {
        ActivateWidget(true);
        activate_on_attach_ = false;
      }
    }
  }
}

void ArcNotificationContentView::UpdatePreferredSize() {
  gfx::Size preferred_size;
  if (surface_)
    preferred_size = surface_->GetSize();
  else if (item_)
    preferred_size = item_->GetSnapshot().size();

  if (preferred_size.IsEmpty())
    return;

  if (preferred_size.width() != message_center::kNotificationWidth) {
    const float scale = static_cast<float>(message_center::kNotificationWidth) /
                        preferred_size.width();
    preferred_size.SetSize(message_center::kNotificationWidth,
                           preferred_size.height() * scale);
  }

  SetPreferredSize(preferred_size);
}

void ArcNotificationContentView::UpdateSnapshot() {
  // Bail if we have a |surface_| because it controls the sizes and paints UI.
  if (surface_)
    return;

  UpdatePreferredSize();
  SchedulePaint();
}

void ArcNotificationContentView::AttachSurface() {
  DCHECK(!native_view());

  if (!GetWidget())
    return;

  UpdatePreferredSize();
  surface_->Attach(this);

  // The texture for this window can be placed at subpixel position
  // with fractional scale factor. Force to align it at the pixel
  // boundary here, and when layout is updated in Layout().
  ::wm::SnapWindowToPixelBoundary(surface_->GetWindow());

  // Creates slide helper after this view is added to its parent.
  slide_helper_.reset(new SlideHelper(this));

  // Invokes Update() in case surface is attached during a slide.
  slide_helper_->Update(base::nullopt);

  // (Re-)create the floating buttons after |surface_| is attached to a widget.
  MaybeCreateFloatingControlButtons();

  UpdateMask(false /* force_update */);
}

void ArcNotificationContentView::ShowCopiedSurface() {
  if (!surface_)
    return;
  DCHECK(surface_->GetWindow());
  surface_copy_ = ::wm::RecreateLayers(surface_->GetWindow());
  // |surface_copy_| is at (0, 0) in owner_->layer().
  gfx::Rect size(surface_copy_->root()->size());
  surface_copy_->root()->SetBounds(size);
  layer()->Add(surface_copy_->root());

  if (ash::features::ShouldUseShaderRoundedCorner()) {
    surface_copy_->root()->SetRoundedCornerRadius(
        {top_radius_, top_radius_, bottom_radius_, bottom_radius_});
    surface_copy_->root()->SetIsFastRoundedCorner(true);
  } else {
    if (!surface_copy_mask_) {
      surface_copy_mask_ = views::Painter::CreatePaintedLayer(
          std::make_unique<message_center::NotificationBackgroundPainter>(
              top_radius_, bottom_radius_));
      surface_copy_mask_->layer()->SetBounds(size);
      surface_copy_mask_->layer()->SetFillsBoundsOpaquely(false);
    }
    DCHECK(!surface_copy_mask_->layer()->parent());
    surface_copy_->root()->SetMaskLayer(surface_copy_mask_->layer());
  }

  // Changes the opacity instead of setting the visibility, to keep
  // |EventFowarder| working.
  surface_->GetWindow()->layer()->SetOpacity(0.0f);
}

void ArcNotificationContentView::HideCopiedSurface() {
  if (!surface_ || !surface_copy_)
    return;
  DCHECK(surface_->GetWindow());
  surface_->GetWindow()->layer()->SetOpacity(1.0f);
  Layout();
  surface_copy_.reset();

  // Re-install the mask since the custom mask is unset by
  // |::wm::RecreateLayers()| in |ShowCopiedSurface()| method.
  UpdateMask(true /* force_update */);
}

void ArcNotificationContentView::UpdateMask(bool force_update) {
  if (top_radius_ == 0 && bottom_radius_ == 0) {
    SetCustomMask(nullptr);
    mask_insets_.reset();
    return;
  }

  gfx::Insets new_insets = GetContentsBounds().InsetsFrom(GetVisibleBounds());
  if (mask_insets_ == new_insets && !force_update)
    return;
  mask_insets_ = new_insets;

  auto mask_painter =
      std::make_unique<message_center::NotificationBackgroundPainter>(
          top_radius_, bottom_radius_);
  // Set insets to round visible notification corners. https://crbug.com/866777
  mask_painter->set_insets(new_insets);

  SetCustomMask(views::Painter::CreatePaintedLayer(std::move(mask_painter)));
}

void ArcNotificationContentView::AddedToWidget() {
  if (attached_widget_)
    attached_widget_->RemoveObserver(this);

  attached_widget_ = GetWidget();
  attached_widget_->AddObserver(this);

  // Hide the copied surface since it may be visible by OnWidgetClosing().
  if (surface_copy_)
    HideCopiedSurface();
}

void ArcNotificationContentView::RemovedFromWidget() {
  if (attached_widget_) {
    attached_widget_->RemoveObserver(this);
    attached_widget_ = nullptr;
  }
}

void ArcNotificationContentView::ViewHierarchyChanged(
    const views::ViewHierarchyChangedDetails& details) {
  views::Widget* widget = GetWidget();

  if (!details.is_add) {
    // Resets slide helper when this view is removed from its parent.
    slide_helper_.reset();

    // Bail if this view is no longer attached to a widget or native_view() has
    // attached to a different widget.
    if (!widget ||
        (native_view() && views::Widget::GetTopLevelWidgetForNativeView(
                              native_view()) != widget)) {
      return;
    }
  }

  views::NativeViewHost::ViewHierarchyChanged(details);

  if (!widget || !surface_ || !details.is_add)
    return;

  if (surface_->IsAttached())
    surface_->Detach();
  AttachSurface();
}

void ArcNotificationContentView::Layout() {
  base::AutoReset<bool> auto_reset_in_layout(&in_layout_, true);

  if (!surface_ || !GetWidget())
    return;

  bool is_surface_visible = (surface_->GetWindow()->layer()->opacity() != 0.0f);
  if (is_surface_visible) {
    // |views::NativeViewHost::Layout()| can be called only when the hosted
    // window is opaque, because that method calls
    // |views::NativeViewHostAura::ShowWidget()| and |aura::Window::Show()|
    // which has DCHECK the opacity of the window.
    views::NativeViewHost::Layout();
    // Reinstall mask to update rounded mask insets. Set null mask unless radius
    // is set.
    UpdateMask(false /* force_update */);

    // Scale notification surface if necessary.
    gfx::Transform transform;
    const gfx::Size surface_size = surface_->GetSize();
    if (!surface_size.IsEmpty()) {
      const float factor =
          static_cast<float>(message_center::kNotificationWidth) /
          surface_size.width();
      transform.Scale(factor, factor);
    }

    // Apply the transform to the surface content so that close button can
    // be positioned without the need to consider the transform.
    surface_->GetContentWindow()->SetTransform(transform);
  }

  if (floating_control_buttons_widget_) {
    const gfx::Rect contents_bounds = GetContentsBounds();

    gfx::Rect control_buttons_bounds(contents_bounds);
    const gfx::Size button_size = control_buttons_view_.GetPreferredSize();

    control_buttons_bounds.set_x(control_buttons_bounds.right() -
                                 button_size.width() -
                                 message_center::kControlButtonPadding);
    control_buttons_bounds.set_y(control_buttons_bounds.y() +
                                 message_center::kControlButtonPadding);
    control_buttons_bounds.set_width(button_size.width());
    control_buttons_bounds.set_height(button_size.height());
    floating_control_buttons_widget_->SetBounds(control_buttons_bounds);
  }

  UpdateControlButtonsVisibility();

  if (is_surface_visible)
    ::wm::SnapWindowToPixelBoundary(surface_->GetWindow());
}

void ArcNotificationContentView::OnPaint(gfx::Canvas* canvas) {
  views::NativeViewHost::OnPaint(canvas);

  SkScalar radii[8] = {top_radius_,    top_radius_,      // top-left
                       top_radius_,    top_radius_,      // top-right
                       bottom_radius_, bottom_radius_,   // bottom-right
                       bottom_radius_, bottom_radius_};  // bottom-left

  SkPath path;
  path.addRoundRect(gfx::RectToSkRect(GetLocalBounds()), radii,
                    SkPath::kCCW_Direction);
  canvas->ClipPath(path, false);

  if (!surface_ && item_ && !item_->GetSnapshot().isNull()) {
    // Draw the snapshot if there is no surface and the snapshot is available.
    const gfx::Rect contents_bounds = GetContentsBounds();
    canvas->DrawImageInt(
        item_->GetSnapshot(), 0, 0, item_->GetSnapshot().width(),
        item_->GetSnapshot().height(), contents_bounds.x(), contents_bounds.y(),
        contents_bounds.width(), contents_bounds.height(), true /* filter */);
  } else {
    // Draw a blank background otherwise. The height of the view and surface are
    // not exactly synced and user may see the blank area out of the surface.
    // This code prevetns an ugly blank area and show white color instead.
    // This should be removed after b/35786193 is done.
    canvas->DrawColor(SK_ColorWHITE);
  }
}

void ArcNotificationContentView::OnMouseEntered(const ui::MouseEvent&) {
  UpdateControlButtonsVisibility();
}

void ArcNotificationContentView::OnMouseExited(const ui::MouseEvent&) {
  UpdateControlButtonsVisibility();
}

void ArcNotificationContentView::OnFocus() {
  auto* notification_view = ArcNotificationView::FromView(parent());
  CHECK(notification_view);

  NativeViewHost::OnFocus();
  notification_view->OnContentFocused();

  if (surface_ && surface_->GetAXTreeId() != ui::AXTreeIDUnknown())
    ActivateWidget(true);
}

void ArcNotificationContentView::OnBlur() {
  if (!parent()) {
    // OnBlur may be called when this view is being removed.
    return;
  }

  auto* notification_view = ArcNotificationView::FromView(parent());
  CHECK(notification_view);

  NativeViewHost::OnBlur();
  notification_view->OnContentBlurred();
}

void ArcNotificationContentView::OnRemoteInputActivationChanged(
    bool activated) {
  // Remove the focus from the currently focused view-control in the message
  // center before activating the window of ARC notification, so that unexpected
  // key handling doesn't happen (b/74415372).
  // Focusing notification surface window doesn't steal the focus from the
  // focused view control in the message center, so that input events handles
  // on both side wrongly without this.
  GetFocusManager()->ClearFocus();

  ActivateWidget(activated);
}

void ArcNotificationContentView::ActivateWidget(bool activate) {
  if (!GetWidget())
    return;

  // Make the widget active.
  if (activate) {
    GetWidget()->widget_delegate()->SetCanActivate(true);
    GetWidget()->Activate();

    if (surface_)
      surface_->FocusSurfaceWindow();
    else
      activate_on_attach_ = true;
  } else {
    GetWidget()->widget_delegate()->SetCanActivate(false);
  }
}

views::FocusTraversable* ArcNotificationContentView::GetFocusTraversable() {
  if (floating_control_buttons_widget_)
    return static_cast<views::internal::RootView*>(
        floating_control_buttons_widget_->GetRootView());
  return nullptr;
}

void ArcNotificationContentView::GetAccessibleNodeData(
    ui::AXNodeData* node_data) {
  if (surface_ && surface_->GetAXTreeId() != ui::AXTreeIDUnknown()) {
    node_data->role = ax::mojom::Role::kClient;
    node_data->AddStringAttribute(ax::mojom::StringAttribute::kChildTreeId,
                                  surface_->GetAXTreeId().ToString());
  } else {
    node_data->role = ax::mojom::Role::kButton;
    node_data->AddStringAttribute(
        ax::mojom::StringAttribute::kRoleDescription,
        l10n_util::GetStringUTF8(
            IDS_MESSAGE_NOTIFICATION_SETTINGS_BUTTON_ACCESSIBLE_NAME));
  }
  node_data->SetName(accessible_name_);
}

void ArcNotificationContentView::OnAccessibilityEvent(ax::mojom::Event event) {
  if (event == ax::mojom::Event::kTextSelectionChanged) {
    // Activate and request focus on notification content view. If text
    // selection changed event is dispatched, it indicates that user is going to
    // type something inside Android notification. Widget of message center is
    // not activated by default. We need to activate the widget. If other view
    // in message center has focus, it can consume key event. We need to request
    // focus to move it to this content view.
    ActivateWidget(true);
    RequestFocus();
  }
}

void ArcNotificationContentView::OnWindowBoundsChanged(
    aura::Window* window,
    const gfx::Rect& old_bounds,
    const gfx::Rect& new_bounds,
    ui::PropertyChangeReason reason) {
  if (in_layout_)
    return;

  UpdatePreferredSize();
  Layout();
}

void ArcNotificationContentView::OnWindowDestroying(aura::Window* window) {
  SetSurface(nullptr);
}

void ArcNotificationContentView::OnWidgetClosing(views::Widget* widget) {
  // Actually this code doesn't show copied surface. Since it looks it doesn't
  // work during closing. This just hides the surface and revails hidden
  // snapshot: https://crbug.com/890701.
  ShowCopiedSurface();

  if (attached_widget_) {
    attached_widget_->RemoveObserver(this);
    attached_widget_ = nullptr;
  }
}

void ArcNotificationContentView::OnItemDestroying() {
  item_->RemoveObserver(this);
  item_ = nullptr;

  // Reset |surface_| with |item_| since no one is observing the |surface_|
  // after |item_| is gone and this view should be removed soon.
  SetSurface(nullptr);
}

void ArcNotificationContentView::OnItemContentChanged(
    arc::mojom::ArcNotificationShownContents content) {
  shown_content_ = content;

  bool is_normal_content_shown =
      (shown_content_ ==
       arc::mojom::ArcNotificationShownContents::CONTENTS_SHOWN);
  message_view_->SetSettingMode(!is_normal_content_shown);
}

void ArcNotificationContentView::OnNotificationSurfaceAdded(
    ArcNotificationSurface* surface) {
  if (surface->GetNotificationKey() != notification_key_)
    return;

  SetSurface(surface);

  // Notify ax::mojom::Event::kChildrenChanged to force AXNodeData of this view
  // updated. As order of OnNotificationSurfaceAdded call is not guaranteed, we
  // are dispatching the event in both ArcNotificationContentView and
  // ArcAccessibilityHelperBridge.
  NotifyAccessibilityEvent(ax::mojom::Event::kChildrenChanged, false);
}

void ArcNotificationContentView::OnNotificationSurfaceRemoved(
    ArcNotificationSurface* surface) {
  if (surface->GetNotificationKey() != notification_key_)
    return;

  SetSurface(nullptr);
}

}  // namespace ash
