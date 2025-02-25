// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/metrics/perf/perf_events_collector.h"

#include "base/bind.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "chrome/browser/metrics/perf/cpu_identity.h"
#include "chrome/browser/metrics/perf/perf_output.h"
#include "chrome/browser/metrics/perf/process_type_collector.h"
#include "chrome/browser/metrics/perf/windowed_incognito_observer.h"
#include "chrome/browser/ui/browser_list.h"
#include "components/variations/variations_associated_data.h"
#include "third_party/metrics_proto/sampled_profile.pb.h"

namespace metrics {

namespace {

const char kCWPFieldTrialName[] = "ChromeOSWideProfilingCollection";

// Limit the total size of protobufs that can be cached, so they don't take up
// too much memory. If the size of cached protobufs exceeds this value, stop
// collecting further perf data. The current value is 4 MB.
const size_t kCachedPerfDataProtobufSizeThreshold = 4 * 1024 * 1024;

// Name of the perf events collector. It is appended to the UMA metric names
// for reporting collection and upload status.
const char kPerfCollectorName[] = "Perf";

// Gets parameter named by |key| from the map. If it is present and is an
// integer, stores the result in |out| and return true. Otherwise return false.
bool GetInt64Param(const std::map<std::string, std::string>& params,
                   const std::string& key,
                   int64_t* out) {
  auto it = params.find(key);
  if (it == params.end())
    return false;
  int64_t value;
  // NB: StringToInt64 will set value even if the conversion fails.
  if (!base::StringToInt64(it->second, &value))
    return false;
  *out = value;
  return true;
}

// Parses the key. e.g.: "PerfCommand::arm::0" returns "arm"
bool ExtractPerfCommandCpuSpecifier(const std::string& key,
                                    std::string* cpu_specifier) {
  std::vector<std::string> tokens = base::SplitStringUsingSubstr(
      key, "::", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
  if (tokens.size() != 3)
    return false;
  if (tokens[0] != "PerfCommand")
    return false;
  *cpu_specifier = tokens[1];
  // tokens[2] is just a unique string (usually an index).
  return true;
}

// Parses the components of a version string, e.g. major.minor.bugfix
void ExtractVersionNumbers(const std::string& version,
                           int32_t* major_version,
                           int32_t* minor_version,
                           int32_t* bugfix_version) {
  *major_version = *minor_version = *bugfix_version = 0;
  // Parse out the version numbers from the string.
  sscanf(version.c_str(), "%d.%d.%d", major_version, minor_version,
         bugfix_version);
}

// Returns if a micro-architecture supports LBR callgraph profiling.
bool MicroarchitectureHasLBRCallgraph(const std::string& uarch) {
  return uarch == "Haswell" || uarch == "Broadwell" || uarch == "Skylake" ||
         uarch == "Kabylake";
}

// Returns if a kernel release supports LBR callgraph profiling.
bool KernelReleaseHasLBRCallgraph(const std::string& release) {
  int32_t major, minor, bugfix;
  ExtractVersionNumbers(release, &major, &minor, &bugfix);
  return major > 4 || (major == 4 && minor >= 4) || (major == 3 && minor == 18);
}

// Hopefully we never need a space in a command argument.
const char kPerfCommandDelimiter[] = " ";

const char kPerfRecordCyclesCmd[] = "perf record -a -e cycles -c 1000003";

const char kPerfRecordFPCallgraphCmd[] =
    "perf record -a -e cycles -g -c 4000037";

const char kPerfRecordLBRCallgraphCmd[] =
    "perf record -a -e cycles -c 4000037 --call-graph lbr";

const char kPerfRecordLBRCmd[] = "perf record -a -e r20c4 -b -c 200011";

// Silvermont, Airmont, Goldmont don't have a branches taken event. Therefore,
// we sample on the branches retired event.
const char kPerfRecordLBRCmdAtom[] = "perf record -a -e rc4 -b -c 300001";

const char kPerfRecordInstructionTLBMissesCmd[] =
    "perf record -a -e iTLB-misses -c 2003";

const char kPerfRecordDataTLBMissesCmd[] =
    "perf record -a -e dTLB-misses -c 2003";

const char kPerfRecordCacheMissesCmd[] =
    "perf record -a -e cache-misses -c 10007";

const char kPerfStatMemoryBandwidthCmd[] =
    "perf stat -a -e cycles -e instructions "
    "-e uncore_imc/data_reads/ -e uncore_imc/data_writes/ "
    "-e cpu/event=0xD0,umask=0x11,name=MEM_UOPS_RETIRED-STLB_MISS_LOADS/ "
    "-e cpu/event=0xD0,umask=0x12,name=MEM_UOPS_RETIRED-STLB_MISS_STORES/";

const std::vector<RandomSelector::WeightAndValue> GetDefaultCommands_x86_64(
    const CPUIdentity& cpuid) {
  using WeightAndValue = RandomSelector::WeightAndValue;
  std::vector<WeightAndValue> cmds;
  DCHECK_EQ(cpuid.arch, "x86_64");
  const std::string cpu_uarch = GetCpuUarch(cpuid);
  // Haswell and newer big Intel cores support LBR callstack profiling. This
  // requires kernel support, which was added in kernel 4.4, and it was
  // backported to kernel 3.18. Prefer LBR callstack profiling where supported
  // instead of FP callchains, because the former works with binaries compiled
  // with frame pointers disabled, such as the ARC runtime.
  const char* callgraph_cmd = kPerfRecordFPCallgraphCmd;
  if (MicroarchitectureHasLBRCallgraph(cpu_uarch) &&
      KernelReleaseHasLBRCallgraph(cpuid.release)) {
    callgraph_cmd = kPerfRecordLBRCallgraphCmd;
  }

  if (cpu_uarch == "IvyBridge" || cpu_uarch == "Haswell" ||
      cpu_uarch == "Broadwell") {
    cmds.push_back(WeightAndValue(45.0, kPerfRecordCyclesCmd));
    cmds.push_back(WeightAndValue(20.0, callgraph_cmd));
    cmds.push_back(WeightAndValue(15.0, kPerfRecordLBRCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordInstructionTLBMissesCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordDataTLBMissesCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfStatMemoryBandwidthCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordCacheMissesCmd));
    return cmds;
  }
  if (cpu_uarch == "SandyBridge" || cpu_uarch == "Skylake" ||
      cpu_uarch == "Kabylake") {
    cmds.push_back(WeightAndValue(50.0, kPerfRecordCyclesCmd));
    cmds.push_back(WeightAndValue(20.0, callgraph_cmd));
    cmds.push_back(WeightAndValue(15.0, kPerfRecordLBRCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordInstructionTLBMissesCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordDataTLBMissesCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordCacheMissesCmd));
    return cmds;
  }
  if (cpu_uarch == "Silvermont" || cpu_uarch == "Airmont" ||
      cpu_uarch == "Goldmont") {
    cmds.push_back(WeightAndValue(50.0, kPerfRecordCyclesCmd));
    cmds.push_back(WeightAndValue(20.0, callgraph_cmd));
    cmds.push_back(WeightAndValue(15.0, kPerfRecordLBRCmdAtom));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordInstructionTLBMissesCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordDataTLBMissesCmd));
    cmds.push_back(WeightAndValue(5.0, kPerfRecordCacheMissesCmd));
    return cmds;
  }
  // Other 64-bit x86
  cmds.push_back(WeightAndValue(65.0, kPerfRecordCyclesCmd));
  cmds.push_back(WeightAndValue(20.0, callgraph_cmd));
  cmds.push_back(WeightAndValue(5.0, kPerfRecordInstructionTLBMissesCmd));
  cmds.push_back(WeightAndValue(5.0, kPerfRecordDataTLBMissesCmd));
  cmds.push_back(WeightAndValue(5.0, kPerfRecordCacheMissesCmd));
  return cmds;
}

}  // namespace

namespace internal {

std::vector<RandomSelector::WeightAndValue> GetDefaultCommandsForCpu(
    const CPUIdentity& cpuid) {
  using WeightAndValue = RandomSelector::WeightAndValue;

  if (cpuid.arch == "x86_64")  // 64-bit x86
    return GetDefaultCommands_x86_64(cpuid);

  std::vector<WeightAndValue> cmds;
  if (cpuid.arch == "x86" ||     // 32-bit x86, or...
      cpuid.arch == "armv7l") {  // ARM
    cmds.push_back(WeightAndValue(80.0, kPerfRecordCyclesCmd));
    cmds.push_back(WeightAndValue(20.0, kPerfRecordFPCallgraphCmd));
    return cmds;
  }

  // Unknown CPUs
  cmds.push_back(WeightAndValue(1.0, kPerfRecordCyclesCmd));
  return cmds;
}

}  // namespace internal

PerfCollector::PerfCollector() : MetricCollector(kPerfCollectorName) {}

PerfCollector::~PerfCollector() {}

void PerfCollector::Init() {
  CHECK(command_selector_.SetOdds(
      internal::GetDefaultCommandsForCpu(GetCPUIdentity())));
  std::map<std::string, std::string> params;
  if (variations::GetVariationParams(kCWPFieldTrialName, &params))
    SetCollectionParamsFromVariationParams(params);

  MetricCollector::Init();
}

namespace internal {

std::string FindBestCpuSpecifierFromParams(
    const std::map<std::string, std::string>& params,
    const CPUIdentity& cpuid) {
  std::string ret;
  // The CPU specified in the variation params could be "default", a system
  // architecture, a CPU microarchitecture, or a CPU model substring. We should
  // prefer to match the most specific.
  enum MatchSpecificity {
    NO_MATCH,
    DEFAULT,
    SYSTEM_ARCH,
    CPU_UARCH,
    CPU_MODEL,
  };
  MatchSpecificity match_level = NO_MATCH;

  const std::string cpu_uarch = GetCpuUarch(cpuid);
  const std::string simplified_cpu_model =
      SimplifyCPUModelName(cpuid.model_name);

  for (const auto& key_val : params) {
    const std::string& key = key_val.first;

    std::string cpu_specifier;
    if (!ExtractPerfCommandCpuSpecifier(key, &cpu_specifier))
      continue;

    if (match_level < DEFAULT && cpu_specifier == "default") {
      match_level = DEFAULT;
      ret = cpu_specifier;
    }
    if (match_level < SYSTEM_ARCH && cpu_specifier == cpuid.arch) {
      match_level = SYSTEM_ARCH;
      ret = cpu_specifier;
    }
    if (match_level < CPU_UARCH && !cpu_uarch.empty() &&
        cpu_specifier == cpu_uarch) {
      match_level = CPU_UARCH;
      ret = cpu_specifier;
    }
    if (match_level < CPU_MODEL &&
        simplified_cpu_model.find(cpu_specifier) != std::string::npos) {
      match_level = CPU_MODEL;
      ret = cpu_specifier;
    }
  }
  return ret;
}

}  // namespace internal

void PerfCollector::SetCollectionParamsFromVariationParams(
    const std::map<std::string, std::string>& params) {
  int64_t value;
  if (GetInt64Param(params, "ProfileCollectionDurationSec", &value)) {
    collection_params_.collection_duration =
        base::TimeDelta::FromSeconds(value);
  }
  if (GetInt64Param(params, "PeriodicProfilingIntervalMs", &value)) {
    collection_params_.periodic_interval =
        base::TimeDelta::FromMilliseconds(value);
  }
  if (GetInt64Param(params, "ResumeFromSuspend::SamplingFactor", &value)) {
    collection_params_.resume_from_suspend.sampling_factor = value;
  }
  if (GetInt64Param(params, "ResumeFromSuspend::MaxDelaySec", &value)) {
    collection_params_.resume_from_suspend.max_collection_delay =
        base::TimeDelta::FromSeconds(value);
  }
  if (GetInt64Param(params, "RestoreSession::SamplingFactor", &value)) {
    collection_params_.restore_session.sampling_factor = value;
  }
  if (GetInt64Param(params, "RestoreSession::MaxDelaySec", &value)) {
    collection_params_.restore_session.max_collection_delay =
        base::TimeDelta::FromSeconds(value);
  }

  const std::string best_cpu_specifier =
      internal::FindBestCpuSpecifierFromParams(params, GetCPUIdentity());

  if (best_cpu_specifier.empty())  // No matching cpu specifier. Keep defaults.
    return;

  std::vector<RandomSelector::WeightAndValue> commands;
  for (const auto& key_val : params) {
    const std::string& key = key_val.first;
    const std::string& val = key_val.second;

    std::string cpu_specifier;
    if (!ExtractPerfCommandCpuSpecifier(key, &cpu_specifier))
      continue;
    if (cpu_specifier != best_cpu_specifier)
      continue;

    auto split = val.find(" ");
    if (split == std::string::npos)
      continue;  // Just drop invalid commands.
    std::string weight_str = std::string(val.begin(), val.begin() + split);

    double weight;
    if (!(base::StringToDouble(weight_str, &weight) && weight > 0.0))
      continue;  // Just drop invalid commands.
    std::string command(val.begin() + split + 1, val.end());
    commands.push_back(RandomSelector::WeightAndValue(weight, command));
  }
  command_selector_.SetOdds(commands);
}

MetricCollector::PerfProtoType PerfCollector::GetPerfProtoType(
    const std::vector<std::string>& args) {
  if (args.size() > 1 && args[0] == "perf") {
    if (args[1] == "record" || args[1] == "mem")
      return PerfProtoType::PERF_TYPE_DATA;
    if (args[1] == "stat")
      return PerfProtoType::PERF_TYPE_STAT;
  }

  return PerfProtoType::PERF_TYPE_UNSUPPORTED;
}

void PerfCollector::ParseOutputProtoIfValid(
    std::unique_ptr<WindowedIncognitoObserver> incognito_observer,
    std::unique_ptr<SampledProfile> sampled_profile,
    PerfProtoType type,
    const std::string& perf_stdout) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // |perf_output_call_| called us, and owns |perf_stdout|. We must delete it,
  // but not before parsing |perf_stdout|, and we may return early.
  std::unique_ptr<PerfOutputCall> call_deleter(std::move(perf_output_call_));

  if (incognito_observer->incognito_launched()) {
    AddToUmaHistogram(CollectionAttemptStatus::INCOGNITO_LAUNCHED);
    return;
  }

  std::map<uint32_t, Process> process_types =
      ProcessTypeCollector::ChromeProcessTypes();
  std::map<uint32_t, Thread> thread_types =
      ProcessTypeCollector::ChromeThreadTypes();
  if (!process_types.empty() && !thread_types.empty()) {
    sampled_profile->mutable_process_types()->insert(process_types.begin(),
                                                     process_types.end());
    sampled_profile->mutable_thread_types()->insert(thread_types.begin(),
                                                    thread_types.end());
  }

  SaveSerializedPerfProto(std::move(sampled_profile), type, perf_stdout);
}

bool PerfCollector::ShouldCollect() const {
  // Only allow one active collection.
  if (perf_output_call_) {
    AddToUmaHistogram(CollectionAttemptStatus::ALREADY_COLLECTING);
    return false;
  }

  // Do not collect further data if we've already collected a substantial amount
  // of data, as indicated by |kCachedPerfDataProtobufSizeThreshold|.
  if (cached_profile_data_size() >= kCachedPerfDataProtobufSizeThreshold) {
    AddToUmaHistogram(CollectionAttemptStatus::NOT_READY_TO_COLLECT);
    return false;
  }

  // For privacy reasons, Chrome should only collect perf data if there is no
  // incognito session active (or gets spawned during the collection).
  if (BrowserList::IsIncognitoSessionActive()) {
    AddToUmaHistogram(CollectionAttemptStatus::INCOGNITO_ACTIVE);
    return false;
  }

  return true;
}

void PerfCollector::CollectProfile(
    std::unique_ptr<SampledProfile> sampled_profile) {
  std::unique_ptr<WindowedIncognitoObserver> incognito_observer =
      std::make_unique<WindowedIncognitoObserver>();

  std::vector<std::string> command =
      base::SplitString(command_selector_.Select(), kPerfCommandDelimiter,
                        base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  PerfProtoType type = GetPerfProtoType(command);

  perf_output_call_ = std::make_unique<PerfOutputCall>(
      collection_params_.collection_duration, command,
      base::BindOnce(&PerfCollector::ParseOutputProtoIfValid,
                     base::AsWeakPtr<PerfCollector>(this),
                     base::Passed(&incognito_observer),
                     base::Passed(&sampled_profile), type));
}

}  // namespace metrics
