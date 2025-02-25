// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_CHROMEOS_SYSTEM_WEB_DIALOG_DELEGATE_H_
#define CHROME_BROWSER_UI_WEBUI_CHROMEOS_SYSTEM_WEB_DIALOG_DELEGATE_H_

#include <string>
#include <vector>

#include "base/macros.h"
#include "base/strings/string16.h"
#include "ui/web_dialogs/web_dialog_delegate.h"
#include "url/gurl.h"

// WebDialogDelegate for system Web UI dialogs, e.g. dialogs opened from the
// Ash system tray. These dialogs are normally movable and draggable so that
// content from other pages can be copy-pasted, but kept always-on-top so that
// they do not get lost behind other windows. On screens that use an overlay
// like the login and lock screens, the dialog must be modal to be displayed on
// top of the overlay.

namespace chromeos {

class SystemWebDialogDelegate : public ui::WebDialogDelegate {
 public:
  // Returns the instance whose Id() matches |id|. If more than one instance
  // matches, the first matching instance created is returned.
  static SystemWebDialogDelegate* FindInstance(const std::string& id);

  // |gurl| is the HTML file path for the dialog content and must be set.
  // |title| may be empty in which case ShouldShowDialogTitle() returns false.
  SystemWebDialogDelegate(const GURL& gurl, const base::string16& title);
  ~SystemWebDialogDelegate() override;

  // Returns an identifier used for matching an instance in FindInstance.
  // By default returns gurl_.spec() which should be sufficient for dialogs
  // that only support a single instance.
  virtual const std::string& Id();

  // Focuses the dialog window. Note: No-op for modal dialogs, see
  // implementation for details.
  void Focus();

  // ui::WebDialogDelegate
  ui::ModalType GetDialogModalType() const override;
  base::string16 GetDialogTitle() const override;
  GURL GetDialogContentURL() const override;
  void GetWebUIMessageHandlers(
      std::vector<content::WebUIMessageHandler*>* handlers) const override;
  void GetDialogSize(gfx::Size* size) const override;
  bool CanResizeDialog() const override;
  std::string GetDialogArgs() const override;
  void OnDialogShown(content::WebUI* webui,
                     content::RenderViewHost* render_view_host) override;
  // Note: deletes |this|.
  void OnDialogClosed(const std::string& json_retval) override;
  void OnCloseContents(content::WebContents* source,
                       bool* out_close_dialog) override;
  bool ShouldShowDialogTitle() const override;

  // Shows a system dialog using the current ative profile.
  // If |parent| is not null, the dialog will be parented to |parent|.
  // Otherwise it will be attached to either the AlwaysOnTop container or the
  // LockSystemModal container, depending on the session state at creation.
  void ShowSystemDialog(gfx::NativeWindow parent = nullptr);

  content::WebUI* GetWebUIForTest() { return webui_; }

  // Width is consistent with the Settings UI.
  static constexpr int kDialogWidth = 512;
  static constexpr int kDialogHeight = 480;

 protected:
  FRIEND_TEST_ALL_PREFIXES(SystemWebDialogLoginTest, NonModalTest);

  // Returns the dialog window (pointer to |aura::Window|). This will be a
  // |nullptr| if the dialog has not been created yet.
  gfx::NativeWindow dialog_window() const { return dialog_window_; }

 private:
  GURL gurl_;
  base::string16 title_;
  content::WebUI* webui_ = nullptr;
  ui::ModalType modal_type_;
  gfx::NativeWindow dialog_window_ = nullptr;

  DISALLOW_COPY_AND_ASSIGN(SystemWebDialogDelegate);
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_UI_WEBUI_CHROMEOS_SYSTEM_WEB_DIALOG_DELEGATE_H_
