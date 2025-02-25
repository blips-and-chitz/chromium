// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/arc/notification/arc_boot_error_notification.h"

#include <memory>
#include <utility>

#include "ash/public/cpp/notification_utils.h"
#include "ash/public/cpp/vector_icons/vector_icons.h"
#include "base/bind.h"
#include "base/macros.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chromeos/policy/browser_policy_connector_chromeos.h"
#include "chrome/browser/notifications/notification_display_service.h"
#include "chrome/browser/notifications/notification_display_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/grit/generated_resources.h"
#include "components/arc/arc_browser_context_keyed_service_factory_base.h"
#include "components/user_manager/user_manager.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/chromeos/resources/grit/ui_chromeos_resources.h"
#include "ui/message_center/public/cpp/notification.h"
#include "ui/message_center/public/cpp/notification_types.h"
#include "ui/message_center/public/cpp/notifier_id.h"

namespace arc {

namespace {

const char kLowDiskSpaceId[] = "arc_low_disk";
const char kNotifierId[] = "arc_boot_error";
const char kStoragePage[] = "storage";

void ShowLowDiskSpaceErrorNotification(content::BrowserContext* context) {
  // We suppress the low-disk notification when there are multiple users on an
  // enterprise managed device. crbug.com/656788.
  if (g_browser_process->platform_part()
          ->browser_policy_connector_chromeos()
          ->IsEnterpriseManaged() &&
      user_manager::UserManager::Get()->GetUsers().size() > 1) {
    LOG(WARNING) << "ARC booting is aborted due to low disk space, but the "
                 << "notification was suppressed on a managed device.";
    return;
  }
  message_center::ButtonInfo storage_settings(
      l10n_util::GetStringUTF16(IDS_LOW_DISK_NOTIFICATION_BUTTON));
  message_center::RichNotificationData optional_fields;
  optional_fields.buttons.push_back(storage_settings);

  message_center::NotifierId notifier_id(
      message_center::NotifierType::SYSTEM_COMPONENT, kNotifierId);
  const AccountId& account_id =
      user_manager::UserManager::Get()->GetPrimaryUser()->GetAccountId();
  notifier_id.profile_id = account_id.GetUserEmail();

  Profile* profile = Profile::FromBrowserContext(context);
  std::unique_ptr<message_center::Notification> notification =
      ash::CreateSystemNotification(
          message_center::NOTIFICATION_TYPE_SIMPLE, kLowDiskSpaceId,
          l10n_util::GetStringUTF16(
              IDS_ARC_CRITICALLY_LOW_DISK_NOTIFICATION_TITLE),
          l10n_util::GetStringUTF16(
              IDS_ARC_CRITICALLY_LOW_DISK_NOTIFICATION_MESSAGE),
          l10n_util::GetStringUTF16(IDS_ARC_NOTIFICATION_DISPLAY_SOURCE),
          GURL(), notifier_id, optional_fields,
          base::MakeRefCounted<message_center::HandleNotificationClickDelegate>(
              base::BindRepeating(
                  [](Profile* profile, base::Optional<int> button_index) {
                    if (button_index) {
                      DCHECK_EQ(0, *button_index);
                      chrome::ShowSettingsSubPageForProfile(profile,
                                                            kStoragePage);
                    }
                  },
                  profile)),
          ash::kNotificationStorageFullIcon,
          message_center::SystemNotificationWarningLevel::CRITICAL_WARNING);
  NotificationDisplayService::GetForProfile(profile)->Display(
      NotificationHandler::Type::TRANSIENT, *notification,
      /*metadata=*/nullptr);
}

// Singleton factory for ArcBootErrorNotificationFactory.
class ArcBootErrorNotificationFactory
    : public internal::ArcBrowserContextKeyedServiceFactoryBase<
          ArcBootErrorNotification,
          ArcBootErrorNotificationFactory> {
 public:
  // Factory name used by ArcBrowserContextKeyedServiceFactoryBase.
  static constexpr const char* kName = "ArcBootErrorNotificationFactory";

  static ArcBootErrorNotificationFactory* GetInstance() {
    return base::Singleton<ArcBootErrorNotificationFactory>::get();
  }

 private:
  friend base::DefaultSingletonTraits<ArcBootErrorNotificationFactory>;
  ArcBootErrorNotificationFactory() {
    DependsOn(NotificationDisplayServiceFactory::GetInstance());
  }
  ~ArcBootErrorNotificationFactory() override = default;
};

}  // namespace

// static
ArcBootErrorNotification* ArcBootErrorNotification::GetForBrowserContext(
    content::BrowserContext* context) {
  return ArcBootErrorNotificationFactory::GetForBrowserContext(context);
}

ArcBootErrorNotification::ArcBootErrorNotification(
    content::BrowserContext* context,
    ArcBridgeService* bridge_service)
    : context_(context) {
  ArcSessionManager::Get()->AddObserver(this);
}

ArcBootErrorNotification::~ArcBootErrorNotification() {
  ArcSessionManager::Get()->RemoveObserver(this);
}

void ArcBootErrorNotification::OnArcSessionStopped(ArcStopReason reason) {
  if (reason == ArcStopReason::LOW_DISK_SPACE)
    ShowLowDiskSpaceErrorNotification(context_);
}

}  // namespace arc
