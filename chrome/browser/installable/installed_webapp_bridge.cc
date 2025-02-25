// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/installable/installed_webapp_bridge.h"

#include <utility>

#include "base/android/jni_string.h"
#include "base/android/jni_utils.h"
#include "components/content_settings/core/common/content_settings.h"
#include "jni/InstalledWebappBridge_jni.h"

using base::android::ConvertJavaStringToUTF8;
using base::android::ScopedJavaLocalRef;

InstalledWebappProvider::RuleList
InstalledWebappBridge::GetInstalledWebappNotificationPermissions() {
  JNIEnv* env = base::android::AttachCurrentThread();
  ScopedJavaLocalRef<jobjectArray> j_permissions =
      Java_InstalledWebappBridge_getNotificationPermissions(env);
  jsize size = env->GetArrayLength(j_permissions.obj());

  InstalledWebappProvider::RuleList rules;
  for (jsize i = 0; i < size; i++) {
    ScopedJavaLocalRef<jobject> j_permission(
        env, env->GetObjectArrayElement(j_permissions.obj(), i));

    GURL origin(ConvertJavaStringToUTF8(
        Java_InstalledWebappBridge_getOriginFromPermission(env, j_permission)));
    ContentSetting setting = IntToContentSetting(
        Java_InstalledWebappBridge_getSettingFromPermission(env, j_permission));
    rules.push_back(std::make_pair(origin, setting));
  }

  return rules;
}
