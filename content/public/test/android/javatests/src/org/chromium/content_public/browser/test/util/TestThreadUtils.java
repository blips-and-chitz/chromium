// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.content_public.browser.test.util;

import org.chromium.base.ThreadUtils;
import org.chromium.base.task.PostTask;
import org.chromium.base.task.SingleThreadTaskRunner;
import org.chromium.base.task.TaskTraits;
import org.chromium.content_public.browser.UiThreadTaskTraits;

import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;

/**
 * Helper methods to deal with threading related tasks.
 */
public class TestThreadUtils {
    /**
     * Run the supplied Runnable on the main thread. The method will block until the Runnable
     * completes.
     *
     * @param r The Runnable to run.
     */
    public static void runOnUiThreadBlocking(final Runnable r) {
        runBlockingWithTraits(UiThreadTaskTraits.DEFAULT, r);
    }

    /**
     * Run the supplied Runnable with the provided traits. The method will block until the Runnable
     * completes.
     *
     * @param traits The Traits to run.
     * @param r The Runnable to run.
     */
    public static void runBlockingWithTraits(final TaskTraits traits, final Runnable r) {
        SingleThreadTaskRunner taskRunner = PostTask.createSingleThreadTaskRunner(traits);
        if (taskRunner.belongsToCurrentThread()) {
            r.run();
        } else {
            FutureTask<Void> task = new FutureTask<Void>(r, null);
            taskRunner.postTask(task);
            try {
                task.get();
            } catch (Exception e) {
                throw new RuntimeException("Exception occurred while waiting for runnable", e);
            }
        }
    }

    /**
     * Run the supplied Callable on the main thread, wrapping any exceptions in a RuntimeException.
     * The method will block until the Callable completes.
     *
     * @param c The Callable to run
     * @return The result of the callable
     */
    public static <T> T runOnUiThreadBlockingNoException(Callable<T> c) {
        try {
            return runOnUiThreadBlocking(c);
        } catch (ExecutionException e) {
            throw new RuntimeException("Error occurred waiting for callable", e);
        }
    }

    /**
     * Run the supplied Callable on the main thread, The method will block until the Callable
     * completes.
     *
     * @param c The Callable to run
     * @return The result of the callable
     * @throws ExecutionException c's exception
     */
    public static <T> T runOnUiThreadBlocking(Callable<T> c) throws ExecutionException {
        return runBlockingWithTraits(UiThreadTaskTraits.DEFAULT, c);
    }

    /**
     * Run the supplied Callable with the given task traits, The method will block until the
     * Callable completes.
     *
     * @param traits The Traits to run
     * @param c The Callable to run
     * @return The result of the callable
     * @throws ExecutionException c's exception
     */
    public static <T> T runBlockingWithTraits(final TaskTraits traits, Callable<T> c)
            throws ExecutionException {
        FutureTask<T> task = new FutureTask<T>(c);
        PostTask.runOrPostTask(traits, task);
        try {
            return task.get();
        } catch (InterruptedException e) {
            throw new RuntimeException("Interrupted waiting for callable", e);
        }
    }

    /**
     * Disables thread asserts.
     *
     * Can be used by tests where code that normally runs multi-threaded is going to run
     * single-threaded for the test (otherwise asserts that are valid in production would fail in
     * those tests).
     */
    public static void setThreadAssertsDisabled(boolean disabled) {
        ThreadUtils.setThreadAssertsDisabledForTesting(disabled);
    }
}
