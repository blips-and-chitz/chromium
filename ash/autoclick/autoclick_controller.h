// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_AUTOCLICK_AUTOCLICK_CONTROLLER_H
#define ASH_AUTOCLICK_AUTOCLICK_CONTROLLER_H

#include "ash/ash_export.h"
#include "ash/public/cpp/ash_constants.h"
#include "ash/public/interfaces/accessibility_controller_enums.mojom.h"
#include "base/macros.h"
#include "base/time/time.h"
#include "ui/aura/window_observer.h"
#include "ui/events/event_handler.h"
#include "ui/gfx/geometry/point.h"

namespace base {
class RetainingOneShotTimer;
}  // namespace base

namespace views {
class Widget;
}  // namespace views

namespace ash {

class AutoclickDragEventRewriter;
class AutoclickRingHandler;
class AutoclickMenuBubbleController;

// Autoclick is one of the accessibility features. If enabled, two circles will
// animate at the mouse event location and an automatic mouse event event will
// happen after a certain amount of time at that location. The event type is
// determined by SetAutoclickEventType.
class ASH_EXPORT AutoclickController : public ui::EventHandler,
                                       public aura::WindowObserver {
 public:
  AutoclickController();
  ~AutoclickController() override;

  // Set whether autoclicking is enabled.
  void SetEnabled(bool enabled);

  // Returns true if autoclicking is enabled.
  bool IsEnabled() const;

  // Set the time to wait in milliseconds from when the mouse stops moving
  // to when the autoclick event is sent.
  void SetAutoclickDelay(base::TimeDelta delay);

  // Gets the default wait time as a base::TimeDelta object.
  static base::TimeDelta GetDefaultAutoclickDelay();

  // Sets the event type.
  void SetAutoclickEventType(mojom::AutoclickEventType type);

  // Sets the movement threshold beyond which mouse movements cancel or begin
  // a new Autoclick event.
  void SetMovementThreshold(int movement_threshold);

  // Sets the menu position and updates the UI.
  void SetMenuPosition(mojom::AutoclickMenuPosition menu_position);

  // Sets whether to revert to a left click after any other event type.
  void set_revert_to_left_click(bool revert_to_left_click) {
    revert_to_left_click_ = revert_to_left_click;
  }

  // Functionality for testing.
  static float GetStartGestureDelayRatioForTesting();
  AutoclickMenuBubbleController* GetMenuBubbleControllerForTesting() {
    return menu_bubble_controller_.get();
  }

 private:
  void SetTapDownTarget(aura::Window* target);
  void CreateAutoclickRingWidget(const gfx::Point& point_in_screen);
  void UpdateAutoclickRingWidget(views::Widget* widget,
                                 const gfx::Point& point_in_screen);
  void DoAutoclickAction();
  void StartAutoclickGesture();
  void CancelAutoclickAction();
  void OnActionCompleted(mojom::AutoclickEventType event_type);
  void InitClickTimers();
  void UpdateRingWidget(const gfx::Point& mouse_location);
  void UpdateRingSize();
  void RecordUserAction(mojom::AutoclickEventType event_type) const;
  bool DragInProgress() const;
  void CreateMenuBubbleController();
  bool AutoclickMenuContainsPoint(const gfx::Point& point) const;

  // ui::EventHandler overrides:
  void OnMouseEvent(ui::MouseEvent* event) override;
  void OnKeyEvent(ui::KeyEvent* event) override;
  void OnTouchEvent(ui::TouchEvent* event) override;
  void OnGestureEvent(ui::GestureEvent* event) override;
  void OnScrollEvent(ui::ScrollEvent* event) override;

  // aura::WindowObserver overrides:
  void OnWindowDestroying(aura::Window* window) override;

  // Whether Autoclick is currently enabled.
  bool enabled_ = false;
  mojom::AutoclickEventType event_type_ = kDefaultAutoclickEventType;
  bool revert_to_left_click_ = true;
  int movement_threshold_ = kDefaultAutoclickMovementThreshold;
  // TODO(katie): The default position should flex with the user's choice of
  // language (RTL vs LTR) and shelf position, following the same behavior
  // as the volume slider bubble. However, once the user changes the position
  // manually, the position will be fixed regardless of language direction and
  // shelf position. This probably means adding a new AutoclickMenuPostion
  // enum for "system default".
  mojom::AutoclickMenuPosition menu_position_ = kDefaultAutoclickMenuPosition;
  int mouse_event_flags_ = ui::EF_NONE;
  // The target window is observed by AutoclickController for the duration
  // of a autoclick gesture.
  aura::Window* tap_down_target_ = nullptr;
  // The position in screen coordinates used to determine the distance the
  // mouse has moved since dwell began. It is used to determine
  // if move events should cancel the gesture.
  gfx::Point anchor_location_ = gfx::Point(-kDefaultAutoclickMovementThreshold,
                                           -kDefaultAutoclickMovementThreshold);
  // The position in screen coodinates tracking where the autoclick gesture
  // should be anchored. While the |start_gesture_timer_| is running and before
  // the animation is drawn, subtle mouse movements will update the
  // |gesture_anchor_location_|, so that once animation begins it can focus on
  // the most recent mose point.
  gfx::Point gesture_anchor_location_ =
      gfx::Point(-kDefaultAutoclickMovementThreshold,
                 -kDefaultAutoclickMovementThreshold);

  std::unique_ptr<views::Widget> widget_;
  base::TimeDelta delay_;
  // The timer that counts down from the beginning of a gesture until a click.
  std::unique_ptr<base::RetainingOneShotTimer> autoclick_timer_;
  // The timer that counts from when the user stops moving the mouse
  // until the start of the animated gesture. This keeps the animation from
  // showing up when the mouse cursor is moving quickly across the screen,
  // instead waiting for the mouse to begin a dwell.
  std::unique_ptr<base::RetainingOneShotTimer> start_gesture_timer_;
  std::unique_ptr<AutoclickRingHandler> autoclick_ring_handler_;
  std::unique_ptr<AutoclickDragEventRewriter> drag_event_rewriter_;
  std::unique_ptr<AutoclickMenuBubbleController> menu_bubble_controller_;

  DISALLOW_COPY_AND_ASSIGN(AutoclickController);
};

}  // namespace ash

#endif  // ASH_AUTOCLICK_AUTOCLICK_CONTROLLER_H
