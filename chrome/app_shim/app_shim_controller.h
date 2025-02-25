// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_APP_SHIM_APP_SHIM_CONTROLLER_H_
#define CHROME_APP_SHIM_APP_SHIM_CONTROLLER_H_

#import <AppKit/AppKit.h>

#include "base/files/file_path.h"
#include "base/mac/scoped_nsobject.h"
#include "chrome/common/mac/app_mode_common.h"
#include "chrome/common/mac/app_shim.mojom.h"
#include "chrome/common/mac/app_shim_param_traits.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "mojo/public/cpp/platform/named_platform_channel.h"
#include "mojo/public/cpp/system/isolated_connection.h"

@class AppShimDelegate;

// The AppShimController is responsible for communication with the main Chrome
// process, and generally controls the lifetime of the app shim process.
class AppShimController : public chrome::mojom::AppShim {
 public:
  explicit AppShimController(
      const app_mode::ChromeAppModeInfo* app_mode_info,
      base::scoped_nsobject<NSRunningApplication> chrome_running_app);
  ~AppShimController() override;

  chrome::mojom::AppShimHost* host() const { return host_.get(); }

  // Called when the app is activated, e.g. by clicking on it in the dock, by
  // dropping a file on the dock icon, or by Cmd+Tabbing to it.
  // Returns whether the message was sent.
  bool SendFocusApp(apps::AppShimFocusType focus_type,
                    const std::vector<base::FilePath>& files);

 private:
  // Create a channel from the Mojo |endpoint| and send a LaunchApp message.
  void CreateChannelAndSendLaunchApp(mojo::PlatformChannelEndpoint endpoint);
  // Builds main menu bar items.
  void SetUpMenu();
  void ChannelError(uint32_t custom_reason, const std::string& description);
  void BootstrapChannelError(uint32_t custom_reason,
                             const std::string& description);
  void LaunchAppDone(apps::AppShimLaunchResult result,
                     chrome::mojom::AppShimRequest app_shim_request);

  // chrome::mojom::AppShim implementation.
  void CreateViewsBridgeFactory(
      views_bridge_mac::mojom::BridgeFactoryAssociatedRequest request) override;
  void CreateContentNSViewBridgeFactory(
      content::mojom::NSViewBridgeFactoryAssociatedRequest request) override;
  void CreateCommandDispatcherForWidget(uint64_t widget_id) override;
  void Hide() override;
  void SetBadgeLabel(const std::string& badge_label) override;
  void UnhideWithoutActivation() override;
  void SetUserAttention(apps::AppShimAttentionType attention_type) override;

  // Terminates the app shim process.
  void Close();

  // Connects to Chrome and sends a LaunchApp message.
  void InitBootstrapPipe();

  // Check to see if Chrome's AppShimHostManager has been initialized. If it
  // has, then connect.
  void PollForChromeReady(const base::TimeDelta& time_until_timeout);

  const app_mode::ChromeAppModeInfo* const app_mode_info_;
  base::scoped_nsobject<NSRunningApplication> chrome_running_app_;

  mojo::IsolatedConnection bootstrap_mojo_connection_;
  chrome::mojom::AppShimHostBootstrapPtr host_bootstrap_;

  mojo::Binding<chrome::mojom::AppShim> shim_binding_;
  chrome::mojom::AppShimHostPtr host_;
  chrome::mojom::AppShimHostRequest host_request_;

  base::scoped_nsobject<AppShimDelegate> delegate_;
  bool launch_app_done_;
  NSInteger attention_request_id_;

  DISALLOW_COPY_AND_ASSIGN(AppShimController);
};

#endif  // CHROME_APP_SHIM_APP_SHIM_CONTROLLER_H_
