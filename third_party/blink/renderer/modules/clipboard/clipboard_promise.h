// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_CLIPBOARD_CLIPBOARD_PROMISE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_CLIPBOARD_CLIPBOARD_PROMISE_H_

#include <utility>

#include "base/macros.h"
#include "base/sequence_checker.h"
#include "third_party/blink/public/mojom/permissions/permission.mojom-blink.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise.h"
#include "third_party/blink/renderer/core/execution_context/context_lifecycle_observer.h"
#include "third_party/blink/renderer/core/fileapi/blob.h"
#include "third_party/blink/renderer/modules/clipboard/clipboard_writer.h"

namespace blink {

class ScriptPromiseResolver;

class ClipboardPromise final
    : public GarbageCollectedFinalized<ClipboardPromise>,
      public ContextLifecycleObserver {
  USING_GARBAGE_COLLECTED_MIXIN(ClipboardPromise);

 public:
  ClipboardPromise(ScriptState*);
  virtual ~ClipboardPromise();

  // Creates promise to execute Clipboard API functions off the main thread.
  static ScriptPromise CreateForRead(ScriptState*);
  static ScriptPromise CreateForReadText(ScriptState*);
  static ScriptPromise CreateForWrite(
      ScriptState*,
      HeapVector<std::pair<String, Member<Blob>>>);
  static ScriptPromise CreateForWriteText(ScriptState*, const String&);

  // For rejections originating from ClipboardWriter.
  void RejectFromReadOrDecodeFailure();

  void WriteNextRepresentation();

  void Trace(blink::Visitor*) override;

 private:
  scoped_refptr<base::SingleThreadTaskRunner> GetTaskRunner();

  bool IsFocusedDocument(ExecutionContext*);

  // Checks for permissions (interacting with PermissionService).
  mojom::blink::PermissionService* GetPermissionService();
  void RequestReadPermission(
      mojom::blink::PermissionService::RequestPermissionCallback);
  void CheckWritePermission(
      mojom::blink::PermissionService::HasPermissionCallback);

  // Checks Read/Write permission (interacting with PermissionService).
  void HandleRead();
  void HandleReadText();
  void HandleWrite(HeapVector<std::pair<String, Member<Blob>>>*);
  void HandleWriteText(const String&);

  // Reads/Writes after permission check.
  void HandleReadWithPermission(mojom::blink::PermissionStatus);
  void HandleReadTextWithPermission(mojom::blink::PermissionStatus);
  void HandleWriteWithPermission(mojom::blink::PermissionStatus);
  void HandleWriteTextWithPermission(mojom::blink::PermissionStatus);

  // Detects whether an image or text is on the clipboard, and
  // returns all valid clipboard types on the clipboard.
  Vector<String> TypesToRead();

  Member<ScriptState> script_state_;
  Member<ScriptPromiseResolver> script_promise_resolver_;

  std::unique_ptr<ClipboardWriter> clipboard_writer_;
  // Checks for Read and Write permission.
  mojom::blink::PermissionServicePtr permission_service_;

  // Only for use in writeText().
  String plain_text_;
  HeapVector<std::pair<String, Member<Blob>>> clipboard_item_;
  // Index of clipboard representation currently being processed.
  wtf_size_t clipboard_representation_index_;

  // Because v8 is thread-hostile, ensures that all interactions with
  // ScriptState and ScriptPromiseResolver occur on the main thread.
  SEQUENCE_CHECKER(sequence_checker_);

  DISALLOW_COPY_AND_ASSIGN(ClipboardPromise);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_CLIPBOARD_CLIPBOARD_PROMISE_H_
