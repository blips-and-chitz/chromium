// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_LOGIN_LOCK_SCREEN_LOCKER_TESTER_H_
#define CHROME_BROWSER_CHROMEOS_LOGIN_LOCK_SCREEN_LOCKER_TESTER_H_

#include <memory>
#include <string>

#include "ash/public/interfaces/login_screen_test_api.test-mojom.h"
#include "chrome/browser/chromeos/login/test/login_screen_tester.h"

class AccountId;

namespace chromeos {

// ScreenLockerTester provides a high-level API to test the lock screen.
class ScreenLockerTester {
 public:
  ScreenLockerTester();
  ~ScreenLockerTester();

  // Synchronously lock the device.
  void Lock();

  // Injects authenticators that only authenticate with the given password.
  void SetUnlockPassword(const AccountId& account_id,
                         const std::string& password);

  // Returns true if the screen is locked.
  bool IsLocked();

  // Returns true if Restart button is visible.
  bool IsLockRestartButtonShown();

  // Returns true if Shutdown button is visible.
  bool IsLockShutdownButtonShown();

  // Enters and submits the given password for the given account.
  void UnlockWithPassword(const AccountId& account_id,
                          const std::string& password);

  // LoginScreenTester proxy methods:
  int64_t GetUiUpdateCount();
  void WaitForUiUpdate(int64_t previous_update_count);

 private:
  test::LoginScreenTester login_screen_tester_;
  ash::mojom::LoginScreenTestApiPtr test_api_;

  DISALLOW_COPY_AND_ASSIGN(ScreenLockerTester);
};

}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_LOGIN_LOCK_SCREEN_LOCKER_TESTER_H_
