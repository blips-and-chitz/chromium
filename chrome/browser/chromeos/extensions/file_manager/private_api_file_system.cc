// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/extensions/file_manager/private_api_file_system.h"

#include <sys/statvfs.h>
#include <sys/xattr.h>

#include <algorithm>
#include <utility>

#include "base/bind.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/posix/eintr_wrapper.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/system/sys_info.h"
#include "base/task/post_task.h"
#include "base/task_runner_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/chromeos/drive/file_system_util.h"
#include "chrome/browser/chromeos/extensions/file_manager/event_router.h"
#include "chrome/browser/chromeos/extensions/file_manager/event_router_factory.h"
#include "chrome/browser/chromeos/extensions/file_manager/file_stream_md5_digester.h"
#include "chrome/browser/chromeos/extensions/file_manager/private_api_util.h"
#include "chrome/browser/chromeos/file_manager/fileapi_util.h"
#include "chrome/browser/chromeos/file_manager/path_util.h"
#include "chrome/browser/chromeos/file_manager/volume_manager.h"
#include "chrome/browser/chromeos/fileapi/file_system_backend.h"
#include "chrome/browser/metrics/chrome_metrics_service_accessor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/extensions/api/file_manager_private.h"
#include "chrome/common/extensions/api/file_manager_private_internal.h"
#include "chromeos/constants/chromeos_features.h"
#include "chromeos/disks/disk_mount_manager.h"
#include "components/drive/chromeos/file_system_interface.h"
#include "components/drive/drive.pb.h"
#include "components/drive/event_logger.h"
#include "components/drive/file_system_core_util.h"
#include "components/storage_monitor/storage_info.h"
#include "components/storage_monitor/storage_monitor.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/common/url_constants.h"
#include "extensions/browser/extension_util.h"
#include "net/base/escape.h"
#include "services/device/public/mojom/mtp_manager.mojom.h"
#include "storage/browser/fileapi/file_stream_reader.h"
#include "storage/browser/fileapi/file_system_context.h"
#include "storage/browser/fileapi/file_system_file_util.h"
#include "storage/browser/fileapi/file_system_operation_context.h"
#include "storage/browser/fileapi/file_system_operation_runner.h"
#include "storage/common/fileapi/file_system_info.h"
#include "storage/common/fileapi/file_system_types.h"
#include "storage/common/fileapi/file_system_util.h"
#include "third_party/cros_system_api/constants/cryptohome.h"

using chromeos::disks::DiskMountManager;
using content::BrowserThread;
using content::ChildProcessSecurityPolicy;
using file_manager::util::EntryDefinition;
using file_manager::util::FileDefinition;
using storage::FileSystemURL;

namespace extensions {
namespace {

const char kRootPath[] = "/";

// Retrieves total and remaining available size on |mount_path|.
void GetSizeStatsAsync(const base::FilePath& mount_path,
                       uint64_t* total_size,
                       uint64_t* remaining_size) {
  int64_t size = base::SysInfo::AmountOfTotalDiskSpace(mount_path);
  if (size >= 0)
    *total_size = size;
  size = base::SysInfo::AmountOfFreeDiskSpace(mount_path);
  if (size >= 0)
    *remaining_size = size;
}

// Retrieves the maximum file name length of the file system of |path|.
// Returns 0 if it could not be queried.
size_t GetFileNameMaxLengthAsync(const std::string& path) {
  struct statvfs stat = {};
  if (HANDLE_EINTR(statvfs(path.c_str(), &stat)) != 0) {
    // The filesystem seems not supporting statvfs(). Assume it to be a commonly
    // used bound 255, and log the failure.
    LOG(ERROR) << "Cannot statvfs() the name length limit for: " << path;
    return 255;
  }
  return stat.f_namemax;
}

bool GetFileExtendedAttribute(const base::FilePath& path,
                              const char* name,
                              std::vector<char>* value) {
  ssize_t len = getxattr(path.value().c_str(), name, nullptr, 0);
  if (len < 0) {
    PLOG_IF(ERROR, errno != ENODATA) << "getxattr: " << path;
    return false;
  }
  value->resize(len);
  if (getxattr(path.value().c_str(), name, value->data(), len) != len) {
    PLOG(ERROR) << "getxattr: " << path;
    return false;
  }
  return true;
}

// Returns EventRouter for the |profile_id| if available.
file_manager::EventRouter* GetEventRouterByProfileId(void* profile_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  // |profile_id| needs to be checked with ProfileManager::IsValidProfile
  // before using it.
  if (!g_browser_process->profile_manager()->IsValidProfile(profile_id))
    return nullptr;
  Profile* profile = reinterpret_cast<Profile*>(profile_id);

  return file_manager::EventRouterFactory::GetForProfile(profile);
}

// Notifies the copy progress to extensions via event router.
void NotifyCopyProgress(
    void* profile_id,
    storage::FileSystemOperationRunner::OperationID operation_id,
    storage::FileSystemOperation::CopyProgressType type,
    const FileSystemURL& source_url,
    const FileSystemURL& destination_url,
    int64_t size) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  file_manager::EventRouter* event_router =
      GetEventRouterByProfileId(profile_id);
  if (event_router) {
    event_router->OnCopyProgress(
        operation_id, type,
        source_url.ToGURL(), destination_url.ToGURL(), size);
  }
}

// Callback invoked periodically on progress update of Copy().
void OnCopyProgress(
    void* profile_id,
    storage::FileSystemOperationRunner::OperationID* operation_id,
    storage::FileSystemOperation::CopyProgressType type,
    const FileSystemURL& source_url,
    const FileSystemURL& destination_url,
    int64_t size) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::UI},
      base::BindOnce(&NotifyCopyProgress, profile_id, *operation_id, type,
                     source_url, destination_url, size));
}

// Notifies the copy completion to extensions via event router.
void NotifyCopyCompletion(
    void* profile_id,
    storage::FileSystemOperationRunner::OperationID operation_id,
    const FileSystemURL& source_url,
    const FileSystemURL& destination_url,
    base::File::Error error) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  file_manager::EventRouter* event_router =
      GetEventRouterByProfileId(profile_id);
  if (event_router)
    event_router->OnCopyCompleted(
        operation_id,
        source_url.ToGURL(), destination_url.ToGURL(), error);
}

// Callback invoked upon completion of Copy() (regardless of succeeded or
// failed).
void OnCopyCompleted(
    void* profile_id,
    storage::FileSystemOperationRunner::OperationID* operation_id,
    const FileSystemURL& source_url,
    const FileSystemURL& destination_url,
    base::File::Error error) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::UI},
      base::BindOnce(&NotifyCopyCompletion, profile_id, *operation_id,
                     source_url, destination_url, error));
}

// Starts the copy operation via FileSystemOperationRunner.
storage::FileSystemOperationRunner::OperationID StartCopyOnIOThread(
    void* profile_id,
    scoped_refptr<storage::FileSystemContext> file_system_context,
    const FileSystemURL& source_url,
    const FileSystemURL& destination_url) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  // Note: |operation_id| is owned by the callback for
  // FileSystemOperationRunner::Copy(). It is always called in the next message
  // loop or later, so at least during this invocation it should alive.
  //
  // TODO(yawano): change ERROR_BEHAVIOR_ABORT to ERROR_BEHAVIOR_SKIP after
  //     error messages of individual operations become appear in the Files app
  //     UI.
  storage::FileSystemOperationRunner::OperationID* operation_id =
      new storage::FileSystemOperationRunner::OperationID;
  *operation_id = file_system_context->operation_runner()->Copy(
      source_url, destination_url, storage::FileSystemOperation::OPTION_NONE,
      storage::FileSystemOperation::ERROR_BEHAVIOR_ABORT,
      base::BindRepeating(&OnCopyProgress, profile_id,
                          base::Unretained(operation_id)),
      base::BindOnce(&OnCopyCompleted, profile_id, base::Owned(operation_id),
                     source_url, destination_url));
  return *operation_id;
}

void OnCopyCancelled(base::File::Error error) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  // We just ignore the status if the copy is actually cancelled or not,
  // because failing cancellation means the operation is not running now.
  DLOG_IF(WARNING, error != base::File::FILE_OK)
      << "Failed to cancel copy: " << error;
}

// Cancels the running copy operation identified by |operation_id|.
void CancelCopyOnIOThread(
    scoped_refptr<storage::FileSystemContext> file_system_context,
    storage::FileSystemOperationRunner::OperationID operation_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  file_system_context->operation_runner()->Cancel(
      operation_id, base::BindOnce(&OnCopyCancelled));
}

// Converts a status code to a bool value and calls the |callback| with it.
void StatusCallbackToResponseCallback(
    const base::Callback<void(bool)>& callback,
    base::File::Error result) {
  callback.Run(result == base::File::FILE_OK);
}

// Calls a response callback (on the UI thread) with a file content hash
// computed on the IO thread.
void ComputeChecksumRespondOnUIThread(
    base::OnceCallback<void(const std::string&)> callback,
    const std::string& hash) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  base::PostTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                           base::BindOnce(std::move(callback), hash));
}

// Calls a response callback on the UI thread.
void GetFileMetadataRespondOnUIThread(
    storage::FileSystemOperation::GetMetadataCallback callback,
    base::File::Error result,
    const base::File::Info& file_info) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::UI},
      base::BindOnce(std::move(callback), result, file_info));
}

}  // namespace

ExtensionFunction::ResponseAction
FileManagerPrivateEnableExternalFileSchemeFunction::Run() {
  ChildProcessSecurityPolicy::GetInstance()->GrantRequestScheme(
      render_frame_host()->GetProcess()->GetID(), content::kExternalFileScheme);
  return RespondNow(NoArguments());
}

FileManagerPrivateGrantAccessFunction::FileManagerPrivateGrantAccessFunction()
    : chrome_details_(this) {
}

ExtensionFunction::ResponseAction FileManagerPrivateGrantAccessFunction::Run() {
  using extensions::api::file_manager_private::GrantAccess::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details_.GetProfile(), render_frame_host());

  storage::ExternalFileSystemBackend* const backend =
      file_system_context->external_backend();
  DCHECK(backend);

  const std::vector<Profile*>& profiles =
      g_browser_process->profile_manager()->GetLoadedProfiles();
  for (auto* profile : profiles) {
    if (profile->IsOffTheRecord())
      continue;
    const GURL site = util::GetSiteForExtensionId(extension_id(), profile);
    storage::FileSystemContext* const context =
        content::BrowserContext::GetStoragePartitionForSite(profile, site)
            ->GetFileSystemContext();
    for (const auto& url : params->entry_urls) {
      const storage::FileSystemURL file_system_url =
          context->CrackURL(GURL(url));
      // Grant permissions only to valid urls backed by the external file system
      // backend.
      if (!file_system_url.is_valid() ||
          file_system_url.mount_type() != storage::kFileSystemTypeExternal) {
        continue;
      }
      backend->GrantFileAccessToExtension(extension_->id(),
                                          file_system_url.virtual_path());
      content::ChildProcessSecurityPolicy::GetInstance()
          ->GrantCreateReadWriteFile(render_frame_host()->GetProcess()->GetID(),
                                     file_system_url.path());
    }
  }
  return RespondNow(NoArguments());
}

namespace {

void PostResponseCallbackTaskToUIThread(
    const FileWatchFunctionBase::ResponseCallback& callback,
    bool success) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  base::PostTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                           base::BindOnce(callback, success));
}

void PostNotificationCallbackTaskToUIThread(
    const storage::WatcherManager::NotificationCallback& callback,
    storage::WatcherManager::ChangeType type) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  base::PostTaskWithTraits(FROM_HERE, {BrowserThread::UI},
                           base::BindOnce(callback, type));
}

}  // namespace

void FileWatchFunctionBase::RespondWith(bool success) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  auto result_value = std::make_unique<base::Value>(success);
  if (success) {
    Respond(OneArgument(std::move(result_value)));
  } else {
    auto result_list = std::make_unique<base::ListValue>();
    result_list->Append(std::move(result_value));
    Respond(ErrorWithArguments(std::move(result_list), ""));
  }
}

ExtensionFunction::ResponseAction FileWatchFunctionBase::Run() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!render_frame_host() || !render_frame_host()->GetProcess())
    return RespondNow(Error("Invalid state"));

  // First param is url of a file to watch.
  std::string url;
  if (!args_->GetString(0, &url) || url.empty())
    return RespondNow(Error("Empty watch URL"));

  const ChromeExtensionFunctionDetails chrome_details(this);
  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details.GetProfile(), render_frame_host());

  const FileSystemURL file_system_url =
      file_system_context->CrackURL(GURL(url));
  if (file_system_url.path().empty()) {
    auto result_list = std::make_unique<base::ListValue>();
    result_list->Append(std::make_unique<base::Value>(false));
    return RespondNow(
        ErrorWithArguments(std::move(result_list), "Invalid URL"));
  }

  file_manager::EventRouter* const event_router =
      file_manager::EventRouterFactory::GetForProfile(
          chrome_details.GetProfile());

  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(&FileWatchFunctionBase::RunAsyncOnIOThread, this,
                     file_system_context, file_system_url,
                     event_router->GetWeakPtr()));
  return RespondLater();
}

void FileWatchFunctionBase::RunAsyncOnIOThread(
    scoped_refptr<storage::FileSystemContext> file_system_context,
    const storage::FileSystemURL& file_system_url,
    base::WeakPtr<file_manager::EventRouter> event_router) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  storage::WatcherManager* const watcher_manager =
      file_system_context->GetWatcherManager(file_system_url.type());

  if (!watcher_manager) {
    base::PostTaskWithTraits(
        FROM_HERE, {BrowserThread::UI},
        base::BindOnce(
            &FileWatchFunctionBase::PerformFallbackFileWatchOperationOnUIThread,
            this, file_system_url, event_router));
    return;
  }

  PerformFileWatchOperationOnIOThread(file_system_context, watcher_manager,
                                      file_system_url, event_router);
}

void FileManagerPrivateInternalAddFileWatchFunction::
    PerformFileWatchOperationOnIOThread(
        scoped_refptr<storage::FileSystemContext> file_system_context,
        storage::WatcherManager* watcher_manager,
        const storage::FileSystemURL& file_system_url,
        base::WeakPtr<file_manager::EventRouter> event_router) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  watcher_manager->AddWatcher(
      file_system_url, false /* recursive */,
      base::Bind(
          &StatusCallbackToResponseCallback,
          base::Bind(&PostResponseCallbackTaskToUIThread,
                     base::Bind(&FileWatchFunctionBase::RespondWith, this))),
      base::Bind(
          &PostNotificationCallbackTaskToUIThread,
          base::Bind(&file_manager::EventRouter::OnWatcherManagerNotification,
                     event_router, file_system_url, extension_id())));
}

void FileManagerPrivateInternalAddFileWatchFunction::
    PerformFallbackFileWatchOperationOnUIThread(
        const storage::FileSystemURL& file_system_url,
        base::WeakPtr<file_manager::EventRouter> event_router) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(event_router);

  // Obsolete. Fallback code if storage::WatcherManager is not implemented.
  event_router->AddFileWatch(
      file_system_url.path(), file_system_url.virtual_path(), extension_id(),
      base::BindOnce(&FileWatchFunctionBase::RespondWith, this));
}

void FileManagerPrivateInternalRemoveFileWatchFunction::
    PerformFileWatchOperationOnIOThread(
        scoped_refptr<storage::FileSystemContext> file_system_context,
        storage::WatcherManager* watcher_manager,
        const storage::FileSystemURL& file_system_url,
        base::WeakPtr<file_manager::EventRouter> event_router) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);

  watcher_manager->RemoveWatcher(
      file_system_url, false /* recursive */,
      base::Bind(
          &StatusCallbackToResponseCallback,
          base::Bind(&PostResponseCallbackTaskToUIThread,
                     base::Bind(&FileWatchFunctionBase::RespondWith, this))));
}

void FileManagerPrivateInternalRemoveFileWatchFunction::
    PerformFallbackFileWatchOperationOnUIThread(
        const storage::FileSystemURL& file_system_url,
        base::WeakPtr<file_manager::EventRouter> event_router) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  DCHECK(event_router);

  // Obsolete. Fallback code if storage::WatcherManager is not implemented.
  event_router->RemoveFileWatch(file_system_url.path(), extension_id());
  RespondWith(true);
}

ExtensionFunction::ResponseAction
FileManagerPrivateGetSizeStatsFunction::Run() {
  using extensions::api::file_manager_private::GetSizeStats::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  using file_manager::VolumeManager;
  using file_manager::Volume;
  const ChromeExtensionFunctionDetails chrome_details(this);
  VolumeManager* const volume_manager =
      VolumeManager::Get(chrome_details.GetProfile());
  if (!volume_manager)
    return RespondNow(Error("Invalid state"));

  base::WeakPtr<Volume> volume =
      volume_manager->FindVolumeById(params->volume_id);
  if (!volume.get())
    return RespondNow(Error("Volume not found"));

  if (volume->type() == file_manager::VOLUME_TYPE_GOOGLE_DRIVE &&
      !base::FeatureList::IsEnabled(chromeos::features::kDriveFs)) {
    drive::FileSystemInterface* file_system =
        drive::util::GetFileSystemByProfile(chrome_details.GetProfile());
    if (!file_system) {
      // |file_system| is NULL if Drive is disabled.
      // If stats couldn't be gotten for drive, result should be left
      // undefined. See comments in GetDriveAvailableSpaceCallback().
      return RespondNow(NoArguments());
    }

    file_system->GetAvailableSpace(base::BindOnce(
        &FileManagerPrivateGetSizeStatsFunction::OnGetDriveAvailableSpace,
        this));
  } else if (volume->type() == file_manager::VOLUME_TYPE_MTP) {
    // Resolve storage_name.
    storage_monitor::StorageMonitor* storage_monitor =
        storage_monitor::StorageMonitor::GetInstance();
    storage_monitor::StorageInfo info;
    storage_monitor->GetStorageInfoForPath(volume->mount_path(), &info);
    std::string storage_name;
    base::RemoveChars(info.location(), kRootPath, &storage_name);
    DCHECK(!storage_name.empty());

    // Get MTP StorageInfo.
    auto* manager = storage_monitor->media_transfer_protocol_manager();
    manager->GetStorageInfoFromDevice(
        storage_name,
        base::BindOnce(
            &FileManagerPrivateGetSizeStatsFunction::OnGetMtpAvailableSpace,
            this));
  } else {
    uint64_t* total_size = new uint64_t(0);
    uint64_t* remaining_size = new uint64_t(0);
    base::PostTaskWithTraitsAndReply(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&GetSizeStatsAsync, volume->mount_path(), total_size,
                       remaining_size),
        base::BindOnce(&FileManagerPrivateGetSizeStatsFunction::OnGetSizeStats,
                       this, base::Owned(total_size),
                       base::Owned(remaining_size)));
  }
  return RespondLater();
}

void FileManagerPrivateGetSizeStatsFunction::OnGetDriveAvailableSpace(
    drive::FileError error,
    int64_t bytes_total,
    int64_t bytes_used) {
  if (error == drive::FILE_ERROR_OK) {
    const uint64_t bytes_total_unsigned = bytes_total;
    // bytes_used can be larger than bytes_total (over quota).
    const uint64_t bytes_remaining_unsigned =
        std::max(bytes_total - bytes_used, int64_t(0));
    OnGetSizeStats(&bytes_total_unsigned, &bytes_remaining_unsigned);
  } else {
    // If stats couldn't be gotten for drive, result should be left undefined.
    Respond(NoArguments());
  }
}

void FileManagerPrivateGetSizeStatsFunction::OnGetMtpAvailableSpace(
    device::mojom::MtpStorageInfoPtr mtp_storage_info,
    const bool error) {
  if (error) {
    // If stats couldn't be gotten from MTP volume, result should be left
    // undefined same as we do for Drive.
    Respond(NoArguments());
    return;
  }

  const uint64_t max_capacity = mtp_storage_info->max_capacity;
  const uint64_t free_space_in_bytes = mtp_storage_info->free_space_in_bytes;
  OnGetSizeStats(&max_capacity, &free_space_in_bytes);
}

void FileManagerPrivateGetSizeStatsFunction::OnGetSizeStats(
    const uint64_t* total_size,
    const uint64_t* remaining_size) {
  std::unique_ptr<base::DictionaryValue> sizes(new base::DictionaryValue());

  sizes->SetDouble("totalSize", static_cast<double>(*total_size));
  sizes->SetDouble("remainingSize", static_cast<double>(*remaining_size));

  Respond(OneArgument(std::move(sizes)));
}

ExtensionFunction::ResponseAction
FileManagerPrivateInternalValidatePathNameLengthFunction::Run() {
  using extensions::api::file_manager_private_internal::ValidatePathNameLength::
      Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  const ChromeExtensionFunctionDetails chrome_details(this);
  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details.GetProfile(), render_frame_host());

  const storage::FileSystemURL file_system_url(
      file_system_context->CrackURL(GURL(params->parent_url)));
  if (!chromeos::FileSystemBackend::CanHandleURL(file_system_url))
    return RespondNow(Error("Invalid URL"));

  // No explicit limit on the length of Drive file names.
  if (file_system_url.type() == storage::kFileSystemTypeDrive) {
    return RespondNow(OneArgument(std::make_unique<base::Value>(true)));
  }

  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(&GetFileNameMaxLengthAsync,
                     file_system_url.path().AsUTF8Unsafe()),
      base::BindOnce(&FileManagerPrivateInternalValidatePathNameLengthFunction::
                         OnFilePathLimitRetrieved,
                     this, params->name.size()));
  return RespondLater();
}

void FileManagerPrivateInternalValidatePathNameLengthFunction::
    OnFilePathLimitRetrieved(size_t current_length, size_t max_length) {
  Respond(
      OneArgument(std::make_unique<base::Value>(current_length <= max_length)));
}

ExtensionFunction::ResponseAction
FileManagerPrivateFormatVolumeFunction::Run() {
  using extensions::api::file_manager_private::FormatVolume::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  using file_manager::VolumeManager;
  using file_manager::Volume;
  const ChromeExtensionFunctionDetails chrome_details(this);
  VolumeManager* const volume_manager =
      VolumeManager::Get(chrome_details.GetProfile());
  if (!volume_manager)
    return RespondNow(Error("Invalid state"));

  base::WeakPtr<Volume> volume =
      volume_manager->FindVolumeById(params->volume_id);
  if (!volume)
    return RespondNow(Error("Volume not found"));

  DiskMountManager::GetInstance()->FormatMountedDevice(
      volume->mount_path().AsUTF8Unsafe());
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
FileManagerPrivateRenameVolumeFunction::Run() {
  using extensions::api::file_manager_private::RenameVolume::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  using file_manager::VolumeManager;
  using file_manager::Volume;
  const ChromeExtensionFunctionDetails chrome_details(this);
  VolumeManager* const volume_manager =
      VolumeManager::Get(chrome_details.GetProfile());
  if (!volume_manager)
    return RespondNow(Error("Invalid state"));

  base::WeakPtr<Volume> volume =
      volume_manager->FindVolumeById(params->volume_id);
  if (!volume)
    return RespondNow(Error("Volume not found"));

  DiskMountManager::GetInstance()->RenameMountedDevice(
      volume->mount_path().AsUTF8Unsafe(), params->new_name);
  return RespondNow(NoArguments());
}

namespace {

// Obtains file size of URL.
void GetFileMetadataOnIOThread(
    scoped_refptr<storage::FileSystemContext> file_system_context,
    const FileSystemURL& url,
    int fields,
    storage::FileSystemOperation::GetMetadataCallback callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::IO);
  file_system_context->operation_runner()->GetMetadata(
      url, fields,
      base::BindOnce(&GetFileMetadataRespondOnUIThread, std::move(callback)));
}

// Gets the available space of the |path|.
int64_t GetLocalDiskSpace(const base::FilePath& path) {
  if (!base::PathExists(path)) {
    return std::numeric_limits<int64_t>::min();
  }
  return base::SysInfo::AmountOfFreeDiskSpace(path);
}

}  // namespace

FileManagerPrivateInternalStartCopyFunction::
    FileManagerPrivateInternalStartCopyFunction()
    : chrome_details_(this) {}

ExtensionFunction::ResponseAction
FileManagerPrivateInternalStartCopyFunction::Run() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  using extensions::api::file_manager_private_internal::StartCopy::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  if (params->url.empty() || params->parent_url.empty() ||
      params->new_name.empty()) {
    // Error code in format of DOMError.name.
    return RespondNow(Error("EncodingError"));
  }

  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details_.GetProfile(), render_frame_host());

  // |parent| may have a trailing slash if it is a root directory.
  std::string destination_url_string = params->parent_url;
  if (destination_url_string.back() != '/')
    destination_url_string += '/';
  destination_url_string += net::EscapePath(params->new_name);

  source_url_ = file_system_context->CrackURL(GURL(params->url));
  destination_url_ =
      file_system_context->CrackURL(GURL(destination_url_string));

  if (!source_url_.is_valid() || !destination_url_.is_valid()) {
    // Error code in format of DOMError.name.
    return RespondNow(Error("EncodingError"));
  }

  // Check how much space we need for the copy operation.
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(
          &GetFileMetadataOnIOThread, file_system_context, source_url_,
          storage::FileSystemOperation::GET_METADATA_FIELD_SIZE |
              storage::FileSystemOperation::GET_METADATA_FIELD_TOTAL_SIZE,
          base::BindOnce(&FileManagerPrivateInternalStartCopyFunction::
                             RunAfterGetFileMetadata,
                         this)));
  return RespondLater();
}

void FileManagerPrivateInternalStartCopyFunction::RunAfterGetFileMetadata(
    base::File::Error result,
    const base::File::Info& file_info) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (result != base::File::FILE_OK) {
    Respond(Error("NotFoundError"));
    return;
  }

  base::FilePath destination_dir;
  if (destination_url_.filesystem_id() ==
      drive::util::GetDriveMountPointPath(chrome_details_.GetProfile())
          .BaseName()
          .value()) {
    // Google Drive's cache is limited by the available space on the local disk.
    destination_dir = file_manager::util::GetMyFilesFolderForProfile(
        chrome_details_.GetProfile());
  } else {
    destination_dir = destination_url_.path().DirName();
  }

  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock()},
      base::BindOnce(&GetLocalDiskSpace, destination_dir),
      base::BindOnce(
          &FileManagerPrivateInternalStartCopyFunction::RunAfterCheckDiskSpace,
          this, file_info.size));
}

void FileManagerPrivateInternalStartCopyFunction::RunAfterCheckDiskSpace(
    int64_t space_needed,
    int64_t space_available) {
  if (space_available < 0) {
    // It might be a virtual path. In this case we just assume that it has
    // enough space.
    RunAfterFreeDiskSpace(true);
  } else if (destination_url_.filesystem_id() ==
                 file_manager::util::GetDownloadsMountPointName(
                     chrome_details_.GetProfile()) ||
             destination_url_.filesystem_id() ==
                 drive::util::GetDriveMountPointPath(
                     chrome_details_.GetProfile())
                     .BaseName()
                     .value()) {
    // If the destination directory is local hard drive or Google Drive we
    // must leave some additional space to make sure we don't break the system.
    if (space_available - cryptohome::kMinFreeSpaceInBytes > space_needed) {
      RunAfterFreeDiskSpace(true);
    } else {
      // Also we can try to secure needed space by freeing Drive caches.
      drive::FileSystemInterface* const drive_file_system =
          drive::util::GetFileSystemByProfile(chrome_details_.GetProfile());
      if (!drive_file_system) {
        RunAfterFreeDiskSpace(false);
      } else {
        drive_file_system->FreeDiskSpaceIfNeededFor(
            space_needed,
            base::Bind(&FileManagerPrivateInternalStartCopyFunction::
                           RunAfterFreeDiskSpace,
                       this));
      }
    }
  } else {
    RunAfterFreeDiskSpace(space_available > space_needed);
  }
}

void FileManagerPrivateInternalStartCopyFunction::RunAfterFreeDiskSpace(
    bool available) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  if (!available) {
    Respond(Error("QuotaExceededError"));
    return;
  }

  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details_.GetProfile(), render_frame_host());
  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(&StartCopyOnIOThread, chrome_details_.GetProfile(),
                     file_system_context, source_url_, destination_url_),
      base::BindOnce(
          &FileManagerPrivateInternalStartCopyFunction::RunAfterStartCopy,
          this));
}

void FileManagerPrivateInternalStartCopyFunction::RunAfterStartCopy(
    int operation_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  Respond(OneArgument(std::make_unique<base::Value>(operation_id)));
}

ExtensionFunction::ResponseAction FileManagerPrivateCancelCopyFunction::Run() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);

  using extensions::api::file_manager_private::CancelCopy::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  const ChromeExtensionFunctionDetails chrome_details(this);
  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details.GetProfile(), render_frame_host());

  // We don't much take care about the result of cancellation.
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(&CancelCopyOnIOThread, file_system_context,
                     params->copy_id));
  return RespondNow(NoArguments());
}

ExtensionFunction::ResponseAction
FileManagerPrivateInternalResolveIsolatedEntriesFunction::Run() {
  using extensions::api::file_manager_private_internal::ResolveIsolatedEntries::
      Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  const ChromeExtensionFunctionDetails chrome_details(this);
  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details.GetProfile(), render_frame_host());
  DCHECK(file_system_context.get());

  const storage::ExternalFileSystemBackend* external_backend =
      file_system_context->external_backend();
  DCHECK(external_backend);

  file_manager::util::FileDefinitionList file_definition_list;
  for (size_t i = 0; i < params->urls.size(); ++i) {
    const FileSystemURL file_system_url =
        file_system_context->CrackURL(GURL(params->urls[i]));
    DCHECK(external_backend->CanHandleType(file_system_url.type()))
        << "GURL: " << file_system_url.ToGURL()
        << "type: " << file_system_url.type();
    FileDefinition file_definition;
    const bool result =
        file_manager::util::ConvertAbsoluteFilePathToRelativeFileSystemPath(
            chrome_details.GetProfile(), extension_->id(),
            file_system_url.path(), &file_definition.virtual_path);
    if (!result)
      continue;
    // The API only supports isolated files. It still works for directories,
    // as the value is ignored for existing entries.
    file_definition.is_directory = false;
    file_definition_list.push_back(file_definition);
  }

  file_manager::util::ConvertFileDefinitionListToEntryDefinitionList(
      chrome_details.GetProfile(), extension_->id(),
      file_definition_list,  // Safe, since copied internally.
      base::BindOnce(
          &FileManagerPrivateInternalResolveIsolatedEntriesFunction::
              RunAsyncAfterConvertFileDefinitionListToEntryDefinitionList,
          this));
  return RespondLater();
}

void FileManagerPrivateInternalResolveIsolatedEntriesFunction::
    RunAsyncAfterConvertFileDefinitionListToEntryDefinitionList(
        std::unique_ptr<file_manager::util::EntryDefinitionList>
            entry_definition_list) {
  using extensions::api::file_manager_private_internal::EntryDescription;
  std::vector<EntryDescription> entries;

  for (const auto& definition : *entry_definition_list) {
    if (definition.error != base::File::FILE_OK)
      continue;
    EntryDescription entry;
    entry.file_system_name = definition.file_system_name;
    entry.file_system_root = definition.file_system_root_url;
    entry.file_full_path = "/" + definition.full_path.AsUTF8Unsafe();
    entry.file_is_directory = definition.is_directory;
    entries.push_back(std::move(entry));
  }

  Respond(ArgumentList(extensions::api::file_manager_private_internal::
                           ResolveIsolatedEntries::Results::Create(entries)));
}

FileManagerPrivateInternalComputeChecksumFunction::
    FileManagerPrivateInternalComputeChecksumFunction()
    : digester_(new drive::util::FileStreamMd5Digester()) {
}

FileManagerPrivateInternalComputeChecksumFunction::
    ~FileManagerPrivateInternalComputeChecksumFunction() = default;

ExtensionFunction::ResponseAction
FileManagerPrivateInternalComputeChecksumFunction::Run() {
  using extensions::api::file_manager_private_internal::ComputeChecksum::Params;
  using drive::util::FileStreamMd5Digester;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  if (params->url.empty()) {
    return RespondNow(Error("File URL must be provided."));
  }

  const ChromeExtensionFunctionDetails chrome_details(this);
  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details.GetProfile(), render_frame_host());

  FileSystemURL file_system_url(
      file_system_context->CrackURL(GURL(params->url)));
  if (!file_system_url.is_valid()) {
    return RespondNow(Error("File URL was invalid"));
  }

  std::unique_ptr<storage::FileStreamReader> reader =
      file_system_context->CreateFileStreamReader(
          file_system_url, 0, storage::kMaximumLength, base::Time());

  FileStreamMd5Digester::ResultCallback result_callback = base::BindOnce(
      &ComputeChecksumRespondOnUIThread,
      base::BindOnce(
          &FileManagerPrivateInternalComputeChecksumFunction::RespondWith,
          this));
  base::PostTaskWithTraits(
      FROM_HERE, {BrowserThread::IO},
      base::BindOnce(&FileStreamMd5Digester::GetMd5Digest,
                     base::Unretained(digester_.get()), std::move(reader),
                     std::move(result_callback)));

  return RespondLater();
}

void FileManagerPrivateInternalComputeChecksumFunction::RespondWith(
    const std::string& hash) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  Respond(OneArgument(std::make_unique<base::Value>(hash)));
}

FileManagerPrivateSearchFilesByHashesFunction::
    FileManagerPrivateSearchFilesByHashesFunction()
    : chrome_details_(this) {}

ExtensionFunction::ResponseAction
FileManagerPrivateSearchFilesByHashesFunction::Run() {
  using api::file_manager_private::SearchFilesByHashes::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  // TODO(hirono): Check the volume ID and fail the function for volumes other
  // than Drive.

  drive::EventLogger* const logger =
      file_manager::util::GetLogger(chrome_details_.GetProfile());
  if (logger) {
    logger->Log(logging::LOG_INFO,
                "%s[%d] called. (volume id: %s, number of hashes: %zd)", name(),
                request_id(), params->volume_id.c_str(),
                params->hash_list.size());
  }
  set_log_on_completion(true);

  drive::DriveIntegrationService* integration_service =
      drive::util::GetIntegrationServiceByProfile(chrome_details_.GetProfile());
  if (!integration_service) {
    // |integration_service| is NULL if Drive is disabled or not mounted.
    return RespondNow(Error("Drive not available"));
  }

  std::set<std::string> hashes(params->hash_list.begin(),
                               params->hash_list.end());

  drive::FileSystemInterface* const file_system =
      drive::util::GetFileSystemByProfile(chrome_details_.GetProfile());
  if (file_system) {
    file_system->SearchByHashes(
        hashes,
        base::BindOnce(
            &FileManagerPrivateSearchFilesByHashesFunction::OnSearchByHashes,
            this, hashes));
  } else {
    // |file_system| is NULL if the backend is DriveFs. It doesn't provide
    // dedicated backup solution yet, so for now just walk the files and check
    // MD5 extended attribute.
    base::PostTaskWithTraitsAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
        base::BindOnce(
            &FileManagerPrivateSearchFilesByHashesFunction::SearchByAttribute,
            this, hashes,
            integration_service->GetMountPointPath().Append(
                drive::util::kDriveMyDriveRootDirName),
            drive::util::GetDriveMountPointPath(chrome_details_.GetProfile())),
        base::BindOnce(
            &FileManagerPrivateSearchFilesByHashesFunction::OnSearchByAttribute,
            this, hashes));
  }

  return RespondLater();
}

std::vector<drive::HashAndFilePath>
FileManagerPrivateSearchFilesByHashesFunction::SearchByAttribute(
    const std::set<std::string>& hashes,
    const base::FilePath& dir,
    const base::FilePath& prefix) {
  std::vector<drive::HashAndFilePath> results;

  if (hashes.empty())
    return results;

  std::set<std::string> remaining = hashes;
  std::vector<char> attribute;
  base::FileEnumerator enumerator(dir, true, base::FileEnumerator::FILES);
  for (base::FilePath path = enumerator.Next(); !path.empty();
       path = enumerator.Next()) {
    if (GetFileExtendedAttribute(path, "user.drive.md5", &attribute)) {
      std::string md5(attribute.begin(), attribute.end());

      if (remaining.erase(md5)) {
        base::FilePath drive_path = prefix;
        bool success = dir.AppendRelativePath(path, &drive_path);
        DCHECK(success);
        results.push_back({md5, drive_path});
        if (remaining.empty())
          break;
      }
    }
  }

  return results;
}

void FileManagerPrivateSearchFilesByHashesFunction::OnSearchByAttribute(
    const std::set<std::string>& hashes,
    const std::vector<drive::HashAndFilePath>& results) {
  OnSearchByHashes(hashes, drive::FileError::FILE_ERROR_OK, results);
}

void FileManagerPrivateSearchFilesByHashesFunction::OnSearchByHashes(
    const std::set<std::string>& hashes,
    drive::FileError error,
    const std::vector<drive::HashAndFilePath>& search_results) {
  if (error != drive::FileError::FILE_ERROR_OK) {
    Respond(Error(drive::FileErrorToString(error)));
    return;
  }

  std::unique_ptr<base::DictionaryValue> result(new base::DictionaryValue());
  for (const auto& hash : hashes) {
    result->SetWithoutPathExpansion(hash, std::make_unique<base::ListValue>());
  }

  for (const auto& hashAndPath : search_results) {
    DCHECK(result->HasKey(hashAndPath.hash));
    base::ListValue* list;
    result->GetListWithoutPathExpansion(hashAndPath.hash, &list);
    list->AppendString(
        file_manager::util::ConvertDrivePathToFileSystemUrl(
            chrome_details_.GetProfile(), hashAndPath.path, extension_id())
            .spec());
  }
  Respond(OneArgument(std::move(result)));
}

FileManagerPrivateInternalSetEntryTagFunction::
    FileManagerPrivateInternalSetEntryTagFunction()
    : chrome_details_(this) {}

ExtensionFunction::ResponseAction
FileManagerPrivateInternalSetEntryTagFunction::Run() {
  using extensions::api::file_manager_private_internal::SetEntryTag::Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          Profile::FromBrowserContext(browser_context()), render_frame_host());
  const storage::FileSystemURL file_system_url(
      file_system_context->CrackURL(GURL(params->url)));
  if (file_system_url.type() == storage::kFileSystemTypeDriveFs) {
    return RespondNow(NoArguments());
  }

  const base::FilePath drive_path =
      drive::util::ExtractDrivePath(file_system_url.path());
  if (drive_path.empty())
    return RespondNow(Error("Only Drive files and directories are supported."));

  drive::FileSystemInterface* const file_system =
      drive::util::GetFileSystemByProfile(chrome_details_.GetProfile());
  // |file_system| is NULL if Drive is disabled.
  if (!file_system)
    return RespondNow(Error("Drive is disabled."));

  google_apis::drive::Property::Visibility visibility;
  switch (params->visibility) {
    case extensions::api::file_manager_private::ENTRY_TAG_VISIBILITY_PRIVATE:
      visibility = google_apis::drive::Property::VISIBILITY_PRIVATE;
      break;
    case extensions::api::file_manager_private::ENTRY_TAG_VISIBILITY_PUBLIC:
      visibility = google_apis::drive::Property::VISIBILITY_PUBLIC;
      break;
    default:
      NOTREACHED();
      return RespondNow(Error("Invalid visibility."));
      break;
  }

  file_system->SetProperty(
      drive_path, visibility, params->key, params->value,
      base::Bind(&FileManagerPrivateInternalSetEntryTagFunction::
                     OnSetEntryPropertyCompleted,
                 this));
  return RespondLater();
}

void FileManagerPrivateInternalSetEntryTagFunction::OnSetEntryPropertyCompleted(
    drive::FileError result) {
  Respond(result == drive::FILE_ERROR_OK ? NoArguments()
                                         : Error("Failed to set a tag."));
}

ExtensionFunction::ResponseAction
FileManagerPrivateInternalGetDirectorySizeFunction::Run() {
  using extensions::api::file_manager_private_internal::GetDirectorySize::
      Params;
  const std::unique_ptr<Params> params(Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);

  if (params->url.empty()) {
    return RespondNow(Error("File URL must be provided."));
  }

  const ChromeExtensionFunctionDetails chrome_details(this);
  scoped_refptr<storage::FileSystemContext> file_system_context =
      file_manager::util::GetFileSystemContextForRenderFrameHost(
          chrome_details.GetProfile(), render_frame_host());
  const storage::FileSystemURL file_system_url(
      file_system_context->CrackURL(GURL(params->url)));
  if (!chromeos::FileSystemBackend::CanHandleURL(file_system_url)) {
    return RespondNow(
        Error("FileSystemBackend failed to handle the entry's url."));
  }
  if (file_system_url.type() != storage::kFileSystemTypeNativeLocal &&
      file_system_url.type() != storage::kFileSystemTypeDriveFs) {
    return RespondNow(Error("Only local directories are supported."));
  }

  const base::FilePath root_path = file_manager::util::GetLocalPathFromURL(
      render_frame_host(), chrome_details.GetProfile(), GURL(params->url));
  if (root_path.empty()) {
    return RespondNow(
        Error("Failed to get a local path from the entry's url."));
  }

  base::PostTaskWithTraitsAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&base::ComputeDirectorySize, root_path),
      base::BindOnce(&FileManagerPrivateInternalGetDirectorySizeFunction::
                         OnDirectorySizeRetrieved,
                     this));
  return RespondLater();
}

void FileManagerPrivateInternalGetDirectorySizeFunction::
    OnDirectorySizeRetrieved(int64_t size) {
  Respond(
      OneArgument(std::make_unique<base::Value>(static_cast<double>(size))));
}

}  // namespace extensions
