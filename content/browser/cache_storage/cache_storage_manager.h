// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_CACHE_STORAGE_CACHE_STORAGE_MANAGER_H_
#define CONTENT_BROWSER_CACHE_STORAGE_CACHE_STORAGE_MANAGER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/macros.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "content/browser/cache_storage/cache_storage.h"
#include "content/browser/cache_storage/cache_storage_context_impl.h"
#include "content/common/content_export.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/cache_storage_context.h"
#include "content/public/browser/storage_usage_info.h"
#include "storage/browser/quota/quota_client.h"
#include "url/origin.h"

namespace base {
class SequencedTaskRunner;
}

namespace storage {
class BlobStorageContext;
class QuotaManagerProxy;
}

namespace content {

class CacheStorageQuotaClient;

namespace cache_storage_manager_unittest {
class CacheStorageManagerTest;
}

enum class CacheStorageOwner {
  kMinValue,

  // Caches that can be accessed by the JS CacheStorage API (developer facing).
  kCacheAPI = kMinValue,

  // Private cache to store background fetch downloads.
  kBackgroundFetch,

  kMaxValue = kBackgroundFetch
};

// Keeps track of a CacheStorage per origin. There is one
// CacheStorageManager per ServiceWorkerContextCore.
// TODO(jkarlin): Remove CacheStorage from memory once they're no
// longer in active use.
class CONTENT_EXPORT CacheStorageManager
    : public base::RefCountedThreadSafe<CacheStorageManager,
                                        BrowserThread::DeleteOnIOThread> {
 public:
  static scoped_refptr<CacheStorageManager> Create(
      const base::FilePath& path,
      scoped_refptr<base::SequencedTaskRunner> cache_task_runner,
      scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy);

  static scoped_refptr<CacheStorageManager> Create(
      CacheStorageManager* old_manager);

  // Map a database identifier (computed from an origin) to the path.
  static base::FilePath ConstructOriginPath(const base::FilePath& root_path,
                                            const url::Origin& origin,
                                            CacheStorageOwner owner);

  // Open the CacheStorage for the given origin and owner.  A reference counting
  // handle is returned which can be stored and used similar to a weak pointer.
  CacheStorageHandle OpenCacheStorage(const url::Origin& origin,
                                      CacheStorageOwner owner);

  // This must be called before creating any of the public *Cache functions
  // above.
  void SetBlobParametersForCache(
      base::WeakPtr<storage::BlobStorageContext> blob_storage_context);

  void AddObserver(CacheStorageContextImpl::Observer* observer);
  void RemoveObserver(CacheStorageContextImpl::Observer* observer);

  void NotifyCacheListChanged(const url::Origin& origin);
  void NotifyCacheContentChanged(const url::Origin& origin,
                                 const std::string& name);

  base::WeakPtr<CacheStorageManager> AsWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

  base::FilePath root_path() const { return root_path_; }

  // This method is called when the last CacheStorageHandle for a particular
  // instance is destroyed and its reference count drops to zero.
  void CacheStorageUnreferenced(LegacyCacheStorage* cache_storage,
                                const url::Origin& origin,
                                CacheStorageOwner owner);

  static bool IsValidQuotaOrigin(const url::Origin& origin);

 private:
  friend class base::DeleteHelper<CacheStorageManager>;
  friend class base::RefCountedThreadSafe<CacheStorageManager>;
  friend class cache_storage_manager_unittest::CacheStorageManagerTest;
  friend class CacheStorageContextImpl;
  friend class CacheStorageQuotaClient;
  friend struct BrowserThread::DeleteOnThread<BrowserThread::IO>;

  typedef std::map<std::pair<url::Origin, CacheStorageOwner>,
                   std::unique_ptr<LegacyCacheStorage>>
      CacheStorageMap;

  CacheStorageManager(
      const base::FilePath& path,
      scoped_refptr<base::SequencedTaskRunner> cache_task_runner,
      scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy);

  virtual ~CacheStorageManager();

  // QuotaClient and Browsing Data Deletion support
  void GetAllOriginsUsage(CacheStorageOwner owner,
                          CacheStorageContext::GetUsageInfoCallback callback);
  void GetAllOriginsUsageGetSizes(
      std::unique_ptr<std::vector<StorageUsageInfo>> usage_info,
      CacheStorageContext::GetUsageInfoCallback callback);

  void GetOriginUsage(const url::Origin& origin_url,
                      CacheStorageOwner owner,
                      storage::QuotaClient::GetUsageCallback callback);
  void GetOrigins(CacheStorageOwner owner,
                  storage::QuotaClient::GetOriginsCallback callback);
  void GetOriginsForHost(const std::string& host,
                         CacheStorageOwner owner,
                         storage::QuotaClient::GetOriginsCallback callback);
  void DeleteOriginData(const url::Origin& origin,
                        CacheStorageOwner owner,
                        storage::QuotaClient::DeletionCallback callback);
  void DeleteOriginData(const url::Origin& origin, CacheStorageOwner owner);
  void DeleteOriginDidClose(const url::Origin& origin,
                            CacheStorageOwner owner,
                            storage::QuotaClient::DeletionCallback callback,
                            std::unique_ptr<LegacyCacheStorage> cache_storage,
                            int64_t origin_size);

  base::WeakPtr<storage::BlobStorageContext> blob_storage_context() const {
    return blob_context_;
  }

  scoped_refptr<base::SequencedTaskRunner> cache_task_runner() const {
    return cache_task_runner_;
  }

  bool IsMemoryBacked() const { return root_path_.empty(); }

  // MemoryPressureListener callback
  void OnMemoryPressure(
      base::MemoryPressureListener::MemoryPressureLevel level);

  base::FilePath root_path_;
  scoped_refptr<base::SequencedTaskRunner> cache_task_runner_;

  scoped_refptr<storage::QuotaManagerProxy> quota_manager_proxy_;

  // The map owns the CacheStorages and the CacheStorages are only accessed on
  // |cache_task_runner_|.
  CacheStorageMap cache_storage_map_;

  base::ObserverList<CacheStorageContextImpl::Observer>::Unchecked observers_;

  base::WeakPtr<storage::BlobStorageContext> blob_context_;

  std::unique_ptr<base::MemoryPressureListener> memory_pressure_listener_;

  base::WeakPtrFactory<CacheStorageManager> weak_ptr_factory_;
  DISALLOW_COPY_AND_ASSIGN(CacheStorageManager);
};

}  // namespace content

#endif  // CONTENT_BROWSER_CACHE_STORAGE_CACHE_STORAGE_MANAGER_H_
