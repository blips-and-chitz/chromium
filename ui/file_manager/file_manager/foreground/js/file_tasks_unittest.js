// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Mock metrics
 * @type {!Object}
 */
window.metrics = {
  recordEnum: function() {},
  recordSmallCount: function() {},
};

/**
 * Mock chrome APIs.
 * @type {!Object}
 */
let mockChrome;

/**
 * Mock task history.
 * @type {!TaskHistory}
 */
const mockTaskHistory = /** @type {!TaskHistory} */ ({
  getLastExecutedTime: function(id) {
    return 0;
  },
  recordTaskExecuted: function(id) {},
});

// Set up test components.
function setUp() {
  // Mock LoadTimeData strings.
  window.loadTimeData.data = {
    DRIVE_FS_ENABLED: false,
  };
  window.loadTimeData.getString = id => id;

  const mockTask = /** @type {!chrome.fileManagerPrivate.FileTask} */ ({
    taskId: 'handler-extension-id|app|any',
    isDefault: false,
    isGenericFileHandler: true,
  });

  // Mock chome APIs.
  mockChrome = {
    commandLinePrivate: {
      hasSwitch: function(name, callback) {
        callback(false);
      },
    },
    fileManagerPrivate: {
      getFileTasks: function(entries, callback) {
        setTimeout(callback.bind(null, [mockTask]), 0);
      },
      executeTask: function(taskId, entries, onViewFiles) {
        onViewFiles('failed');
      },
      sharePathsWithCrostini: function(vmName, entries, persist, callback) {
        callback();
      },
    },
    runtime: {
      id: 'test-extension-id',
    },
  };

  installMockChrome(mockChrome);
}

/**
 * Fail with an error message.
 * @param {string} message The error message.
 * @param {string=} opt_details Optional details.
 */
function failWithMessage(message, opt_details) {
  if (opt_details) {
    message += ': '.concat(opt_details);
  }
  throw new Error(message);
}

/**
 * Returns mocked file manager components.
 * @return {!Object}
 */
function getMockFileManager() {
  const crostini = createCrostiniForTest();

  const fileManager = {
    volumeManager: /** @type {!VolumeManager} */ ({
      getLocationInfo: function(entry) {
        return {
          rootType: VolumeManagerCommon.RootType.DRIVE,
        };
      },
      getDriveConnectionState: function() {
        return VolumeManagerCommon.DriveConnectionType.ONLINE;
      },
      getVolumeInfo: function(entry) {
        return {
          volumeType: VolumeManagerCommon.VolumeType.DRIVE,
        };
      },
    }),
    ui: /** @type {!FileManagerUI} */ ({
      alertDialog: {
        showHtml: function(title, text, onOk, onCancel, onShow) {},
      },
      speakA11yMessage: (text) => {},
    }),
    metadataModel: /** @type {!MetadataModel} */ ({}),
    namingController: /** @type {!NamingController} */ ({}),
    directoryModel: /** @type {!DirectoryModel} */ ({
      getCurrentRootType: function() {
        return null;
      },
    }),
    crostini: crostini,
  };

  fileManager.crostini.init(fileManager.volumeManager);
  return fileManager;
}

/**
 * Returns a promise that resolves when the showHtml method of alert dialog is
 * called with the expected title and text.
 *
 * @param {!Array<!Entry>} entries Entries.
 * @param {string} expectedTitle The expected title.
 * @param {string} expectedText The expected text.
 * @return {!Promise}
 */
function showHtmlOfAlertDialogIsCalled(entries, expectedTitle, expectedText) {
  return new Promise((resolve, reject) => {
    const fileManager = getMockFileManager();
    fileManager.ui.alertDialog.showHtml =
        (title, text, onOk, onCancel, onShow) => {
          assertEquals(expectedTitle, title);
          assertEquals(expectedText, text);
          resolve();
        };

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, fileManager.ui, entries, [null],
            mockTaskHistory, fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.executeDefault();
        });
  });
}

/**
 * Returns a promise that resolves when openSuggestAppsDialog is called.
 *
 * @param {!Array<!Entry>} entries Entries.
 * @param {!Array<?string>} mimeTypes Mime types.
 * @return {!Promise}
 */
function openSuggestAppsDialogIsCalled(entries, mimeTypes) {
  return new Promise((resolve, reject) => {
    const fileManager = getMockFileManager();
    fileManager.ui.suggestAppsDialog = {
      showByExtensionAndMime: function(extension, mimeType, onDialogClosed) {
        resolve();
      },
    };

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, fileManager.ui, entries, mimeTypes,
            mockTaskHistory, fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.executeDefault();
        });
  });
}

/**
 * Returns a promise that resolves when the task picker is called.
 *
 * @param {!Array<!Entry>} entries Entries.
 * @param {!Array<?string>} mimeTypes Mime types.
 * @return {!Promise}
 */
function showDefaultTaskDialogCalled(entries, mimeTypes) {
  return new Promise((resolve, reject) => {
    const fileManager = getMockFileManager();
    fileManager.ui.defaultTaskPicker = {
      showDefaultTaskDialog: function(
          title, message, items, defaultIdx, onSuccess) {
        resolve();
      },
    };

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, fileManager.ui, entries, mimeTypes,
            mockTaskHistory, fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.executeDefault();
        });
  });
}

/**
 * Tests opening a .exe file.
 */
function testToOpenExeFile(callback) {
  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.exe');

  reportPromise(
      showHtmlOfAlertDialogIsCalled(
          [mockEntry], 'test.exe', 'NO_TASK_FOR_EXECUTABLE'),
      callback);
}

/**
 * Tests opening a .dmg file.
 */
function testToOpenDmgFile(callback) {
  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.dmg');

  reportPromise(
      showHtmlOfAlertDialogIsCalled([mockEntry], 'test.dmg', 'NO_TASK_FOR_DMG'),
      callback);
}

/**
 * Tests opening a .crx file.
 */
function testToOpenCrxFile(callback) {
  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.crx');

  reportPromise(
      showHtmlOfAlertDialogIsCalled(
          [mockEntry], 'NO_TASK_FOR_CRX_TITLE', 'NO_TASK_FOR_CRX'),
      callback);
}

/**
 * Tests opening a .rtf file.
 */
function testToOpenRtfFile(callback) {
  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.rtf');

  reportPromise(
      openSuggestAppsDialogIsCalled([mockEntry], ['application/rtf']),
      callback);
}

/**
 * Tests opening an entry that has external metadata type.
 */
function testOpenSuggestAppsDialogWithMetadata(callback) {
  const showByExtensionAndMimeIsCalled = new Promise((resolve, reject) => {
    const mockFileSystem = new MockFileSystem('volumeId');
    const mockEntry = new MockFileEntry(mockFileSystem, '/test.rtf');
    const fileManager = getMockFileManager();

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, /** @type {!FileManagerUI} */ ({
              taskMenuButton: document.createElement('button'),
              fileContextMenu: {
                defaultActionMenuItem: document.createElement('div'),
              },
              suggestAppsDialog: {
                showByExtensionAndMime: function(
                    extension, mimeType, onDialogClosed) {
                  assertEquals('.rtf', extension);
                  assertEquals('application/rtf', mimeType);
                  resolve();
                },
              },
            }),
            [mockEntry], ['application/rtf'], mockTaskHistory,
            fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.openSuggestAppsDialog(() => {}, () => {}, () => {});
        });
  });

  reportPromise(showByExtensionAndMimeIsCalled, callback);
}

/**
 * Tests opening an entry that has no extension. Since the entry extension and
 * entry MIME type are required, the onFalure method should be called.
 */
function testOpenSuggestAppsDialogFailure(callback) {
  const onFailureIsCalled = new Promise((resolve, reject) => {
    const mockFileSystem = new MockFileSystem('volumeId');
    const mockEntry = new MockFileEntry(mockFileSystem, '/test');
    const fileManager = getMockFileManager();

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, fileManager.ui, [mockEntry], [null],
            mockTaskHistory, fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.openSuggestAppsDialog(() => {}, () => {}, resolve);
        });
  });

  reportPromise(onFailureIsCalled, callback);
}

/**
 * Tests opening the task picker with an entry that does not have a default app
 * but there are multiple apps that could open it.
 */
function testOpenTaskPicker(callback) {
  window.chrome.fileManagerPrivate.getFileTasks = (entries, callback) => {
    setTimeout(
        callback.bind(
            null,
            [
              {
                taskId: 'handler-extension-id1|app|any',
                isDefault: false,
                isGenericFileHandler: false,
                title: 'app 1',
              },
              {
                taskId: 'handler-extension-id2|app|any',
                isDefault: false,
                isGenericFileHandler: false,
                title: 'app 2',
              },
            ]),
        0);
  };

  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.tiff');

  reportPromise(
      showDefaultTaskDialogCalled([mockEntry], ['image/tiff']), callback);
}

/**
 * Tests opening the task picker with an entry that does not have a default app
 * but there are multiple apps that could open it. The app with the most recent
 * task execution order should execute.
 */
function testOpenWithMostRecentlyExecuted(callback) {
  const latestTaskId = 'handler-extension-most-recently-executed|app|any';
  const oldTaskId = 'handler-extension-executed-before|app|any';

  window.chrome.fileManagerPrivate.getFileTasks = (entries, callback) => {
    setTimeout(
        callback.bind(
            null,
            // File tasks is sorted by last executed time, latest first.
            [
              {
                taskId: latestTaskId,
                isDefault: false,
                isGenericFileHandler: false,
                title: 'app 1',
              },
              {
                taskId: oldTaskId,
                isDefault: false,
                isGenericFileHandler: false,
                title: 'app 2',
              },
              {
                taskId: 'handler-extension-never-executed|app|any',
                isDefault: false,
                isGenericFileHandler: false,
                title: 'app 3',
              },
            ]),
        0);
  };

  const taskHistory = /** @type {!TaskHistory} */ ({
    getLastExecutedTime: function(id) {
      if (id == oldTaskId) {
        return 10000;
      }
      if (id == latestTaskId) {
        return 20000;
      }
      return 0;
    },
    recordTaskExecuted: function(id) {},
  });

  let executedTask = null;
  window.chrome.fileManagerPrivate.executeTask =
      (taskId, entries, onViewFiles) => {
        executedTask = taskId;
        onViewFiles('success');
      };

  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.tiff');

  const promise = new Promise((resolve, reject) => {
    const fileManager = getMockFileManager();
    fileManager.ui.defaultTaskPicker = {
      showDefaultTaskDialog: function(
          title, message, items, defaultIdx, onSuccess) {
        failWithMessage('should not show task picker');
      },
    };

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, fileManager.ui, [mockEntry], [null],
            taskHistory, fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.executeDefault();
          assertEquals(latestTaskId, executedTask);
          resolve();
        });
  });

  reportPromise(promise, callback);
}

/**
 * Tests opening a .zip file.
 */
function testOpenZipWithZipArchiver(callback) {
  const zipArchiverTaskId = 'dmboannefpncccogfdikhmhpmdnddgoe|app|open';

  chrome.commandLinePrivate.hasSwitch = (name, callback) => {
    if (name == 'enable-zip-archiver-unpacker') {
      // This flag used to exist and was used to switch between the "Zip
      // Unpacker" and "Zip Archiver" component extensions.
      failWithMessage('run zip archiver', 'zip archiver flags checked');
    }
    callback(false);
  };

  window.chrome.fileManagerPrivate.getFileTasks = (entries, callback) => {
    setTimeout(
        callback.bind(
            null,
            [
              {
                taskId: zipArchiverTaskId,
                isDefault: false,
                isGenericFileHandler: false,
                title: 'Zip Archiver',
              },
            ]),
        0);
  };

  // None of the tasks has ever been executed.
  const taskHistory = /** @type {!TaskHistory} */ ({
    getLastExecutedTime: function(id) {
      return 0;
    },
    recordTaskExecuted: function(id) {},
  });

  let executedTask = null;
  window.chrome.fileManagerPrivate.executeTask =
      (taskId, entries, onViewFiles) => {
        executedTask = taskId;
        onViewFiles('success');
      };

  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.zip');

  const promise = new Promise((resolve, reject) => {
    const fileManager = getMockFileManager();
    fileManager.ui.defaultTaskPicker = {
      showDefaultTaskDialog: function(
          title, message, items, defaultIdx, onSuccess) {
        failWithMessage('run zip archiver', 'default task picker was shown');
      },
    };

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, fileManager.ui, [mockEntry], [null],
            taskHistory, fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.executeDefault();
          assertEquals(zipArchiverTaskId, executedTask);
          resolve();
        });
  });

  reportPromise(promise, callback);
}

/**
 * Tests opening a .deb file. The crostini linux package install dialog should
 * be called.
 */
function testOpenInstallLinuxPackageDialog(callback) {
  window.chrome.fileManagerPrivate.getFileTasks = (entries, callback) => {
    setTimeout(
        callback.bind(
            null,
            [
              {
                taskId: 'test-extension-id|app|install-linux-package',
                isDefault: false,
                isGenericFileHandler: false,
                title: '__MSG_INSTALL_LINUX_PACKAGE__',
              },
            ]),
        0);
  };

  const mockFileSystem = new MockFileSystem('volumeId');
  const mockEntry = new MockFileEntry(mockFileSystem, '/test.deb');

  const promise = new Promise((resolve, reject) => {
    const fileManager = getMockFileManager();
    fileManager.ui.installLinuxPackageDialog = {
      showInstallLinuxPackageDialog: function(entry) {
        resolve();
      },
    };

    fileManager.volumeManager.getLocationInfo = entry => {
      return /** @type {!EntryLocation} */ ({
        rootType: VolumeManagerCommon.RootType.CROSTINI,
      });
    };

    FileTasks
        .create(
            fileManager.volumeManager, fileManager.metadataModel,
            fileManager.directoryModel, fileManager.ui, [mockEntry], [null],
            mockTaskHistory, fileManager.namingController, fileManager.crostini)
        .then(tasks => {
          tasks.executeDefault();
        });
  });

  reportPromise(promise, callback);
}

/**
 * Tests that opening files within the Downloads directory using a crostini app
 * displays the sharing permission dialog, which users can use to allow or deny
 * sharing the directory with crostini.
 */
function testMaybeShareCrostiniOrShowDialog() {
  const volumeManagerDownloads = /** @type {!VolumeManager} */ ({
    getLocationInfo: (entry) => {
      return /** @type {!EntryLocation} */ ({
        rootType: entry.filesystem.name,
      });
    },
  });

  const mockFsDownloads = new MockFileSystem('downloads');
  const sharedDir = new MockDirectoryEntry(mockFsDownloads, '/shared');
  const shared = new MockFileEntry(mockFsDownloads, '/shared/file');

  const crostini = createCrostiniForTest();
  crostini.init(volumeManagerDownloads);
  crostini.setEnabled(true);
  crostini.registerSharedPath('vm', sharedDir);

  const notShared1 = new MockFileEntry(mockFsDownloads, '/notShared/file1');
  const notShared2 = new MockFileEntry(mockFsDownloads, '/notShared/file2');
  const otherNotShared =
      new MockFileEntry(mockFsDownloads, '/otherNotShared/file');
  const mockFsUnsharable = new MockFileSystem('unsharable');
  const unsharable = new MockDirectoryEntry(mockFsUnsharable, '/unsharable');

  function expect(
      comment, entries, expectSuccess, expectedDialogTitle,
      expectedDialogMessage) {
    let showHtmlCalled = false;
    function showHtml(title, message) {
      showHtmlCalled = true;
      assertEquals(
          expectedDialogTitle, title,
          'crostini share dialog title: ' + comment);
      assertEquals(
          expectedDialogMessage, message,
          'crostini share dialog message: ' + comment);
    }

    const fakeFilesTask = {
      entries_: entries,
      crostini_: crostini,
      ui_: {
        alertDialog: {showHtml: showHtml},
        confirmDialog: {showHtml: showHtml},
      },
      volumeManager_: volumeManagerDownloads,
    };

    const crostiniTask = /** @type {!chrome.fileManagerPrivate.FileTask} */ ({
      taskId: '|crostini|',
    });

    let success = false;
    let proto = /** @type {!Object} */ (FileTasks.prototype);
    proto.maybeShareWithCrostiniOrShowDialog_.call(
        fakeFilesTask, crostiniTask, () => {
          success = true;
        });

    assertEquals(expectSuccess, success, 'success: ' + comment);
    assertEquals(expectSuccess, !showHtmlCalled, 'showHtml called:' + comment);
  }

  expect('No entries', [], true, '', '');

  crostini.setEnabled(false);
  expect(
      'Single entry, crostini-files not enabled', [notShared1], false,
      'UNABLE_TO_OPEN_CROSTINI_TITLE', 'UNABLE_TO_OPEN_CROSTINI');

  crostini.setEnabled(true);

  expect('Single entry, not shared', [notShared1], true, '', '');

  expect('Single entry, shared', [shared], true, '', '');

  expect(
      '2 entries, not shared, same dir', [notShared1, notShared2], true, '',
      '');
  // Non-persistent shares should not be registered.
  assertFalse(crostini.isPathShared('vm', notShared1));

  expect(
      '2 entries, not shared, different dir', [notShared1, otherNotShared],
      true, '', '');

  expect(
      '2 entries, 1 not shared, different dir, not shared first',
      [notShared1, shared], true, '', '');

  expect(
      '2 entries, 1 not shared, different dir, shared first',
      [shared, notShared1], true, '', '');

  expect(
      '3 entries, 2 not shared, different dir',
      [shared, notShared1, notShared2], true, '', '');

  expect(
      '2 entries, 1 not sharable', [notShared1, unsharable], false,
      'UNABLE_TO_OPEN_CROSTINI_TITLE', 'UNABLE_TO_OPEN_CROSTINI');
}

/**
 * Tests file tasks and crostini sharing.
 */
function testTaskRequiresCrostiniSharing() {
  /**
   * Returns a fileManagerPrivate.FileTask containing the given task |id|.
   * @param {string} id
   * @return {!chrome.fileManagerPrivate.FileTask}
   */
  const createTask = (id) => {
    return /** @type {!chrome.fileManagerPrivate.FileTask} */ ({
      taskId: id,
    });
  };

  const crostiniOpenWithTask = createTask('app|crostini|open-with');
  assertTrue(FileTasks.taskRequiresCrostiniSharing(crostiniOpenWithTask));

  const installLinuxPackageTask = createTask('appId|x|install-linux-package');
  assertTrue(FileTasks.taskRequiresCrostiniSharing(installLinuxPackageTask));

  const notRequiredOpenWithTask = createTask('appId|x|open-with');
  assertFalse(FileTasks.taskRequiresCrostiniSharing(notRequiredOpenWithTask));
}
