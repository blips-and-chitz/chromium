// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/device/geolocation/wifi_data_provider_chromeos.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_task_environment.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/shill/shill_manager_client.h"
#include "chromeos/network/geolocation_handler.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

namespace device {

class GeolocationChromeOsWifiDataProviderTest : public testing::Test {
 protected:
  GeolocationChromeOsWifiDataProviderTest()
      : scoped_task_environment_(
            base::test::ScopedTaskEnvironment::MainThreadType::UI) {}

  void SetUp() override {
    chromeos::DBusThreadManager::Initialize();
    chromeos::NetworkHandler::Initialize();
    manager_client_ =
        chromeos::DBusThreadManager::Get()->GetShillManagerClient();
    manager_test_ = manager_client_->GetTestInterface();
    provider_ = new WifiDataProviderChromeOs();
    base::RunLoop().RunUntilIdle();
  }

  void TearDown() override {
    provider_ = NULL;
    chromeos::NetworkHandler::Shutdown();
    chromeos::DBusThreadManager::Shutdown();
  }

  bool GetAccessPointData() { return provider_->GetAccessPointData(&ap_data_); }

  void AddAccessPoints(int ssids, int aps_per_ssid) {
    for (int i = 0; i < ssids; ++i) {
      for (int j = 0; j < aps_per_ssid; ++j) {
        base::DictionaryValue properties;
        std::string mac_address = base::StringPrintf(
            "%02X:%02X:%02X:%02X:%02X:%02X", i, j, 3, 4, 5, 6);
        std::string channel = base::NumberToString(i * 10 + j);
        std::string strength = base::NumberToString(i * 100 + j);
        properties.SetKey(shill::kGeoMacAddressProperty,
                          base::Value(mac_address));
        properties.SetKey(shill::kGeoChannelProperty, base::Value(channel));
        properties.SetKey(shill::kGeoSignalStrengthProperty,
                          base::Value(strength));
        manager_test_->AddGeoNetwork(shill::kGeoWifiAccessPointsProperty,
                                     properties);
      }
    }
    base::RunLoop().RunUntilIdle();
  }

  base::test::ScopedTaskEnvironment scoped_task_environment_;
  scoped_refptr<WifiDataProviderChromeOs> provider_;
  chromeos::ShillManagerClient* manager_client_;
  chromeos::ShillManagerClient::TestInterface* manager_test_;
  WifiData::AccessPointDataSet ap_data_;
};

TEST_F(GeolocationChromeOsWifiDataProviderTest, NoAccessPoints) {
  base::RunLoop().RunUntilIdle();
  // Initial call to GetAccessPointData requests data and will return false.
  EXPECT_FALSE(GetAccessPointData());
  base::RunLoop().RunUntilIdle();
  // Additional call to GetAccessPointData also returns false with no devices.
  EXPECT_FALSE(GetAccessPointData());
  EXPECT_EQ(0u, ap_data_.size());
}

TEST_F(GeolocationChromeOsWifiDataProviderTest, GetOneAccessPoint) {
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(GetAccessPointData());

  AddAccessPoints(1, 1);
  EXPECT_TRUE(GetAccessPointData());
  ASSERT_EQ(1u, ap_data_.size());
  EXPECT_EQ("00:00:03:04:05:06",
            base::UTF16ToUTF8(ap_data_.begin()->mac_address));
}

TEST_F(GeolocationChromeOsWifiDataProviderTest, GetManyAccessPoints) {
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(GetAccessPointData());

  AddAccessPoints(3, 4);
  EXPECT_TRUE(GetAccessPointData());
  ASSERT_EQ(12u, ap_data_.size());
}

}  // namespace device
