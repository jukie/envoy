#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/random_generator.h"
#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/http/codec.h"
#include "envoy/network/transport_socket.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/callback_impl.h"
#include "source/common/common/logger.h"
#include "source/common/http/codec_client.h"
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

  // Out-of-band ORCA reporting configuration.
  // When `oob_enabled` is true, the manager will open per-host ORCA OOB streams
  // (xds.service.orca.v3.OpenRcaService.StreamCoreMetrics) to receive load
  // reports out-of-band and feed them into the same OrcaLoadReportHandler.
  bool oob_enabled{false};
  std::chrono::milliseconds oob_reporting_period{};
  std::vector<std::string> oob_request_cost_names;
};

/**
 * All OrcaWeightManager OOB stats. @see stats_macros.h
 */
#define ALL_ORCA_OOB_STATS(COUNTER, GAUGE)                                                         \
  COUNTER(reports_received)                                                                        \
  COUNTER(report_errors)                                                                           \
  COUNTER(stream_failures)                                                                         \
  COUNTER(stream_opens)                                                                            \
  COUNTER(stream_terminated)                                                                       \
  GAUGE(active_sessions, Accumulate)

/**
 * Definition of all OrcaWeightManager OOB stats. @see stats_macros.h
 */
struct OrcaOobStats {
  ALL_ORCA_OOB_STATS(GENERATE_COUNTER_STRUCT, GENERATE_GAUGE_STRUCT)
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
  explicit OrcaHostLbPolicyData(OrcaLoadReportHandlerSharedPtr handler)
      : report_handler_(std::move(handler)) {}
  OrcaHostLbPolicyData(OrcaLoadReportHandlerSharedPtr handler, uint32_t weight,
                       MonotonicTime non_empty_since, MonotonicTime last_update_time)
      : report_handler_(std::move(handler)), weight_(weight), non_empty_since_(non_empty_since),
        last_update_time_(last_update_time) {}

  bool receivesOrcaLoadReport() const override { return true; }
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
 * Per-attempt codec client factory used by OrcaWeightManager when creating
 * OrcaOobSession instances. The factory takes the connection data produced by
 * `Host::createOrcaReportingConnection` and wraps it in an Http::CodecClient.
 *
 * NOTE: production implementations construct CodecClientProd, which initiates
 * the underlying connection synchronously inside its constructor. The factory
 * must therefore only be invoked from inside the per-attempt lambda passed to
 * the OrcaOobSession (i.e. AFTER the staggered start timer fires).
 *
 * The `random` and `transport_socket_options` are passed in at the call site by
 * the manager so that there is a single source of truth for these dependencies
 * (the manager itself), rather than duplicating them on the factory. This
 * prevents accidental drift between the manager's connection-creation path
 * and the codec-client construction path.
 */
class OrcaOobCodecClientFactory {
public:
  virtual ~OrcaOobCodecClientFactory() = default;
  virtual Http::CodecClientPtr
  create(Upstream::Host::CreateConnectionData&& connection_data, Event::Dispatcher& dispatcher,
         Random::RandomGenerator& random,
         Network::TransportSocketOptionsConstSharedPtr transport_socket_options) const PURE;
};

using OrcaOobCodecClientFactoryPtr = std::unique_ptr<OrcaOobCodecClientFactory>;

/**
 * Production OrcaOobCodecClientFactory implementation that builds an HTTP/2
 * CodecClientProd around the supplied connection. Holds no state of its own;
 * `random` and `transport_socket_options` are supplied by the caller (the
 * OrcaWeightManager) at each invocation.
 */
// Wired up by CSWRR in client_side_weighted_round_robin_lb.cc — see Agent D.
class ProdOrcaOobCodecClientFactory : public OrcaOobCodecClientFactory {
public:
  Http::CodecClientPtr
  create(Upstream::Host::CreateConnectionData&& connection_data, Event::Dispatcher& dispatcher,
         Random::RandomGenerator& random,
         Network::TransportSocketOptionsConstSharedPtr transport_socket_options) const override;
};

/**
 * Manages ORCA-based weight computation for hosts in a priority set.
 * Extracted from ClientSideWeightedRoundRobinLoadBalancer to allow reuse.
 *
 * When out-of-band ORCA reporting is enabled in the config, this manager owns
 * one OrcaOobSession per host in the priority set. Sessions are scheduled with
 * a staggered start so that newly added hosts do not all reach for their
 * upstream connection at the same instant.
 */
class OrcaWeightManager : protected Logger::Loggable<Logger::Id::upstream> {
public:
  using WeightsUpdatedCb = std::function<void()>;

  OrcaWeightManager(const OrcaWeightManagerConfig& config,
                    const Upstream::PrioritySet& priority_set, TimeSource& time_source,
                    Event::Dispatcher& dispatcher, Random::RandomGenerator& random,
                    Stats::Scope& stats_scope,
                    Network::TransportSocketOptionsConstSharedPtr transport_socket_options,
                    OrcaOobCodecClientFactoryPtr codec_client_factory,
                    WeightsUpdatedCb on_weights_updated);

  ~OrcaWeightManager();

  // Attach host data, register priority-update callback, start timer.
  absl::Status initialize();

  // Iterate priority sets, call on_weights_updated_ if changed.
  void updateWeightsOnMainThread();

  // Core weight update (blackout, expiration, median default). Returns true if any weight changed.
  bool updateWeightsOnHosts(const Upstream::HostVector& hosts);

  // Accessor for the report handler (used by tests and for creating host data).
  OrcaLoadReportHandlerSharedPtr reportHandler() { return report_handler_; }

  // Accessor for stats (test-only).
  const OrcaOobStats& oobStatsForTest() const { return oob_stats_; }

  // Get weight based on host LB policy data if valid, otherwise return nullopt.
  static absl::optional<uint32_t> getWeightIfValidFromHost(const Upstream::Host& host,
                                                           MonotonicTime max_non_empty_since,
                                                           MonotonicTime min_last_update_time);

private:
  // Add LB policy data to all hosts that don't already have it.
  void addLbPolicyDataToHosts(const Upstream::HostVector& hosts);

  // OOB session lifecycle helpers.
  void onHostsAdded(const Upstream::HostVector& hosts);
  void onHostsRemoved(const Upstream::HostVector& hosts);
  // Schedule a staggered start for `host` if OOB is enabled and there is no
  // pending timer or active session for this host.
  void schedulePendingOobSession(const Upstream::HostSharedPtr& host);
  // Construct and start the OrcaOobSession for `host`. Called from the stagger
  // timer callback.
  void startOobSession(const Upstream::HostSharedPtr& host);
  // Compute a randomized stagger delay in [0, oob_reporting_period_).
  std::chrono::milliseconds computeStaggerDelay();

  static OrcaOobStats generateOrcaOobStats(Stats::Scope& scope);

  OrcaLoadReportHandlerSharedPtr report_handler_;
  const Upstream::PrioritySet& priority_set_;
  TimeSource& time_source_;
  Event::Dispatcher& dispatcher_;
  Random::RandomGenerator& random_;
  Stats::Scope& stats_scope_;
  const Network::TransportSocketOptionsConstSharedPtr transport_socket_options_;
  OrcaOobCodecClientFactoryPtr codec_client_factory_;

  // Timing parameters for the weight update.
  std::chrono::milliseconds blackout_period_;
  std::chrono::milliseconds weight_expiration_period_;
  std::chrono::milliseconds weight_update_period_;

  // OOB configuration (cached from OrcaWeightManagerConfig).
  const bool oob_enabled_;
  const std::chrono::milliseconds oob_reporting_period_;
  const std::vector<std::string> oob_request_cost_names_;

  OrcaOobStats oob_stats_;

  Event::TimerPtr weight_calculation_timer_;
  // Callback for priority_set_ updates.
  Envoy::Common::CallbackHandlePtr priority_update_cb_;
  // Callback invoked when weights are updated.
  WeightsUpdatedCb on_weights_updated_;

  // Active OrcaOobSession per host. Sessions are deferred-deleted on removal /
  // terminal callback.
  absl::flat_hash_map<Upstream::HostConstSharedPtr, Envoy::Orca::OrcaOobSessionPtr> oob_sessions_;
  // Stagger timers awaiting their initial firing per host. A host has at most
  // one entry in this map OR in oob_sessions_, never both.
  absl::flat_hash_map<Upstream::HostConstSharedPtr, Event::TimerPtr> pending_oob_session_timers_;
};

} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
