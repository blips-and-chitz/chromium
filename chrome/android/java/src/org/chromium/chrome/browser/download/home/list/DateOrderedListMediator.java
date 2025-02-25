// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.download.home.list;

import android.content.Intent;
import android.os.Handler;
import android.support.annotation.Nullable;
import android.support.v4.util.Pair;

import org.chromium.base.Callback;
import org.chromium.base.CollectionUtil;
import org.chromium.chrome.browser.ChromeApplication;
import org.chromium.chrome.browser.download.home.DownloadManagerUiConfig;
import org.chromium.chrome.browser.download.home.JustNowProvider;
import org.chromium.chrome.browser.download.home.OfflineItemSource;
import org.chromium.chrome.browser.download.home.filter.DeleteUndoOfflineItemFilter;
import org.chromium.chrome.browser.download.home.filter.Filters.FilterType;
import org.chromium.chrome.browser.download.home.filter.InvalidStateOfflineItemFilter;
import org.chromium.chrome.browser.download.home.filter.OffTheRecordOfflineItemFilter;
import org.chromium.chrome.browser.download.home.filter.OfflineItemFilter;
import org.chromium.chrome.browser.download.home.filter.OfflineItemFilterObserver;
import org.chromium.chrome.browser.download.home.filter.OfflineItemFilterSource;
import org.chromium.chrome.browser.download.home.filter.SearchOfflineItemFilter;
import org.chromium.chrome.browser.download.home.filter.TypeOfflineItemFilter;
import org.chromium.chrome.browser.download.home.glue.OfflineContentProviderGlue;
import org.chromium.chrome.browser.download.home.glue.ThumbnailRequestGlue;
import org.chromium.chrome.browser.download.home.list.DateOrderedListCoordinator.DateOrderedListObserver;
import org.chromium.chrome.browser.download.home.list.DateOrderedListCoordinator.DeleteController;
import org.chromium.chrome.browser.download.home.metrics.OfflineItemStartupLogger;
import org.chromium.chrome.browser.download.home.metrics.UmaUtils;
import org.chromium.chrome.browser.download.home.metrics.UmaUtils.ImagesMenuAction;
import org.chromium.chrome.browser.download.home.metrics.UmaUtils.ViewAction;
import org.chromium.chrome.browser.widget.ThumbnailProvider;
import org.chromium.chrome.browser.widget.ThumbnailProvider.ThumbnailRequest;
import org.chromium.chrome.browser.widget.ThumbnailProviderImpl;
import org.chromium.chrome.browser.widget.selection.SelectionDelegate;
import org.chromium.components.offline_items_collection.OfflineContentProvider;
import org.chromium.components.offline_items_collection.OfflineItem;
import org.chromium.components.offline_items_collection.OfflineItemShareInfo;
import org.chromium.components.offline_items_collection.OfflineItemState;
import org.chromium.components.offline_items_collection.VisualsCallback;

import java.io.Closeable;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;

/**
 * A Mediator responsible for converting an OfflineContentProvider to a list of items in downloads
 * home.  This includes support for filtering, deleting, etc..
 */
class DateOrderedListMediator {
    /** Helper interface for handling share requests by the UI. */
    @FunctionalInterface
    public interface ShareController {
        /**
         * Will be called whenever {@link OfflineItem}s are being requested to be shared by the UI.
         * @param intent The {@link Intent} representing the share action to broadcast to Android.
         */
        void share(Intent intent);
    }

    /**
     * Helper interface for handling rename requests by the UI, allows implementers of the
     * RenameController to finish the asynchronous rename operation.
     */
    @FunctionalInterface
    public interface RenameCallback {
        /**
         * Calling this will asynchronously attempt to commit a new name.
         * @param newName String representing the new name user designated to rename the item.
         * @param callback A callback that will pass to the backend to determine the validation
         *         result.
         */
        void tryToRename(String newName, Callback</*RenameResult*/ Integer> callback);
    }

    /** Helper interface for handling rename requests by the UI. */
    @FunctionalInterface
    public interface RenameController {
        /**
         * Will be called whenever {@link OfflineItem}s are being requested to be renamed by the UI.
         * @param name representing new name user designated to rename the item.
         */
        void rename(String name, RenameCallback result);
    }

    private final Handler mHandler = new Handler();

    private final OfflineContentProviderGlue mProvider;
    private final ShareController mShareController;
    private final ListItemModel mModel;
    private final DeleteController mDeleteController;
    private final RenameController mRenameController;

    private final OfflineItemSource mSource;
    private final DateOrderedListMutator mListMutator;
    private final ThumbnailProvider mThumbnailProvider;
    private final MediatorSelectionObserver mSelectionObserver;
    private final SelectionDelegate<ListItem> mSelectionDelegate;
    private final DownloadManagerUiConfig mUiConfig;

    private final OffTheRecordOfflineItemFilter mOffTheRecordFilter;
    private final InvalidStateOfflineItemFilter mInvalidStateFilter;
    private final DeleteUndoOfflineItemFilter mDeleteUndoFilter;
    private final TypeOfflineItemFilter mTypeFilter;
    private final SearchOfflineItemFilter mSearchFilter;

    /**
     * A selection observer that correctly updates the selection state for each item in the list.
     */
    private class MediatorSelectionObserver
            implements SelectionDelegate.SelectionObserver<ListItem> {
        private final SelectionDelegate<ListItem> mSelectionDelegate;

        public MediatorSelectionObserver(SelectionDelegate<ListItem> delegate) {
            mSelectionDelegate = delegate;
            mSelectionDelegate.addObserver(this);
        }

        @Override
        public void onSelectionStateChange(List<ListItem> selectedItems) {
            for (int i = 0; i < mModel.size(); i++) {
                ListItem item = mModel.get(i);
                boolean selected = mSelectionDelegate.isItemSelected(item);
                item.showSelectedAnimation = selected && !item.selected;
                item.selected = selected;
                mModel.update(i, item);
            }
            mModel.dispatchLastEvent();
            mModel.getProperties().set(
                    ListProperties.SELECTION_MODE_ACTIVE, mSelectionDelegate.isSelectionEnabled());
        }
    }

    /**
     * Creates an instance of a DateOrderedListMediator that will push {@code provider} into
     * {@code model}.
     * @param provider                The {@link OfflineContentProvider} to visually represent.
     * @param deleteController        A class to manage whether or not items can be deleted.
     * @param shareController         A class responsible for sharing downloaded item {@link
     *                                Intent}s.
     * @param selectionDelegate       A class responsible for handling list item selection.
     * @param config                  A {@link DownloadManagerUiConfig} to provide UI config params.
     * @param dateOrderedListObserver An observer of the list and recycler view.
     * @param model                   The {@link ListItemModel} to push {@code provider} into.
     */
    public DateOrderedListMediator(OfflineContentProvider provider, ShareController shareController,
            DeleteController deleteController, RenameController renameController,
            SelectionDelegate<ListItem> selectionDelegate, DownloadManagerUiConfig config,
            DateOrderedListObserver dateOrderedListObserver, ListItemModel model) {
        // Build a chain from the data source to the model.  The chain will look like:
        // [OfflineContentProvider] ->
        //     [OfflineItemSource] ->
        //         [OffTheRecordOfflineItemFilter] ->
        //             [InvalidStateOfflineItemFilter] ->
        //                 [DeleteUndoOfflineItemFilter] ->
        //                     [SearchOfflineItemFitler] ->
        //                         [TypeOfflineItemFilter] ->
        //                             [DateOrderedListMutator] ->
        //                                 [ListItemModel]

        mProvider = new OfflineContentProviderGlue(provider, config);
        mShareController = shareController;
        mModel = model;
        mDeleteController = deleteController;
        mRenameController = renameController;
        mSelectionDelegate = selectionDelegate;
        mUiConfig = config;

        mSource = new OfflineItemSource(mProvider);
        mOffTheRecordFilter = new OffTheRecordOfflineItemFilter(config.isOffTheRecord, mSource);
        mInvalidStateFilter = new InvalidStateOfflineItemFilter(mOffTheRecordFilter);
        mDeleteUndoFilter = new DeleteUndoOfflineItemFilter(mInvalidStateFilter);
        mSearchFilter = new SearchOfflineItemFilter(mDeleteUndoFilter);
        mTypeFilter = new TypeOfflineItemFilter(mSearchFilter);
        mListMutator = new DateOrderedListMutator(
                mTypeFilter, mModel, config, new JustNowProvider(config));

        new OfflineItemStartupLogger(config, mInvalidStateFilter);

        mSearchFilter.addObserver(new EmptyStateObserver(mSearchFilter, dateOrderedListObserver));
        mThumbnailProvider = new ThumbnailProviderImpl(ChromeApplication.getReferencePool(),
                config.inMemoryThumbnailCacheSizeBytes,
                ThumbnailProviderImpl.ClientType.DOWNLOAD_HOME);
        mSelectionObserver = new MediatorSelectionObserver(selectionDelegate);

        mModel.getProperties().set(ListProperties.ENABLE_ITEM_ANIMATIONS, true);
        mModel.getProperties().set(ListProperties.CALLBACK_OPEN, this ::onOpenItem);
        mModel.getProperties().set(ListProperties.CALLBACK_PAUSE, this ::onPauseItem);
        mModel.getProperties().set(ListProperties.CALLBACK_RESUME, this ::onResumeItem);
        mModel.getProperties().set(ListProperties.CALLBACK_CANCEL, this ::onCancelItem);
        mModel.getProperties().set(ListProperties.CALLBACK_SHARE, this ::onShareItem);
        mModel.getProperties().set(ListProperties.CALLBACK_SHARE_ALL, this ::onShareItems);
        mModel.getProperties().set(ListProperties.CALLBACK_REMOVE, this ::onDeleteItem);
        mModel.getProperties().set(ListProperties.CALLBACK_REMOVE_ALL, this ::onDeleteItems);
        mModel.getProperties().set(ListProperties.PROVIDER_VISUALS, this ::getVisuals);
        mModel.getProperties().set(ListProperties.CALLBACK_SELECTION, this ::onSelection);
        mModel.getProperties().set(ListProperties.CALLBACK_RENAME,
                mUiConfig.isRenameEnabled ? this::onRenameItem : null);
        mModel.getProperties().set(
                ListProperties.CALLBACK_START_SELECTION, this ::onStartSelection);
    }

    /** Tears down this mediator. */
    public void destroy() {
        mSource.destroy();
        mProvider.destroy();
        mThumbnailProvider.destroy();
    }

    /**
     * To be called when this mediator should filter its content based on {@code filter}.
     * @see TypeOfflineItemFilter#onFilterSelected(int)
     */
    public void onFilterTypeSelected(@FilterType int filter) {
        mListMutator.onFilterTypeSelected(filter);
        try (AnimationDisableClosable closeable = new AnimationDisableClosable()) {
            mTypeFilter.onFilterSelected(filter);
        }
    }

    /**
     * To be called when this mediator should filter its content based on {@code filter}.
     * @see SearchOfflineItemFilter#onQueryChanged(String)
     */
    public void onFilterStringChanged(String filter) {
        try (AnimationDisableClosable closeable = new AnimationDisableClosable()) {
            mSearchFilter.onQueryChanged(filter);
        }
    }

    /**
     * Called to delete the list of currently selected items.
     * @return The number of items that were deleted.
     */
    public int deleteSelectedItems() {
        deleteItemsInternal(ListUtils.toOfflineItems(mSelectionDelegate.getSelectedItems()));
        int itemCount = mSelectionDelegate.getSelectedItems().size();
        mSelectionDelegate.clearSelection();
        return itemCount;
    }

    /**
     * Called to share the list of currently selected items.
     * @return The number of items that were shared.
     */
    public int shareSelectedItems() {
        shareItemsInternal(ListUtils.toOfflineItems(mSelectionDelegate.getSelectedItems()));
        int itemCount = mSelectionDelegate.getSelectedItems().size();
        mSelectionDelegate.clearSelection();
        return itemCount;
    }

    /** Called to handle a back press event. */
    public boolean handleBackPressed() {
        if (mSelectionDelegate.isSelectionEnabled()) {
            mSelectionDelegate.clearSelection();
            return true;
        }
        return false;
    }

    /**
     * @return The {@link OfflineItemFilterSource} that should be used to determine which filter
     *         options are available.
     */
    public OfflineItemFilterSource getFilterSource() {
        return mSearchFilter;
    }

    /**
     * @return The {@link OfflineItemFilterSource} that should be used to determine whether there
     * are no items and empty view should be shown.
     */
    public OfflineItemFilterSource getEmptySource() {
        return mTypeFilter;
    }

    private void onSelection(@Nullable ListItem item) {
        mSelectionDelegate.toggleSelectionForItem(item);
    }

    private void onStartSelection() {
        // We are hard coding that this is coming from the Photos section, as that is the only
        // one that supports a section menu.  If that changes we need to support a wider array
        // of metrics.
        UmaUtils.recordImagesMenuAction(ImagesMenuAction.MENU_START_SELECTING);
        mSelectionDelegate.setSelectionModeEnabledForZeroItems(true);
    }

    private void onOpenItem(OfflineItem item) {
        UmaUtils.recordItemAction(ViewAction.OPEN);
        mProvider.openItem(item);
    }

    private void onPauseItem(OfflineItem item) {
        UmaUtils.recordItemAction(ViewAction.PAUSE);
        mProvider.pauseDownload(item);
    }

    private void onResumeItem(OfflineItem item) {
        UmaUtils.recordItemAction(ViewAction.RESUME);
        mProvider.resumeDownload(item, true /* hasUserGesture */);
    }

    private void onCancelItem(OfflineItem item) {
        UmaUtils.recordItemAction(ViewAction.CANCEL);
        mProvider.cancelDownload(item);
    }

    private void onDeleteItem(OfflineItem item) {
        UmaUtils.recordItemAction(ViewAction.MENU_DELETE);
        deleteItemsInternal(CollectionUtil.newArrayList(item));
    }

    private void onShareItem(OfflineItem item) {
        UmaUtils.recordItemAction(ViewAction.MENU_SHARE);
        shareItemsInternal(CollectionUtil.newHashSet(item));
    }

    private void onShareItems(List<OfflineItem> items) {
        // We are hard coding that this is coming from the Photos section, as that is the only
        // one that supports a section menu.  If that changes we need to support a wider array
        // of metrics.
        UmaUtils.recordImagesMenuAction(ImagesMenuAction.MENU_SHARE_ALL);
        shareItemsInternal(items);
    }

    private void onDeleteItems(List<OfflineItem> items) {
        // We are hard coding that this is coming from the Photos section, as that is the only
        // one that supports a section menu.  If that changes we need to support a wider array
        // of metrics.
        UmaUtils.recordImagesMenuAction(ImagesMenuAction.MENU_DELETE_ALL);
        deleteItemsInternal(items);
    }

    private void onRenameItem(OfflineItem item) {
        // TODO(hesen): Add uma stats.
        mRenameController.rename(item.title, (newName, renameCallback) -> {
            mProvider.renameItem(item, newName, renameCallback);
        });
    }

    /**
     * Deletes a given list of items. If the items are not completed yet, they would be cancelled.
     * @param items The list of items to delete.
     */
    private void deleteItemsInternal(List<OfflineItem> items) {
        // Calculate the real offline items we are going to remove here.
        final Collection<OfflineItem> itemsToDelete =
                ItemUtils.findItemsWithSameFilePath(items, mSource.getItems());

        mDeleteUndoFilter.addPendingDeletions(itemsToDelete);
        mDeleteController.canDelete(items, delete -> {
            if (delete) {
                for (OfflineItem item : itemsToDelete) {
                    if (item.state != OfflineItemState.COMPLETE) {
                        mProvider.cancelDownload(item);
                    } else {
                        mProvider.removeItem(item);
                    }

                    // Remove and have a single decision path for cleaning up thumbnails when the
                    // glue layer is no longer needed.
                    mProvider.removeVisualsForItem(mThumbnailProvider, item.id);
                }
            } else {
                mDeleteUndoFilter.removePendingDeletions(itemsToDelete);
            }
        });
    }

    private void shareItemsInternal(Collection<OfflineItem> items) {
        UmaUtils.recordItemsShared(items);

        final Collection<Pair<OfflineItem, OfflineItemShareInfo>> shareInfo = new ArrayList<>();
        for (OfflineItem item : items) {
            mProvider.getShareInfoForItem(item, (id, info) -> {
                shareInfo.add(Pair.create(item, info));

                // When we've gotten callbacks for all items, create and share the intent.
                if (shareInfo.size() == items.size()) {
                    Intent intent = ShareUtils.createIntent(shareInfo);
                    if (intent != null) mShareController.share(intent);
                }
            });
        }
    }

    private Runnable getVisuals(
            OfflineItem item, int iconWidthPx, int iconHeightPx, VisualsCallback callback) {
        if (!UiUtils.canHaveThumbnails(item) || iconWidthPx == 0 || iconHeightPx == 0) {
            mHandler.post(() -> callback.onVisualsAvailable(item.id, null));
            return () -> {};
        }

        ThumbnailRequest request = new ThumbnailRequestGlue(mProvider, item, iconWidthPx,
                iconHeightPx, mUiConfig.maxThumbnailScaleFactor, callback);
        mThumbnailProvider.getThumbnail(request);
        return () -> mThumbnailProvider.cancelRetrieval(request);
    }

    /** Helper class to disable animations for certain list changes. */
    private class AnimationDisableClosable implements Closeable {
        AnimationDisableClosable() {
            mModel.getProperties().set(ListProperties.ENABLE_ITEM_ANIMATIONS, false);
        }

        // Closeable implementation.
        @Override
        public void close() {
            mHandler.post(() -> {
                mModel.getProperties().set(ListProperties.ENABLE_ITEM_ANIMATIONS, true);
            });
        }
    }

    /**
     * A helper class to observe the list content and notify the given observer when the list state
     * changes between empty and non-empty.
     */
    private static class EmptyStateObserver implements OfflineItemFilterObserver {
        private Boolean mIsEmpty;
        private final DateOrderedListObserver mDateOrderedListObserver;
        private final OfflineItemFilter mOfflineItemFilter;

        public EmptyStateObserver(OfflineItemFilter offlineItemFilter,
                DateOrderedListObserver dateOrderedListObserver) {
            mOfflineItemFilter = offlineItemFilter;
            mDateOrderedListObserver = dateOrderedListObserver;
            new Handler().post(() -> calculateEmptyState());
        }

        @Override
        public void onItemsAvailable() {
            calculateEmptyState();
        }

        @Override
        public void onItemsAdded(Collection<OfflineItem> items) {
            calculateEmptyState();
        }

        @Override
        public void onItemsRemoved(Collection<OfflineItem> items) {
            calculateEmptyState();
        }

        @Override
        public void onItemUpdated(OfflineItem oldItem, OfflineItem item) {
            calculateEmptyState();
        }

        private void calculateEmptyState() {
            Boolean isEmpty = mOfflineItemFilter.getItems().isEmpty();
            if (isEmpty.equals(mIsEmpty)) return;

            mIsEmpty = isEmpty;
            mDateOrderedListObserver.onEmptyStateChanged(mIsEmpty);
        }
    }
}
