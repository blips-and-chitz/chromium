// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_CHROMEOS_POWER_AUTO_SCREEN_BRIGHTNESS_ADAPTER_H_
#define CHROME_BROWSER_CHROMEOS_POWER_AUTO_SCREEN_BRIGHTNESS_ADAPTER_H_

#include <string>

#include "base/macros.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/optional.h"
#include "base/scoped_observer.h"
#include "base/sequenced_task_runner.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/als_reader.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/als_samples.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/brightness_monitor.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/metrics_reporter.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/model_config.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/model_config_loader.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/modeller.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/monotone_cubic_spline.h"
#include "chrome/browser/chromeos/power/auto_screen_brightness/utils.h"
#include "chromeos/dbus/power/power_manager_client.h"

class Profile;

namespace chromeos {
namespace power {
namespace auto_screen_brightness {

// Adapter monitors changes in ambient light, selects an optimal screen
// brightness as predicted by the model and instructs powerd to change it.
class Adapter : public AlsReader::Observer,
                public BrightnessMonitor::Observer,
                public Modeller::Observer,
                public ModelConfigLoader::Observer,
                public PowerManagerClient::Observer {
 public:
  // Type of curve to use.
  // These values are persisted to logs. Entries should not be renumbered and
  // numeric values should never be reused.
  enum class ModelCurve {
    // Always use the global curve.
    kGlobal = 0,
    // Always use the personal curve, and make no brightness adjustment until a
    // personal curve is trained.
    kPersonal = 1,
    // Use the personal curve if available, else use the global curve.
    kLatest = 2,
    kMaxValue = kLatest
  };

  // How user manual brightness change will affect Adapter.
  // These values are persisted to logs. Entries should not be renumbered and
  // numeric values should never be reused.
  enum class UserAdjustmentEffect {
    // Completely disable Adapter until browser restarts.
    kDisableAuto = 0,
    // Pause Adapter until system is suspended and then resumed.
    kPauseAuto = 1,
    // No impact on Adapter and Adapter continues to auto-adjust brightness.
    kContinueAuto = 2,
    kMaxValue = kContinueAuto
  };

  // The values in Params can be overridden by experiment flags.
  // TODO(jiameng): move them to cros config json file once experiments are
  // complete.
  struct Params {
    Params();

    // Brightness is only changed if
    // 1. the log of average ambient value has gone up (resp. down) by
    //    |brightening_log_lux_threshold| (resp. |darkening_log_lux_threshold|)
    //    from the reference value. The reference value is the average ALS when
    //    brightness was changed last time (by user or model).
    //   and
    // 2. the std-dev of ALS within the averaging period is less than
    //    |stabilization_threshold| multiplied by the brightening/darkening
    //    thresholds to show the ALS has stabilized.
    double brightening_log_lux_threshold = 0.6;
    double darkening_log_lux_threshold = 0.6;
    double stabilization_threshold = 0.15;

    ModelCurve model_curve = ModelCurve::kLatest;

    // Average ambient value is calculated over the past
    // |auto_brightness_als_horizon|. This is only used for brightness update,
    // which can be different from the horizon used in model training.
    base::TimeDelta auto_brightness_als_horizon =
        base::TimeDelta::FromSeconds(4);

    UserAdjustmentEffect user_adjustment_effect =
        UserAdjustmentEffect::kDisableAuto;

    std::string metrics_key;
  };

  // These values are persisted to logs. Entries should not be renumbered and
  // numeric values should never be reused.
  enum class Status {
    kInitializing = 0,
    kSuccess = 1,
    kDisabled = 2,
    kMaxValue = kDisabled
  };

  // These values are persisted to logs. Entries should not be renumbered and
  // numeric values should never be reused.
  enum class BrightnessChangeCause {
    kInitialAlsReceived = 0,
    // Deprecated.
    kImmediateBrightneningThresholdExceeded = 1,
    // Deprecated.
    kImmediateDarkeningThresholdExceeded = 2,
    kBrightneningThresholdExceeded = 3,
    kDarkeningThresholdExceeded = 4,
    kMaxValue = kDarkeningThresholdExceeded
  };

  // These values are persisted to logs. Entries should not be renumbered and
  // numeric values should never be reused.
  enum class NoBrightnessChangeCause {
    kWaitingForInitialAls = 0,
    kWaitingForAvgHorizon = 1,
    // |log_als_values_| is empty.
    kMissingAlsData = 2,
    // User manually changed brightness before and it stopped adapter from
    // changing brightness.
    kDisabledByUser = 3,
    kBrightnessSetByPolicy = 4,
    // ALS increased beyond the brightening threshold, but ALS data has been
    // fluctuating above the stabilization threshold.
    kFluctuatingAlsIncrease = 5,
    // ALS decreased beyond the darkening threshold, but ALS data has been
    // fluctuating above the stabilization threshold.
    kFluctuatingAlsDecrease = 6,
    // ALS change is within darkening and brightening thresholds.
    kMinimalAlsChange = 7,
    // Adapter should only use personal curves but none is available.
    kMissingPersonalCurve = 8,
    kMaxValue = kMissingPersonalCurve
  };

  struct AdapterDecision {
    AdapterDecision();
    AdapterDecision(const AdapterDecision& decision);
    // If |no_brightness_change_cause| is not nullopt, then brightness
    // should not be changed.
    // If |brightness_change_cause| is not nullopt, then brightness should be
    // changed. In this case |log_als_avg_stddev| should not be nullopt.
    // Exactly one of |no_brightness_change_cause| and
    // |brightness_change_cause| should be non-nullopt.
    // |log_als_avg_stddev| may be set even when brightness should not be
    // changed. It is only nullopt if there is no ALS data in the data cache.
    base::Optional<NoBrightnessChangeCause> no_brightness_change_cause;
    base::Optional<BrightnessChangeCause> brightness_change_cause;
    base::Optional<AlsAvgStdDev> log_als_avg_stddev;
  };

  Adapter(Profile* profile,
          AlsReader* als_reader,
          BrightnessMonitor* brightness_monitor,
          Modeller* modeller,
          ModelConfigLoader* model_config_loader,
          MetricsReporter* metrics_reporter,
          chromeos::PowerManagerClient* power_manager_client);
  ~Adapter() override;

  // AlsReader::Observer overrides:
  void OnAmbientLightUpdated(int lux) override;
  void OnAlsReaderInitialized(AlsReader::AlsInitStatus status) override;

  // BrightnessMonitor::Observer overrides:
  void OnBrightnessMonitorInitialized(bool success) override;
  void OnUserBrightnessChanged(double old_brightness_percent,
                               double new_brightness_percent) override;
  void OnUserBrightnessChangeRequested() override;

  // Modeller::Observer overrides:
  void OnModelTrained(const MonotoneCubicSpline& brightness_curve) override;
  void OnModelInitialized(
      const base::Optional<MonotoneCubicSpline>& global_curve,
      const base::Optional<MonotoneCubicSpline>& personal_curve) override;

  // ModelConfigLoader::Observer overrides:
  void OnModelConfigLoaded(base::Optional<ModelConfig> model_config) override;

  // chromeos::PowerManagerClient::Observer overrides:
  void SuspendDone(const base::TimeDelta& sleep_duration) override;

  Status GetStatusForTesting() const;

  // Only returns true if Adapter status is success and it's not disabled by
  // user adjustment.
  bool IsAppliedForTesting() const;
  base::Optional<MonotoneCubicSpline> GetGlobalCurveForTesting() const;
  base::Optional<MonotoneCubicSpline> GetPersonalCurveForTesting() const;

  // Returns the average and std-dev over |log_als_values_|.
  base::Optional<AlsAvgStdDev> GetAverageAmbientWithStdDevForTesting(
      base::TimeTicks now);
  double GetBrighteningThresholdForTesting() const;
  double GetDarkeningThresholdForTesting() const;

  // Returns |average_log_ambient_lux_|.
  base::Optional<double> GetCurrentAvgLogAlsForTesting() const;

  static std::unique_ptr<Adapter> CreateForTesting(
      Profile* profile,
      AlsReader* als_reader,
      BrightnessMonitor* brightness_monitor,
      Modeller* modeller,
      ModelConfigLoader* model_config_loader,
      MetricsReporter* metrics_reporter,
      chromeos::PowerManagerClient* power_manager_client,
      const base::TickClock* tick_clock);

 private:
  Adapter(Profile* profile,
          AlsReader* als_reader,
          BrightnessMonitor* brightness_monitor,
          Modeller* modeller,
          ModelConfigLoader* model_config_loader,
          MetricsReporter* metrics_reporter,
          chromeos::PowerManagerClient* power_manager_client,
          const base::TickClock* tick_clock);

  // Called by |OnModelConfigLoaded|. It will initialize all params used by
  // the modeller from |model_config| and also other experiment flags. If
  // any param is invalid, it will disable the adapter.
  void InitParams(const ModelConfig& model_config);

  // Called when powerd becomes available.
  void OnPowerManagerServiceAvailable(bool service_is_ready);

  // Called to update |adapter_status_| when there's some status change from
  // AlsReader, BrightnessMonitor, Modeller, power manager and after
  // |InitParams|.
  void UpdateStatus();

  // Checks whether brightness should be changed.
  // This is generally the case when the brightness hasn't been manually
  // set, we've received enough initial ambient light readings, and
  // the ambient light has changed beyond thresholds and has stabilized, and
  // also if personal curve exists (if param says we should only use personal
  // curve).
  AdapterDecision CanAdjustBrightness(base::TimeTicks now);

  // Changes the brightness. In addition to asking powerd to
  // change brightness, it also calls |OnBrightnessChanged| and writes to logs.
  void AdjustBrightness(BrightnessChangeCause cause, double log_als_avg);

  // Calculates brightness from given |ambient_log_lux| based on either
  // |global_curve_| or |personal_curve_| (as specified by the experiment
  // params). It's only safe to call this method when |CanAdjustBrightness|
  // returns a |BrightnessChangeCause| in its decision.
  double GetBrightnessBasedOnAmbientLogLux(double ambient_log_lux) const;

  // Called when brightness is changed by the model or user. This function
  // updates |latest_brightness_change_time_|, |current_brightness_|. If
  // |new_log_als| is not nullopt, it will also update
  // |average_log_ambient_lux_| and thresholds. |new_log_als| should be
  // available when this function is called, but may be nullopt when a user
  // changes brightness before any ALS reading comes in. We log an error if this
  // happens.
  void OnBrightnessChanged(base::TimeTicks now,
                           double new_brightness_percent,
                           base::Optional<double> new_log_als);

  // Called by |AdjustBrightness| when brightness should be changed.
  void WriteLogMessages(double new_log_als,
                        double new_brightness,
                        BrightnessChangeCause cause) const;

  Profile* const profile_;

  ScopedObserver<AlsReader, AlsReader::Observer> als_reader_observer_;
  ScopedObserver<BrightnessMonitor, BrightnessMonitor::Observer>
      brightness_monitor_observer_;
  ScopedObserver<Modeller, Modeller::Observer> modeller_observer_;

  ScopedObserver<ModelConfigLoader, ModelConfigLoader::Observer>
      model_config_loader_observer_;

  ScopedObserver<chromeos::PowerManagerClient,
                 chromeos::PowerManagerClient::Observer>
      power_manager_client_observer_;

  // Used to report daily metrics to UMA. This may be null in unit tests.
  MetricsReporter* metrics_reporter_;

  chromeos::PowerManagerClient* const power_manager_client_;

  Params params_;

  // This will be replaced by a mock tick clock during tests.
  const base::TickClock* tick_clock_;

  // TODO(jiameng): refactor internal states and flags.

  // This buffer will be used to store the recent ambient light values in the
  // log space.
  std::unique_ptr<AmbientLightSampleBuffer> log_als_values_;

  base::Optional<AlsReader::AlsInitStatus> als_init_status_;
  // Time when AlsReader is initialized.
  base::TimeTicks als_init_time_;

  base::Optional<bool> brightness_monitor_success_;

  // |model_config_exists_| will remain nullopt until |OnModelConfigLoaded| is
  // called. Its value will then be set to true if the input model config exists
  // (not nullopt), else its value will be false.
  base::Optional<bool> model_config_exists_;

  bool model_initialized_ = false;

  base::Optional<bool> power_manager_service_available_;

  Status adapter_status_ = Status::kInitializing;

  // This is set to true whenever a user makes a manual adjustment, and if
  // |params_.user_adjustment_effect| is not |kContinueAuto|. It will be
  // reset to false if |params_.user_adjustment_effect| is |kPauseAuto|.
  // It won't be set/reset if adapter is disabled because it won't be necessary
  // to check |adapter_disabled_by_user_adjustment_|.
  bool adapter_disabled_by_user_adjustment_ = false;

  // The thresholds are calculated from the |average_log_ambient_lux_|.
  // They are only updated when brightness is changed (either by user or model).
  base::Optional<double> brightening_threshold_;
  base::Optional<double> darkening_threshold_;

  base::Optional<MonotoneCubicSpline> global_curve_;
  base::Optional<MonotoneCubicSpline> personal_curve_;

  // |average_log_ambient_lux_| is only recorded when screen brightness is
  // changed by either model or user. New thresholds will be calculated from it.
  base::Optional<double> average_log_ambient_lux_;

  // Last time brightness change occurred, either by user or model.
  base::TimeTicks latest_brightness_change_time_;

  // Last time brightness was changed by the model.
  base::TimeTicks latest_model_brightness_change_time_;

  // Current recorded brightness. It can be either the user requested brightness
  // or the model requested brightness.
  base::Optional<double> current_brightness_;

  // Used to record number of model-triggered brightness changes.
  int model_brightness_change_counter_ = 1;

  base::WeakPtrFactory<Adapter> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(Adapter);
};

}  // namespace auto_screen_brightness
}  // namespace power
}  // namespace chromeos

#endif  // CHROME_BROWSER_CHROMEOS_POWER_AUTO_SCREEN_BRIGHTNESS_ADAPTER_H_
