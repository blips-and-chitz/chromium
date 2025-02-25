// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tabmodel;

import android.support.annotation.NonNull;

import org.chromium.base.ObserverList;
import org.chromium.chrome.browser.tab.Tab;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * This class is responsible for filtering tabs from {@link TabModel}. The filtering logic is
 * delegated to the concrete class that extends this abstract class. If no filter is active, this
 * class has the same {@link TabList} as {@link TabModel} does.
 *
 * If there is at least one filter active, this is a {@link TabList} that contains the most
 * important tabs that the filter defines.
 */
public abstract class TabModelFilter extends EmptyTabModelObserver implements TabList {
    private static final List<Tab> sEmptyRelatedTabList =
            Collections.unmodifiableList(new ArrayList<Tab>());
    private TabModel mTabModel;
    protected ObserverList<TabModelObserver> mFilteredObservers = new ObserverList<>();

    public TabModelFilter(TabModel tabModel) {
        mTabModel = tabModel;
        mTabModel.addObserver(this);
    }

    /**
     * Adds a {@link TabModelObserver} to be notified on {@link TabModelFilter} changes.
     * @param observer The {@link TabModelObserver} to add.
     */
    public void addObserver(TabModelObserver observer) {
        mFilteredObservers.addObserver(observer);
    }

    /**
     * Removes a {@link TabModelObserver}.
     * @param observer The {@link TabModelObserver} to remove.
     */
    public void removeObserver(TabModelObserver observer) {
        mFilteredObservers.removeObserver(observer);
    }

    public boolean isCurrentlySelectedFilter() {
        return mTabModel.isCurrentModel();
    }

    /**
     * To be called when this filter should be destroyed. This filter should no longer be used after
     * this.
     */
    public void destroy() {
        mFilteredObservers.clear();
    }

    /**
     * @return The {@link TabModel} that the filter is acting on.
     */
    protected TabModel getTabModel() {
        return mTabModel;
    }

    /**
     * Any of the concrete class can override and define a relationship that links a {@link Tab} to
     * a list of related {@link Tab}s. By default, this returns an unmodifiable list that only
     * contains the {@link Tab} with the given id. Note that the meaning of related can vary
     * depending on the filter being applied.
     * @param tabId Id of the {@link Tab} try to relate with.
     * @return An unmodifiable list of {@link Tab} that relate with the given tab id.
     */
    @NonNull
    public List<Tab> getRelatedTabList(int tabId) {
        Tab tab = TabModelUtils.getTabById(getTabModel(), tabId);
        if (tab == null) return sEmptyRelatedTabList;
        List<Tab> relatedTab = new ArrayList<>();
        relatedTab.add(tab);
        return Collections.unmodifiableList(relatedTab);
    }

    /**
     * Concrete class requires to define what's the behavior when {@link TabModel} added a
     * {@link Tab}.
     * @param tab {@link Tab} had added to {@link TabModel}.
     */
    protected abstract void addTab(Tab tab);

    /**
     * Concrete class requires to define what's the behavior when {@link TabModel} closed a
     * {@link Tab}.
     * @param tab {@link Tab} had closed from {@link TabModel}.
     */
    protected abstract void closeTab(Tab tab);

    /**
     * Concrete class requires to define what's the behavior when {@link TabModel} selected a
     * {@link Tab}.
     * @param tab {@link Tab} had selected.
     */
    protected abstract void selectTab(Tab tab);

    /**
     * Concrete class requires to define the ordering of each Tab within the filter.
     */
    protected abstract void reorder();

    // TabModelObserver implementation.
    @Override
    public void didSelectTab(Tab tab, int type, int lastId) {
        selectTab(tab);
        for (TabModelObserver observer : mFilteredObservers) {
            observer.didSelectTab(tab, type, lastId);
        }
    }

    @Override
    public void willCloseTab(Tab tab, boolean animate) {
        closeTab(tab);
        for (TabModelObserver observer : mFilteredObservers) {
            observer.willCloseTab(tab, animate);
        }
    }

    @Override
    public void didCloseTab(int tabId, boolean incognito) {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.didCloseTab(tabId, incognito);
        }
    }

    @Override
    public void willAddTab(Tab tab, int type) {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.willAddTab(tab, type);
        }
    }

    @Override
    public void didAddTab(Tab tab, int type) {
        addTab(tab);
        for (TabModelObserver observer : mFilteredObservers) {
            observer.didAddTab(tab, type);
        }
    }

    @Override
    public void didMoveTab(Tab tab, int newIndex, int curIndex) {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.didMoveTab(tab, newIndex, curIndex);
        }
    }

    @Override
    public void tabPendingClosure(Tab tab) {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.tabPendingClosure(tab);
        }
    }

    @Override
    public void tabClosureUndone(Tab tab) {
        addTab(tab);
        reorder();
        for (TabModelObserver observer : mFilteredObservers) {
            observer.tabClosureUndone(tab);
        }
    }

    @Override
    public void tabClosureCommitted(Tab tab) {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.tabClosureCommitted(tab);
        }
    }

    @Override
    public void multipleTabsPendingClosure(List<Tab> tabs, boolean isAllTabs) {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.multipleTabsPendingClosure(tabs, isAllTabs);
        }
    }

    @Override
    public void allTabsClosureCommitted() {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.allTabsClosureCommitted();
        }
    }

    @Override
    public void tabRemoved(Tab tab) {
        for (TabModelObserver observer : mFilteredObservers) {
            observer.tabRemoved(tab);
        }
    }

    @Override
    public void restoreCompleted() {
        if (getCount() != 0) reorder();

        for (TabModelObserver observer : mFilteredObservers) {
            observer.restoreCompleted();
        }
    }
}
