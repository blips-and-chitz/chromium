// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
package org.chromium.chrome.browser.download.home.rename;

import android.content.Context;
import android.view.LayoutInflater;

import org.chromium.base.Callback;
import org.chromium.ui.modaldialog.DialogDismissalCause;
import org.chromium.ui.modaldialog.ModalDialogManager;
import org.chromium.ui.modaldialog.ModalDialogProperties;
import org.chromium.ui.modelutil.PropertyModel;

/**
 * The Coordinator for the Rename Extension Dialog. Manages UI objects like views and model, and
 * handles communication with the {@link ModalDialogManager}.
 */
public class RenameDialogCoordinator {
    private final ModalDialogManager mModalDialogManager;
    private final PropertyModel mRenameDialogModel;
    private final RenameDialogCustomView mRenameDialogCustomView;
    private final Callback<Boolean> mOnClickEventCallback;
    private final Callback<Integer> mOnDismissEventCallback;

    public RenameDialogCoordinator(Context context, ModalDialogManager modalDialogManager,
            Callback<Boolean> onClickCallback,
            Callback</*DialogDismissalCause*/ Integer> dismissCallback) {
        mModalDialogManager = modalDialogManager;
        mRenameDialogCustomView = (RenameDialogCustomView) LayoutInflater.from(context).inflate(
                org.chromium.chrome.download.R.layout.download_rename_custom_dialog, null);
        mRenameDialogModel =
                new PropertyModel.Builder(ModalDialogProperties.ALL_KEYS)
                        .with(ModalDialogProperties.CONTROLLER, new RenameDialogController())
                        .with(ModalDialogProperties.TITLE,
                                context.getString(org.chromium.chrome.download.R.string.rename))
                        .with(ModalDialogProperties.CUSTOM_VIEW, mRenameDialogCustomView)
                        .with(ModalDialogProperties.POSITIVE_BUTTON_TEXT, context.getResources(),
                                org.chromium.chrome.download.R.string.ok)
                        .with(ModalDialogProperties.NEGATIVE_BUTTON_TEXT, context.getResources(),
                                org.chromium.chrome.download.R.string.cancel)
                        .build();
        mOnClickEventCallback = onClickCallback;
        mOnDismissEventCallback = dismissCallback;
    }

    public void destroy() {
        dismissDialog(DialogDismissalCause.ACTIVITY_DESTROYED);
    }

    public String getCurSuggestedName() {
        return mRenameDialogCustomView.getTargetName();
    }

    /**
     * Initialize rename dialog view and its sub-components, {@link ModalDialogManager}.
     * @param name The content to initialize display on EditTextBox.
     */
    public void showDialog(String name) {
        mModalDialogManager.showDialog(
                mRenameDialogModel, ModalDialogManager.ModalDialogType.APP, true);
        mRenameDialogCustomView.initializeView(name);
    }

    /**
     * Update rename dialog view and its sub-components, {@link ModalDialogManager}.
     * @param name The content to update the display on EditText box.
     * @param error {@RenameResult} Error message to display on subtitle view.
     */
    public void showDialogWithErrorMessage(String name, int /*RenameResult*/ error) {
        mRenameDialogCustomView.updateToErrorView(name, error);
        if (!mModalDialogManager.isShowing()) {
            mModalDialogManager.showDialog(
                    mRenameDialogModel, ModalDialogManager.ModalDialogType.APP, true);
        }
    }

    public void dismissDialog(int dismissalCause) {
        if (mModalDialogManager != null) {
            mModalDialogManager.dismissDialog(mRenameDialogModel, dismissalCause);
        }
    }

    private class RenameDialogController implements ModalDialogProperties.Controller {
        @Override
        public void onDismiss(PropertyModel model, int dismissalCause) {
            mOnDismissEventCallback.onResult(dismissalCause);
        }

        @Override
        public void onClick(PropertyModel model, int buttonType) {
            switch (buttonType) {
                case ModalDialogProperties.ButtonType.POSITIVE:
                    mOnClickEventCallback.onResult(true);
                    break;
                case ModalDialogProperties.ButtonType.NEGATIVE:
                    mOnClickEventCallback.onResult(false);
                    break;
                default:
                    break;
            }
        }
    }
}
