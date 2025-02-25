// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.browserservices.permissiondelegation;

import static org.junit.Assert.assertEquals;

import android.support.test.InstrumentationRegistry;
import android.support.test.filters.MediumTest;

import org.junit.After;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.chromium.base.library_loader.LibraryLoader;
import org.chromium.base.library_loader.LibraryProcessType;
import org.chromium.base.library_loader.ProcessInitException;
import org.chromium.base.test.util.CommandLineFlags;
import org.chromium.chrome.browser.ChromeSwitches;
import org.chromium.chrome.browser.browserservices.Origin;
import org.chromium.chrome.browser.customtabs.CustomTabActivityTestRule;
import org.chromium.chrome.browser.customtabs.CustomTabsTestUtils;
import org.chromium.chrome.test.ChromeJUnit4ClassRunner;
import org.chromium.net.test.EmbeddedTestServer;
import org.chromium.net.test.ServerCertificate;

import java.util.concurrent.TimeoutException;

/**
 * Tests that cached permissions for Trusted Web Activities have an effect on the actual permission
 * state.
 */
@RunWith(ChromeJUnit4ClassRunner.class)
@CommandLineFlags.Add({ChromeSwitches.DISABLE_FIRST_RUN_EXPERIENCE})
public class TrustedWebActivityPermissionsTest {
    @Rule
    public CustomTabActivityTestRule mCustomTabActivityTestRule = new CustomTabActivityTestRule();

    private static final String TEST_PAGE = "/chrome/test/data/android/google.html";
    private EmbeddedTestServer mTestServer;
    private String mTestPage;
    private Origin mOrigin;

    @Before
    public void setUp() throws InterruptedException, ProcessInitException, TimeoutException {
        // Native needs to be initialized to start the test server.
        LibraryLoader.getInstance().ensureInitialized(LibraryProcessType.PROCESS_BROWSER);

        // TWAs only work with HTTPS.
        mTestServer = EmbeddedTestServer.createAndStartHTTPSServer(
                InstrumentationRegistry.getInstrumentation().getContext(),
                ServerCertificate.CERT_OK);
        mTestPage = mTestServer.getURL(TEST_PAGE);
        mOrigin = new Origin(mTestPage);

        mCustomTabActivityTestRule.startCustomTabActivityWithIntent(
                CustomTabsTestUtils.createMinimalCustomTabIntent(
                        InstrumentationRegistry.getTargetContext(), mTestPage));

        TrustedWebActivityPermissionManager.clearForTesting();
        assertEquals("\"default\"", getNotificationPermission());
    }

    @After
    public void tearDown() throws TimeoutException {
        TrustedWebActivityPermissionManager.clearForTesting();
        mTestServer.stopAndDestroyServer();
    }

    @Test
    @MediumTest
    public void allowNotifications() throws TimeoutException, InterruptedException {
        TrustedWebActivityPermissionManager.register(mOrigin, true);
        assertEquals("\"granted\"", getNotificationPermission());
    }

    @Test
    @MediumTest
    public void blockNotifications() throws TimeoutException, InterruptedException {
        TrustedWebActivityPermissionManager.register(mOrigin, false);
        assertEquals("\"denied\"", getNotificationPermission());
    }

    @Test
    @MediumTest
    public void unregisterTwa() throws TimeoutException, InterruptedException {
        TrustedWebActivityPermissionManager.register(mOrigin, true);
        assertEquals("\"granted\"", getNotificationPermission());

        TrustedWebActivityPermissionManager.unregister(mOrigin);
        assertEquals("\"default\"", getNotificationPermission());
    }

    private String getNotificationPermission() throws TimeoutException, InterruptedException {
        return mCustomTabActivityTestRule.runJavaScriptCodeInCurrentTab("Notification.permission");
    }
}
