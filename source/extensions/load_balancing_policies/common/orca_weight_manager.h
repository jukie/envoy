#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/random_generator.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/callback_impl.h"
#include "source/common/common/logger.h"
#include "source/common/orca/orca_oob_session.h"

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace Common {

using OrcaLoadReportProto = xds::data::orca::v3::OrcaLoadReport;

/**
 * Configuration for OrcaWeightManager.
 */
struct OrcaWeightManagerConfig {
  std::vector<std::string> metric_names_for_computing_utilization;
  double error_utilization_penalty;
  std::chrono::milliseconds blackout_period;
  std::chrono::milliseconds weight_expiration_period;
  std::chrono::milliseconds weight_update_period;
  // TODO(jukie): wire from CSWRR proto in follow-up PR. Currently always
  // false / default -- no public config surface yet.
  bool enable_oob_load_report{false};
  std::chrono::milliseconds oob_reporting_period{10000}; // 10s
};

struct OrcaHostLbPolicyData;

/**
 * Handles ORCA load reports and computes host weights.
 * Stores the config necessary to calculate host weight based on the report.
 */
class OrcaLoadReportHandler {
public:
  OrcaLoadReportHandler(const OrcaWeightManagerConfig& config, TimeSource& time_source);

  // Update client side data from `orca_load_report`. Invoked from `onOrcaLoadReport` callback of
  // OrcaHostLbPolicyData.
  absl::Status updateClientSideDataFromOrcaLoadReport(const OrcaLoadReportProto& orca_load_report,
                                                      OrcaHostLbPolicyData& client_side_data);

  // Get utilization from `orca_load_report` using named metrics specified in
  // `metric_names_for_computing_utilization`.
  static double getUtilizationFromOrcaReport(
      const OrcaLoadReportProto& orca_load_report,
      const std::vector<std::string>& metric_names_for_computing_utilization);

  // Calculate client side weight from `orca_load_report` using `getUtilizationFromOrcaReport()`,
  // QPS, EPS and `error_utilization_penalty`.
  static absl::StatusOr<uint32_t> calculateWeightFromOrcaReport(
      const OrcaLoadReportProto& orca_load_report,
      const std::vector<std::string>& metric_names_for_computing_utilization,
      double error_utilization_penalty);

private:
  const std::vector<std::string> metric_names_for_computing_utilization_;
  const double error_utilization_penalty_;
  TimeSource& time_source_;
};

using OrcaLoadReportHandlerSharedPtr = std::shared_ptr<OrcaLoadReportHandler>;

/**
 * Per-host LB policy data storing ORCA-derived weight and timestamps.
 * Hosts are not shared between different clusters, but are shared between load
 * balancer instances on different threads.
 */
struct OrcaHostLbPolicyData : public Envoy::Upstream::HostLbPolicyData {
  explicit OrcaHostLbPolicyData(OrcaLoadReportHandlerSharedPtr handler, bool oob_configured = false)
      : report_handler_(std::move(handler)), oob_configured_(oob_configured) {}
  OrcaHostLbPolicyData(OrcaLoadReportHandlerSharedPtr handler, uint32_t weight,
                       MonotonicTime non_empty_since, MonotonicTime last_update_time,
                       bool oob_configured = false)
      : report_handler_(std::move(handler)), oob_configured_(oob_configured), weight_(weight),
        non_empty_since_(non_empty_since), last_update_time_(last_update_time) {}

  bool receivesOrcaLoadReport() const override { return true; }
  bool oobReportingConfigured() const override { return oob_configured_; }
  absl::Status onOrcaLoadReport(const Upstream::OrcaLoadReport& report,
                                const StreamInfo::StreamInfo& stream_info) override;

  // Update the weight and timestamps for first and last update time.
  void updateWeightNow(uint32_t weight, const MonotonicTime& now) {
    weight_.store(weight);
    last_update_time_.store(now);
    if (non_empty_since_.load() == kDefaultNonEmptySince) {
      non_empty_since_.store(now);
    }
  }

  // Get the weight if it was updated between max_non_empty_since and min_last_update_time,
  // otherwise return nullopt.
  absl::optional<uint32_t> getWeightIfValid(MonotonicTime max_non_empty_since,
                                            MonotonicTime min_last_update_time) {
    // If non_empty_since_ is too recent, we should use the default weight.
    if (max_non_empty_since < non_empty_since_.load()) {
      return absl::nullopt;
    }
    // If last update time is too old, we should use the default weight.
    if (last_update_time_.load() < min_last_update_time) {
      // Reset the non_empty_since_ time so the timer will start again.
      non_empty_since_.store(OrcaHostLbPolicyData::kDefaultNonEmptySince);
      return absl::nullopt;
    }
    return weight_;
  }

  OrcaLoadReportHandlerSharedPtr report_handler_;
  const bool oob_configured_{false};

  // Weight as calculated from the last load report.
  std::atomic<uint32_t> weight_ = 1;
  // Time when the weight is first updated. The weight is invalid if it is within of
  // `blackout_period_`.
  std::atomic<MonotonicTime> non_empty_since_ = kDefaultNonEmptySince;
  // Time when the weight is last updated. The weight is invalid if it is outside of
  // `expiration_period_`.
  std::atomic<MonotonicTime> last_update_time_ = kDefaultLastUpdateTime;

  static constexpr MonotonicTime kDefaultNonEmptySince = MonotonicTime::max();
  static constexpr MonotonicTime kDefaultLastUpdateTime = MonotonicTime::min();
};

/**
 * Manages ORCA-based weight computation for hosts in a priority set.
 * Extracted from ClientSideWeightedRoundRobinLoadBalancer to allow reuse.
 *
 * Stat scope: when OOB load reporting is enabled, the manager publishes its
 * counters and gauges under the unscoped "orca_oob." prefix on `stats_scope`.
 * Two OrcaWeightManager instances sharing a scope would silently share those
 * counters, so a given cluster scope must host at most one OOB-enabled
 * manager at a time. This is a per-cluster invariant today because CSWRR is
 * the only consumer; if that changes the scope should be suffixed.
 */
class OrcaWeightManager : protected Logger::Loggable<Logger::Id::upstream> {
public:
  using WeightsUpdatedCb = std::function<void()>;

  OrcaWeightManager(const OrcaWeightManagerConfig& config,
                    const Upstream::PrioritySet& priority_set, TimeSource& time_source,
                    Event::Dispatcher& dispatcher, WeightsUpdatedCb on_weights_updated,
                    Random::RandomGenerator& random, Stats::Scope& stats_scope);
  virtual ~OrcaWeightManager();

  // Attach host data, register priority-update callback, start timer.
  absl::Status initialize();

  // Iterate priority sets, call on_weights_updated_ if changed.
  void updateWeightsOnMainThread();

  // Core weight update (blackout, expiration, median default). Returns true if any weight changed.
  bool updateWeightsOnHosts(const Upstream::HostVector& hosts);

  // Accessor for the report handler (used by tests and for creating host data).
  OrcaLoadReportHandlerSharedPtr reportHandler() { return report_handler_; }

  // Get weight based on host LB policy data if valid, otherwise return nullopt.
  static absl::optional<uint32_t> getWeightIfValidFromHost(const Upstream::Host& host,
                                                           MonotonicTime max_non_empty_since,
                                                           MonotonicTime min_last_update_time);

protected:
  // Virtual for test override. Creates an OOB session for a host.
  virtual Orca::OrcaOobSessionPtr createOobSession(const Upstream::HostSharedPtr& host,
                                                   Orca::OrcaOobCallbacks& callbacks,
                                                   Orca::OrcaOobStats& stats);

private:
  // Test-only friendship grants TestOrcaWeightManager access to
  // dispatcher_ / oob_reporting_period_ / random_ without exposing them
  // on the public/protected surface.
  friend class TestOrcaWeightManager;

  Event::Dispatcher& dispatcher_;
  std::chrono::milliseconds oob_reporting_period_;
  Random::RandomGenerator& random_;

  // Add LB policy data to all hosts that don't already have it.
  void addLbPolicyDataToHosts(const Upstream::HostVector& hosts);

  // OOB session management.
  void startOobSession(const Upstream::HostSharedPtr& host);
  void stopOobSessions(const Upstream::HostVector& hosts_removed);

  OrcaLoadReportHandlerSharedPtr report_handler_;
  const Upstream::PrioritySet& priority_set_;
  TimeSource& time_source_;

  // Timing parameters for the weight update.
  std::chrono::milliseconds blackout_period_;
  std::chrono::milliseconds weight_expiration_period_;
  std::chrono::milliseconds weight_update_period_;

  Event::TimerPtr weight_calculation_timer_;
  // Callback for priority_set_ updates.
  Envoy::Common::CallbackHandlePtr priority_update_cb_;
  // Callback invoked when weights are updated.
  WeightsUpdatedCb on_weights_updated_;

  // Per-host OOB callback adapter that routes reports to the host's LB policy data.
  class OobCallbackAdapter : public Orca::OrcaOobCallbacks,
                             public Logger::Loggable<Logger::Id::upstream> {
  public:
    explicit OobCallbackAdapter(Upstream::Host& host) : host_(host) {}
    void onOrcaOobReport(const xds::data::orca::v3::OrcaLoadReport& report) override;
    void onOrcaOobStreamFailure(Grpc::Status::GrpcStatus status) override;

  private:
    Upstream::Host& host_;
  };

  const bool enable_oob_load_report_;

  // OOB sessions keyed by host pointer. Each entry owns both the session and callback adapter.
  // NOTE: `callback` must be declared before `session` so that the session (which holds a
  // reference to the callback) is destroyed first during struct destruction.
  struct OobSessionEntry {
    std::unique_ptr<OobCallbackAdapter> callback;
    Orca::OrcaOobSessionPtr session;
  };
  absl::flat_hash_map<Upstream::Host*, OobSessionEntry> oob_sessions_;
  std::unique_ptr<Orca::OrcaOobStats> oob_stats_;
};

} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
