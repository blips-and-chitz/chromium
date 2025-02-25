// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/script/modulator_impl_base.h"

#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/renderer/bindings/core/v8/script_promise_resolver.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/use_counter.h"
#include "third_party/blink/renderer/core/loader/modulescript/module_script_fetch_request.h"
#include "third_party/blink/renderer/core/loader/modulescript/module_tree_linker.h"
#include "third_party/blink/renderer/core/loader/modulescript/module_tree_linker_registry.h"
#include "third_party/blink/renderer/core/origin_trials/origin_trials.h"
#include "third_party/blink/renderer/core/script/dynamic_module_resolver.h"
#include "third_party/blink/renderer/core/script/import_map.h"
#include "third_party/blink/renderer/core/script/module_map.h"
#include "third_party/blink/renderer/core/script/module_record_resolver_impl.h"
#include "third_party/blink/renderer/core/script/module_script.h"
#include "third_party/blink/renderer/core/script/parsed_specifier.h"
#include "third_party/blink/renderer/platform/bindings/v8_throw_exception.h"

namespace blink {

ExecutionContext* ModulatorImplBase::GetExecutionContext() const {
  return ExecutionContext::From(script_state_);
}

ModulatorImplBase::ModulatorImplBase(ScriptState* script_state)
    : script_state_(script_state),
      task_runner_(ExecutionContext::From(script_state_)
                       ->GetTaskRunner(TaskType::kNetworking)),
      map_(MakeGarbageCollected<ModuleMap>(this)),
      tree_linker_registry_(MakeGarbageCollected<ModuleTreeLinkerRegistry>()),
      module_record_resolver_(MakeGarbageCollected<ModuleRecordResolverImpl>(
          this,
          ExecutionContext::From(script_state_))),
      dynamic_module_resolver_(
          MakeGarbageCollected<DynamicModuleResolver>(this)) {
  DCHECK(script_state_);
  DCHECK(task_runner_);
}

ModulatorImplBase::~ModulatorImplBase() {}

bool ModulatorImplBase::IsScriptingDisabled() const {
  return !GetExecutionContext()->CanExecuteScripts(kAboutToExecuteScript);
}

bool ModulatorImplBase::BuiltInModuleInfraEnabled() const {
  return origin_trials::BuiltInModuleInfraEnabled(GetExecutionContext());
}

bool ModulatorImplBase::BuiltInModuleEnabled(
    blink::layered_api::Module module) const {
  DCHECK(BuiltInModuleInfraEnabled());
  switch (module) {
    case blink::layered_api::Module::kBlank:
      return true;
    case blink::layered_api::Module::kVirtualScroller:
      return RuntimeEnabledFeatures::BuiltInModuleAllEnabled();
    case blink::layered_api::Module::kKvStorage:
      return RuntimeEnabledFeatures::BuiltInModuleAllEnabled() ||
             origin_trials::BuiltInModuleKvStorageEnabled(
                 GetExecutionContext());
  }
}

void ModulatorImplBase::BuiltInModuleUseCount(
    blink::layered_api::Module module) const {
  DCHECK(BuiltInModuleInfraEnabled());
  DCHECK(BuiltInModuleEnabled(module));
  switch (module) {
    case blink::layered_api::Module::kBlank:
      break;
    case blink::layered_api::Module::kVirtualScroller:
      UseCounter::Count(GetExecutionContext(),
                        WebFeature::kBuiltInModuleVirtualScroller);
      break;
    case blink::layered_api::Module::kKvStorage:
      UseCounter::Count(GetExecutionContext(),
                        WebFeature::kBuiltInModuleKvStorage);
      break;
  }
}

// <specdef label="fetch-a-module-script-tree"
// href="https://html.spec.whatwg.org/C/#fetch-a-module-script-tree">
// <specdef label="fetch-a-module-worker-script-tree"
// href="https://html.spec.whatwg.org/C/#fetch-a-module-worker-script-tree">
void ModulatorImplBase::FetchTree(
    const KURL& url,
    ResourceFetcher* fetch_client_settings_object_fetcher,
    mojom::RequestContextType destination,
    const ScriptFetchOptions& options,
    ModuleScriptCustomFetchType custom_fetch_type,
    ModuleTreeClient* client) {
  // <spec label="fetch-a-module-script-tree" step="2">Perform the internal
  // module script graph fetching procedure given url, settings object,
  // destination, options, settings object, visited set, "client", and with the
  // top-level module fetch flag set. If the caller of this algorithm specified
  // custom perform the fetch steps, pass those along as well.</spec>

  // <spec label="fetch-a-module-worker-script-tree" step="3">Perform the
  // internal module script graph fetching procedure given url, fetch client
  // settings object, destination, options, module map settings object, visited
  // set, "client", and with the top-level module fetch flag set. If the caller
  // of this algorithm specified custom perform the fetch steps, pass those
  // along as well.</spec>

  ModuleTreeLinker::Fetch(url, fetch_client_settings_object_fetcher,
                          destination, options, this, custom_fetch_type,
                          tree_linker_registry_, client);

  // <spec label="fetch-a-module-script-tree" step="3">When the internal module
  // script graph fetching procedure asynchronously completes with result,
  // asynchronously complete this algorithm with result.</spec>

  // <spec label="fetch-a-module-worker-script-tree" step="4">When the internal
  // module script graph fetching procedure asynchronously completes with
  // result, asynchronously complete this algorithm with result.</spec>

  // Note: We delegate to ModuleTreeLinker to notify ModuleTreeClient.
}

void ModulatorImplBase::FetchDescendantsForInlineScript(
    ModuleScript* module_script,
    ResourceFetcher* fetch_client_settings_object_fetcher,
    mojom::RequestContextType destination,
    ModuleTreeClient* client) {
  ModuleTreeLinker::FetchDescendantsForInlineScript(
      module_script, fetch_client_settings_object_fetcher, destination, this,
      ModuleScriptCustomFetchType::kNone, tree_linker_registry_, client);
}

void ModulatorImplBase::FetchSingle(
    const ModuleScriptFetchRequest& request,
    ResourceFetcher* fetch_client_settings_object_fetcher,
    ModuleGraphLevel level,
    ModuleScriptCustomFetchType custom_fetch_type,
    SingleModuleClient* client) {
  map_->FetchSingleModuleScript(request, fetch_client_settings_object_fetcher,
                                level, custom_fetch_type, client);
}

ModuleScript* ModulatorImplBase::GetFetchedModuleScript(const KURL& url) {
  return map_->GetFetchedModuleScript(url);
}

// <specdef href="https://html.spec.whatwg.org/C/#resolve-a-module-specifier">
KURL ModulatorImplBase::ResolveModuleSpecifier(const String& specifier,
                                               const KURL& base_url,
                                               String* failure_reason) {
  ParsedSpecifier parsed_specifier =
      ParsedSpecifier::Create(specifier, base_url);

  if (!parsed_specifier.IsValid()) {
    if (failure_reason) {
      *failure_reason =
          "Invalid relative url or base scheme isn't hierarchical.";
    }
    return KURL();
  }

  // If |logger| is non-null, outputs detailed logs.
  // The detailed log should be useful for debugging particular import maps
  // errors, but should be supressed (i.e. |logger| should be null) in normal
  // cases.

  base::Optional<KURL> mapped_url;
  if (import_map_) {
    String import_map_debug_message;
    mapped_url =
        import_map_->Resolve(parsed_specifier, &import_map_debug_message);

    // Output the resolution log. This is too verbose to be always shown, but
    // will be helpful for Web developers (and also Chromium developers) for
    // debugging import maps.
    LOG(INFO) << import_map_debug_message;

    if (mapped_url) {
      KURL url = *mapped_url;
      if (!url.IsValid()) {
        if (failure_reason)
          *failure_reason = import_map_debug_message;
        return KURL();
      }
      return url;
    }
  }

  // The specifier is not mapped by import maps, either because
  // - There are no import maps, or
  // - The import map doesn't have an entry for |parsed_specifier|.

  switch (parsed_specifier.GetType()) {
    case ParsedSpecifier::Type::kInvalid:
      NOTREACHED();
      return KURL();

    case ParsedSpecifier::Type::kBare:
      // Allow |@std/x| specifiers if Layered API is enabled.
      if (BuiltInModuleInfraEnabled()) {
        if (parsed_specifier.GetImportMapKeyString().StartsWith("@std/")) {
          return KURL("import:" + parsed_specifier.GetImportMapKeyString());
        }
      }

      // Reject bare specifiers as specced by the pre-ImportMap spec.
      if (failure_reason) {
        *failure_reason =
            "Relative references must start with either \"/\", \"./\", or "
            "\"../\".";
      }
      return KURL();

    case ParsedSpecifier::Type::kURL:
      return parsed_specifier.GetUrl();
  }
}

void ModulatorImplBase::RegisterImportMap(const ImportMap* import_map) {
  if (import_map_) {
    // Only one import map is allowed.
    // TODO(crbug.com/927119): Implement merging.
    GetExecutionContext()->AddConsoleMessage(
        mojom::ConsoleMessageSource::kOther, mojom::ConsoleMessageLevel::kError,
        "Multiple import maps are not yet supported. https://crbug.com/927119");
    return;
  }

  if (!BuiltInModuleInfraEnabled()) {
    GetExecutionContext()->AddConsoleMessage(
        mojom::ConsoleMessageSource::kOther, mojom::ConsoleMessageLevel::kError,
        "Import maps are disabled when Layered API Infra is disabled.");
    return;
  }
  import_map_ = import_map;
}

bool ModulatorImplBase::HasValidContext() {
  return script_state_->ContextIsValid();
}

void ModulatorImplBase::ResolveDynamically(
    const String& specifier,
    const KURL& referrer_url,
    const ReferrerScriptInfo& referrer_info,
    ScriptPromiseResolver* resolver) {
  String reason;
  if (IsDynamicImportForbidden(&reason)) {
    resolver->Reject(V8ThrowException::CreateTypeError(
        GetScriptState()->GetIsolate(), reason));
    return;
  }
  UseCounter::Count(GetExecutionContext(),
                    WebFeature::kDynamicImportModuleScript);
  dynamic_module_resolver_->ResolveDynamically(specifier, referrer_url,
                                               referrer_info, resolver);
}

// <specdef href="https://html.spec.whatwg.org/C/#hostgetimportmetaproperties">
ModuleImportMeta ModulatorImplBase::HostGetImportMetaProperties(
    ModuleRecord record) const {
  // <spec step="1">Let module script be moduleRecord.[[HostDefined]].</spec>
  const ModuleScript* module_script =
      module_record_resolver_->GetHostDefined(record);
  DCHECK(module_script);

  // <spec step="2">Let urlString be module script's base URL,
  // serialized.</spec>
  String url_string = module_script->BaseURL().GetString();

  // <spec step="3">Return « Record { [[Key]]: "url", [[Value]]: urlString }
  // ».</spec>
  return ModuleImportMeta(url_string);
}

ScriptValue ModulatorImplBase::InstantiateModule(ModuleRecord module_record) {
  UseCounter::Count(GetExecutionContext(),
                    WebFeature::kInstantiateModuleScript);

  ScriptState::Scope scope(script_state_);
  return module_record.Instantiate(script_state_);
}

Vector<Modulator::ModuleRequest>
ModulatorImplBase::ModuleRequestsFromModuleRecord(ModuleRecord module_record) {
  ScriptState::Scope scope(script_state_);
  Vector<String> specifiers = module_record.ModuleRequests(script_state_);
  Vector<TextPosition> positions =
      module_record.ModuleRequestPositions(script_state_);
  DCHECK_EQ(specifiers.size(), positions.size());
  Vector<ModuleRequest> requests;
  requests.ReserveInitialCapacity(specifiers.size());
  for (wtf_size_t i = 0; i < specifiers.size(); ++i) {
    requests.emplace_back(specifiers[i], positions[i]);
  }
  return requests;
}

void ModulatorImplBase::ProduceCacheModuleTreeTopLevel(
    ModuleScript* module_script) {
  DCHECK(module_script);
  // Since we run this asynchronously, context might be gone already,
  // for example because the frame was detached.
  if (!script_state_->ContextIsValid())
    return;
  HeapHashSet<Member<const ModuleScript>> discovered_set;
  ProduceCacheModuleTree(module_script, &discovered_set);
}

void ModulatorImplBase::ProduceCacheModuleTree(
    ModuleScript* module_script,
    HeapHashSet<Member<const ModuleScript>>* discovered_set) {
  DCHECK(module_script);

  discovered_set->insert(module_script);

  ModuleRecord record = module_script->Record();
  DCHECK(!record.IsNull());

  module_script->ProduceCache();

  Vector<Modulator::ModuleRequest> child_specifiers =
      ModuleRequestsFromModuleRecord(record);

  for (const auto& module_request : child_specifiers) {
    KURL child_url =
        module_script->ResolveModuleSpecifier(module_request.specifier);

    CHECK(child_url.IsValid())
        << "ModuleScript::ResolveModuleSpecifier() impl must "
           "return a valid url.";

    ModuleScript* child_module = GetFetchedModuleScript(child_url);
    CHECK(child_module);

    if (discovered_set->Contains(child_module))
      continue;

    ProduceCacheModuleTree(child_module, discovered_set);
  }
}

// <specdef href="https://html.spec.whatwg.org/C/#run-a-module-script">
ScriptValue ModulatorImplBase::ExecuteModule(
    ModuleScript* module_script,
    CaptureEvalErrorFlag capture_error) {
  // <spec step="1">If rethrow errors is not given, let it be false.</spec>

  // <spec step="2">Let settings be the settings object of script.</spec>
  //
  // The settings object is |this|.

  // <spec step="3">Check if we can run script with settings. If this returns
  // "do not run" then return NormalCompletion(empty).</spec>
  if (IsScriptingDisabled())
    return ScriptValue();

  // <spec step="4">Prepare to run script given settings.</spec>
  //
  // This is placed here to also cover ModuleRecord::ReportException().
  ScriptState::Scope scope(script_state_);

  // <spec step="5">Let evaluationStatus be null.</spec>
  //
  // |error| corresponds to "evaluationStatus of [[Type]]: throw".
  ScriptValue error;

  // <spec step="6">If script's error to rethrow is not null, ...</spec>
  if (module_script->HasErrorToRethrow()) {
    // <spec step="6">... then set evaluationStatus to Completion { [[Type]]:
    // throw, [[Value]]: script's error to rethrow, [[Target]]: empty }.</spec>
    error = module_script->CreateErrorToRethrow();
  } else {
    // <spec step="7">Otherwise:</spec>

    // <spec step="7.1">Let record be script's record.</spec>
    const ModuleRecord& record = module_script->Record();
    CHECK(!record.IsNull());

    // <spec step="7.2">Set evaluationStatus to record.Evaluate(). ...</spec>
    error = record.Evaluate(script_state_);

    // <spec step="7.2">... If Evaluate fails to complete as a result of the
    // user agent aborting the running script, then set evaluationStatus to
    // Completion { [[Type]]: throw, [[Value]]: a new "QuotaExceededError"
    // DOMException, [[Target]]: empty }.</spec>

    // [not specced] Store V8 code cache on successful evaluation.
    if (error.IsEmpty()) {
      TaskRunner()->PostTask(
          FROM_HERE,
          WTF::Bind(&ModulatorImplBase::ProduceCacheModuleTreeTopLevel,
                    WrapWeakPersistent(this), WrapPersistent(module_script)));
    }
  }

  // <spec step="8">If evaluationStatus is an abrupt completion, then:</spec>
  if (!error.IsEmpty()) {
    // <spec step="8.1">If rethrow errors is true, rethrow the exception given
    // by evaluationStatus.[[Value]].</spec>
    if (capture_error == CaptureEvalErrorFlag::kCapture)
      return error;

    // <spec step="8.2">Otherwise, report the exception given by
    // evaluationStatus.[[Value]] for script.</spec>
    ModuleRecord::ReportException(script_state_, error.V8Value());
  }

  // <spec step="9">Clean up after running script with settings.</spec>
  //
  // Implemented as the ScriptState::Scope destructor.
  return ScriptValue();
}

void ModulatorImplBase::Trace(blink::Visitor* visitor) {
  visitor->Trace(script_state_);
  visitor->Trace(map_);
  visitor->Trace(tree_linker_registry_);
  visitor->Trace(module_record_resolver_);
  visitor->Trace(dynamic_module_resolver_);
  visitor->Trace(import_map_);

  Modulator::Trace(visitor);
}

}  // namespace blink
