// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview
 * Closure compiler type definitions used by display_manager.js .
 */

var DisplayManagerScreenAttributes = {};

/**
 * True when screen should have size matched with others.
 * (i.e. it's a part of main flow)
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.commonScreenSize;

/**
 * True if showing "enable debugging" is allowed for the screen.
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.enableDebuggingAllowed;

/**
 * True if enabling demo mode is allowed for the screen.
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.enterDemoModeAllowed;

/**
 * True if screen does not use left-current-right animation.
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.noAnimatedTransition;

/**
 * True if enrollment accelerator should schedule postponed enrollment.
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.postponeEnrollmentAllowed;

/**
 * True if device reset is allowed on the screen.
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.resetAllowed;

/**
 * True if enrollment accelerator should start enrollment.
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.startEnrollmentAllowed;

/**
 * True if "enable kiosk" accelerator is allowed.
 * @type {boolean|undefined}
 */
DisplayManagerScreenAttributes.toggleKioskAllowed;

