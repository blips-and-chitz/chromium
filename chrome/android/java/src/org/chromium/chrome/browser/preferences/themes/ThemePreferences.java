// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.preferences.themes;

import static org.chromium.chrome.browser.preferences.ChromePreferenceManager.UI_THEME_SETTING_KEY;

import android.os.Build;
import android.os.Bundle;
import android.preference.PreferenceFragment;
import android.support.annotation.IntDef;
import android.support.annotation.Nullable;
import android.widget.ListView;

import org.chromium.chrome.R;
import org.chromium.chrome.browser.preferences.ChromePreferenceManager;
import org.chromium.chrome.browser.preferences.PreferenceUtils;
import org.chromium.ui.UiUtils;

import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;

/**
 * Fragment to manage the theme user settings.
 */
public class ThemePreferences extends PreferenceFragment {
    @IntDef({ThemeSetting.SYSTEM_DEFAULT, ThemeSetting.LIGHT, ThemeSetting.DARK})
    @Retention(RetentionPolicy.SOURCE)
    public @interface ThemeSetting {
        int SYSTEM_DEFAULT = 0;
        int LIGHT = 1;
        int DARK = 2;
    }

    static final String PREF_UI_THEME_PREF = "ui_theme_pref";

    @Override
    public void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        PreferenceUtils.addPreferencesFromResource(this, R.xml.theme_preferences);
        getActivity().setTitle(getResources().getString(R.string.prefs_themes));

        RadioButtonGroupThemePreference radioButtonGroupThemePreference =
                (RadioButtonGroupThemePreference) findPreference(PREF_UI_THEME_PREF);
        radioButtonGroupThemePreference.initialize(
                ChromePreferenceManager.getInstance().readInt(UI_THEME_SETTING_KEY));
        radioButtonGroupThemePreference.setOnPreferenceChangeListener((preference, newValue) -> {
            int theme = (int) newValue;
            ChromePreferenceManager.getInstance().writeInt(UI_THEME_SETTING_KEY, theme);
            return true;
        });
    }

    @Override
    public void onActivityCreated(Bundle savedInstanceState) {
        super.onActivityCreated(savedInstanceState);

        // On O_MR1, the flag View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR in this fragment is not
        // updated to the attribute android:windowLightNavigationBar set in preference theme, so
        // we set the flag explicitly to workaround the issue. See https://crbug.com/942551.
        if (Build.VERSION.SDK_INT == Build.VERSION_CODES.O_MR1) {
            UiUtils.setNavigationBarIconColor(getActivity().getWindow().getDecorView(),
                    getResources().getBoolean(R.bool.window_light_navigation_bar));
        }

        ListView listView = getView().findViewById(android.R.id.list);
        listView.setDivider(null);
    }
}