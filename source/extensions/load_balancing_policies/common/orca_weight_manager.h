#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/time.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/callback_impl.h"
#include "source/common/common/logger.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace Common {

using OrcaLoadReportProto = xds::data::orca::v3::OrcaLoadReport;

/**
 * Plain struct holding ORCA weight-management parameters.
 */
struct OrcaWeightManagerConfig {
  std::vector<std::string> metric_names_for_computing_utilization;
  double error_utilization_penalty;
  std::chrono::milliseconds blackout_period;
  std::chrono::milliseconds weight_expiration_period;
  std::chrono::milliseconds weight_update_period;
};

/**
 * Handles ORCA load reports and calculates host weights.
 */
class OrcaLoadReportHandler {
public:
  OrcaLoadReportHandler(const OrcaWeightManagerConfig& config, TimeSource& time_source);

  absl::Status
  updateClientSideDataFromOrcaLoadReport(const OrcaLoadReportProto& orca_load_report,
                                         struct OrcaHostLbPolicyData& client_side_data);

  static double getUtilizationFromOrcaReport(
      const OrcaLoadReportProto& orca_load_report,
      const std::vector<std::string>& metric_names_for_computing_utilization);

  static absl::StatusOr<uint32_t> calculateWeightFromOrcaReport(
      const OrcaLoadReportProto& orca_load_report,
      const std::vector<std::string>& metric_names_for_computing_utilization,
      double error_utilization_penalty);

  const std::vector<std::string> metric_names_for_computing_utilization_;
  const double error_utilization_penalty_;
  TimeSource& time_source_;
};

using OrcaLoadReportHandlerSharedPtr = std::shared_ptr<OrcaLoadReportHandler>;

/**
 * Per-host data that stores ORCA-derived weights. Thread-safe via atomics.
 */
struct OrcaHostLbPolicyData : public Envoy::Upstream::HostLbPolicyData {
  OrcaHostLbPolicyData(OrcaLoadReportHandlerSharedPtr handler)
      : report_handler_(std::move(handler)) {}
  OrcaHostLbPolicyData(OrcaLoadReportHandlerSharedPtr handler, uint32_t weight,
                       MonotonicTime non_empty_since, MonotonicTime last_update_time)
      : report_handler_(std::move(handler)), weight_(weight), non_empty_since_(non_empty_since),
        last_update_time_(last_update_time) {}

  absl::Status onOrcaLoadReport(const Upstream::OrcaLoadReport& report,
                                const StreamInfo::StreamInfo& stream_info) override;

  void updateWeightNow(uint32_t weight, const MonotonicTime& now) {
    weight_.store(weight);
    last_update_time_.store(now);
    if (non_empty_since_.load() == kDefaultNonEmptySince) {
      non_empty_since_.store(now);
    }
  }

  absl::optional<uint32_t> getWeightIfValid(MonotonicTime max_non_empty_since,
                                            MonotonicTime min_last_update_time) {
    if (max_non_empty_since < non_empty_since_.load()) {
      return std::nullopt;
    }
    if (last_update_time_.load() < min_last_update_time) {
      non_empty_since_.store(OrcaHostLbPolicyData::kDefaultNonEmptySince);
      return std::nullopt;
    }
    return weight_;
  }

  OrcaLoadReportHandlerSharedPtr report_handler_;

  std::atomic<uint32_t> weight_ = 1;
  std::atomic<MonotonicTime> non_empty_since_ = kDefaultNonEmptySince;
  std::atomic<MonotonicTime> last_update_time_ = kDefaultLastUpdateTime;

  static constexpr MonotonicTime kDefaultNonEmptySince = MonotonicTime::max();
  static constexpr MonotonicTime kDefaultLastUpdateTime = MonotonicTime::min();
};

/**
 * Manages ORCA-based host weight updates. Owns the timer, priority-set callback,
 * and report handler. Delegates weight-application to a callback provided by
 * the owning load balancer.
 */
class OrcaWeightManager : protected Logger::Loggable<Logger::Id::upstream> {
public:
  /**
   * @param config ORCA weight management parameters.
   * @param priority_set the priority set to monitor for host changes.
   * @param time_source time source for monotonic timestamps.
   * @param dispatcher main-thread dispatcher for creating the timer.
   * @param on_weights_updated callback invoked when host weights are updated.
   */
  OrcaWeightManager(const OrcaWeightManagerConfig& config,
                    const Upstream::PrioritySet& priority_set, TimeSource& time_source,
                    Event::Dispatcher& dispatcher, std::function<void()> on_weights_updated);

  /**
   * Attach host data, register priority-update callback, and start the timer.
   * Must be called from the main thread after construction.
   */
  absl::Status initialize();

  /** Update weights for all priority sets. Public for test access. */
  void updateWeightsOnMainThread();

  /** Update weights for a single host vector. Public for test access. Returns true if any updated.
   */
  bool updateWeightsOnHosts(const Upstream::HostVector& hosts);

  /** Accessor for the report handler (used by tests). */
  OrcaLoadReportHandlerSharedPtr reportHandler() const { return report_handler_; }

private:
  void addLbPolicyDataToHosts(const Upstream::HostVector& hosts);

  static absl::optional<uint32_t> getWeightIfValidFromHost(const Upstream::Host& host,
                                                           MonotonicTime max_non_empty_since,
                                                           MonotonicTime min_last_update_time);

  OrcaLoadReportHandlerSharedPtr report_handler_;
  const Upstream::PrioritySet& priority_set_;
  TimeSource& time_source_;

  std::chrono::milliseconds blackout_period_;
  std::chrono::milliseconds weight_expiration_period_;
  std::chrono::milliseconds weight_update_period_;

  Event::TimerPtr weight_calculation_timer_;
  ::Envoy::Common::CallbackHandlePtr priority_update_cb_;

  std::function<void()> on_weights_updated_;
};

} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
