// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROMEOS_DBUS_SHILL_FAKE_GSM_SMS_CLIENT_H_
#define CHROMEOS_DBUS_SHILL_FAKE_GSM_SMS_CLIENT_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chromeos/dbus/shill/gsm_sms_client.h"
#include "dbus/object_path.h"

namespace chromeos {

// A fake implementation of GsmSMSClient used for tests.
class COMPONENT_EXPORT(CHROMEOS_DBUS) FakeGsmSMSClient : public GsmSMSClient {
 public:
  FakeGsmSMSClient();
  ~FakeGsmSMSClient() override;

  // GsmSMSClient overrides
  void SetSmsReceivedHandler(const std::string& service_name,
                             const dbus::ObjectPath& object_path,
                             const SmsReceivedHandler& handler) override;
  void ResetSmsReceivedHandler(const std::string& service_name,
                               const dbus::ObjectPath& object_path) override;
  void Delete(const std::string& service_name,
              const dbus::ObjectPath& object_path,
              uint32_t index,
              VoidDBusMethodCallback callback) override;
  void Get(const std::string& service_name,
           const dbus::ObjectPath& object_path,
           uint32_t index,
           DBusMethodCallback<base::DictionaryValue> callback) override;
  void List(const std::string& service_name,
            const dbus::ObjectPath& object_path,
            DBusMethodCallback<base::ListValue> callback) override;
  void RequestUpdate(const std::string& service_name,
                     const dbus::ObjectPath& object_path) override;

 private:
  void PushTestMessageChain();
  void PushTestMessageDelayed();
  bool PushTestMessage();

  int test_index_;
  std::vector<std::string> test_messages_;
  base::ListValue message_list_;
  SmsReceivedHandler handler_;

  bool sms_test_message_switch_present_;

  base::WeakPtrFactory<FakeGsmSMSClient> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(FakeGsmSMSClient);
};

}  // namespace chromeos

#endif  // CHROMEOS_DBUS_SHILL_FAKE_GSM_SMS_CLIENT_H_
