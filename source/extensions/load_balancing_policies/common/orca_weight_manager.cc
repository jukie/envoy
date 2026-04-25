#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

#include "envoy/upstream/upstream.h"

#include "source/common/common/backoff_strategy.h"
#include "source/common/orca/orca_load_metrics.h"
#include "source/common/orca/orca_oob_session.h"
#include "source/common/runtime/runtime_features.h"

#include "absl/status/status.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace Common {

namespace {

constexpr absl::string_view kOrcaOobStatsPrefix = "orca_oob.";

// Backoff bounds for OOB stream retries. Modeled after typical xDS / health
// checker reconnect backoffs.
constexpr uint64_t kOobBackoffBaseIntervalMs = 1000;
constexpr uint64_t kOobBackoffMaxIntervalMs = 30000;

std::string getHostAddress(const Upstream::Host* host) {
  if (host == nullptr || host->address() == nullptr) {
    return "unknown";
  }
  return host->address()->asString();
}

} // namespace

OrcaLoadReportHandler::OrcaLoadReportHandler(const OrcaWeightManagerConfig& config,
                                             TimeSource& time_source)
    : metric_names_for_computing_utilization_(config.metric_names_for_computing_utilization),
      error_utilization_penalty_(config.error_utilization_penalty), time_source_(time_source) {}

double OrcaLoadReportHandler::getUtilizationFromOrcaReport(
    const OrcaLoadReportProto& orca_load_report,
    const std::vector<std::string>& metric_names_for_computing_utilization) {
  if (Runtime::runtimeFeatureEnabled(
          "envoy.reloadable_features.orca_weight_manager_use_named_metrics_first")) {
    // By default (with runtime feature enabled) we prefer named metrics over application
    // utilization, so if named metrics are not ignored if application utilization is
    // available and set. See https://github.com/envoyproxy/envoy/pull/44196 for discussion.

    // Find the most constrained utilization metric in `metric_names_for_computing_utilization`.
    double utilization =
        Envoy::Orca::getMaxUtilization(metric_names_for_computing_utilization, orca_load_report);
    if (utilization > 0) {
      return utilization;
    }
    // If named metrics are not available, use `application_utilization` as the utilization metric.
    utilization = orca_load_report.application_utilization();
    if (utilization > 0) {
      return utilization;
    }
  } else {
    // With the runtime flag disabled, we use application utilization if it is set over named
    // metrics. This means that metric_names_for_computing_utilization is ignored if
    // application_utilization is available and set.

    // If application_utilization is valid, use it as the utilization metric.
    double utilization = orca_load_report.application_utilization();
    if (utilization > 0) {
      return utilization;
    }
    // Otherwise, find the most constrained utilization metric.
    utilization =
        Envoy::Orca::getMaxUtilization(metric_names_for_computing_utilization, orca_load_report);
    if (utilization > 0) {
      return utilization;
    }
  }
  // If utilization is <= 0, use `cpu_utilization` as the utilization metric.
  return orca_load_report.cpu_utilization();
}

absl::StatusOr<uint32_t> OrcaLoadReportHandler::calculateWeightFromOrcaReport(
    const OrcaLoadReportProto& orca_load_report,
    const std::vector<std::string>& metric_names_for_computing_utilization,
    double error_utilization_penalty) {
  double qps = orca_load_report.rps_fractional();
  if (qps <= 0) {
    return absl::InvalidArgumentError("QPS must be positive");
  }

  double utilization =
      getUtilizationFromOrcaReport(orca_load_report, metric_names_for_computing_utilization);
  // If there are errors, then increase utilization to lower the weight.
  utilization += error_utilization_penalty * orca_load_report.eps() / qps;

  if (utilization <= 0) {
    return absl::InvalidArgumentError("Utilization must be positive");
  }

  // Calculate the weight.
  double weight = qps / utilization;

  // Limit the weight to uint32_t max.
  if (weight > std::numeric_limits<uint32_t>::max()) {
    weight = std::numeric_limits<uint32_t>::max();
  }
  return weight;
}

absl::Status OrcaLoadReportHandler::updateClientSideDataFromOrcaLoadReport(
    const OrcaLoadReportProto& orca_load_report, OrcaHostLbPolicyData& client_side_data) {
  const absl::StatusOr<uint32_t> weight = calculateWeightFromOrcaReport(
      orca_load_report, metric_names_for_computing_utilization_, error_utilization_penalty_);
  if (!weight.ok()) {
    return weight.status();
  }

  // Update client side data attached to the host.
  client_side_data.updateWeightNow(weight.value(), time_source_.monotonicTime());
  return absl::OkStatus();
}

absl::Status OrcaHostLbPolicyData::onOrcaLoadReport(const Upstream::OrcaLoadReport& report,
                                                    const StreamInfo::StreamInfo&) {
  ASSERT(report_handler_ != nullptr);
  return report_handler_->updateClientSideDataFromOrcaLoadReport(report, *this);
}

Http::CodecClientPtr
ProdOrcaOobCodecClientFactory::create(Upstream::Host::CreateConnectionData&& connection_data,
                                      Event::Dispatcher& dispatcher) const {
  // ORCA OOB streams use HTTP/2 to multiplex framing on a single connection.
  return std::make_unique<Http::CodecClientProd>(
      Http::CodecType::HTTP2, std::move(connection_data.connection_),
      connection_data.host_description_, dispatcher, random_, transport_socket_options_);
}

OrcaOobStats OrcaWeightManager::generateOrcaOobStats(Stats::Scope& scope) {
  return {ALL_ORCA_OOB_STATS(POOL_COUNTER_PREFIX(scope, kOrcaOobStatsPrefix),
                             POOL_GAUGE_PREFIX(scope, kOrcaOobStatsPrefix))};
}

OrcaWeightManager::OrcaWeightManager(
    const OrcaWeightManagerConfig& config, const Upstream::PrioritySet& priority_set,
    TimeSource& time_source, Event::Dispatcher& dispatcher, Random::RandomGenerator& random,
    Stats::Scope& stats_scope,
    Network::TransportSocketOptionsConstSharedPtr transport_socket_options,
    OrcaOobCodecClientFactoryPtr codec_client_factory, WeightsUpdatedCb on_weights_updated)
    : report_handler_(std::make_shared<OrcaLoadReportHandler>(config, time_source)),
      priority_set_(priority_set), time_source_(time_source), dispatcher_(dispatcher),
      random_(random), stats_scope_(stats_scope),
      transport_socket_options_(std::move(transport_socket_options)),
      codec_client_factory_(std::move(codec_client_factory)),
      blackout_period_(config.blackout_period),
      weight_expiration_period_(config.weight_expiration_period),
      weight_update_period_(config.weight_update_period), oob_enabled_(config.oob_enabled),
      oob_reporting_period_(config.oob_reporting_period),
      oob_request_cost_names_(config.oob_request_cost_names),
      oob_stats_(generateOrcaOobStats(stats_scope_)),
      on_weights_updated_(std::move(on_weights_updated)) {
  // OOB requires a codec client factory whenever it is enabled.
  if (oob_enabled_) {
    ASSERT(codec_client_factory_ != nullptr);
    ASSERT(oob_reporting_period_.count() > 0);
  }
  weight_calculation_timer_ = dispatcher_.createTimer([this]() -> void {
    updateWeightsOnMainThread();
    weight_calculation_timer_->enableTimer(weight_update_period_);
  });
}

OrcaWeightManager::~OrcaWeightManager() {
  // Cancel any pending stagger timers (their TimerPtr destructors disable the
  // underlying timer); deferred-delete every active session so they tear down
  // their codec clients on the next dispatcher iteration without re-entering
  // user callbacks from inside our destructor frame.
  pending_oob_session_timers_.clear();
  for (auto& kv : oob_sessions_) {
    if (kv.second != nullptr) {
      kv.second->close();
      dispatcher_.deferredDelete(std::move(kv.second));
    }
  }
  oob_sessions_.clear();
  // Defensive: ensure the gauge reflects the empty session map after teardown.
  oob_stats_.active_sessions_.set(0);
}

absl::Status OrcaWeightManager::initialize() {
  // Ensure that all hosts have LB policy data.
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    addLbPolicyDataToHosts(host_set->hosts());
    if (oob_enabled_) {
      onHostsAdded(host_set->hosts());
    }
  }

  // Setup a callback to receive priority set updates.
  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t, const Upstream::HostVector& hosts_added,
             const Upstream::HostVector& hosts_removed) {
        addLbPolicyDataToHosts(hosts_added);
        if (oob_enabled_) {
          onHostsRemoved(hosts_removed);
          onHostsAdded(hosts_added);
        }
        updateWeightsOnMainThread();
      });

  weight_calculation_timer_->enableTimer(weight_update_period_);

  return absl::OkStatus();
}

void OrcaWeightManager::updateWeightsOnMainThread() {
  ENVOY_LOG(trace, "updateWeightsOnMainThread");
  bool updated = false;
  // Update weights on hosts in priority set of the thread aware load balancer
  // on the main thread.
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    updated = updateWeightsOnHosts(host_set->hosts()) || updated;
  }
  if (updated) {
    on_weights_updated_();
  }
}

bool OrcaWeightManager::updateWeightsOnHosts(const Upstream::HostVector& hosts) {
  std::vector<uint32_t> weights;
  Upstream::HostVector hosts_with_default_weight;
  bool weights_updated = false;
  const MonotonicTime now = time_source_.monotonicTime();
  // Weight is considered invalid (too recent) if it was first updated within `blackout_period_`.
  const MonotonicTime max_non_empty_since = now - blackout_period_;
  // Weight is considered invalid (too old) if it was last updated before
  // `weight_expiration_period_`.
  const MonotonicTime min_last_update_time = now - weight_expiration_period_;
  weights.reserve(hosts.size());
  hosts_with_default_weight.reserve(hosts.size());
  ENVOY_LOG(trace, "updateWeights hosts.size() = {}, time since epoch = {}", hosts.size(),
            now.time_since_epoch().count());
  // Scan through all hosts and update their weights if they are valid.
  for (const auto& host_ptr : hosts) {
    // Get client side weight or `nullopt` if it is invalid (see above).
    absl::optional<uint32_t> client_side_weight =
        getWeightIfValidFromHost(*host_ptr, max_non_empty_since, min_last_update_time);
    // If `client_side_weight` is valid, then set it as the host weight and store it in
    // `weights` to calculate median valid weight across all hosts.
    if (client_side_weight.has_value()) {
      const uint32_t new_weight = client_side_weight.value();
      weights.push_back(new_weight);
      if (new_weight != host_ptr->weight()) {
        host_ptr->weight(new_weight);
        ENVOY_LOG(trace, "updateWeights hostWeight {} = {}", getHostAddress(host_ptr.get()),
                  host_ptr->weight());
        weights_updated = true;
      }
    } else {
      // If `client_side_weight` is invalid, then set host to default (median) weight.
      hosts_with_default_weight.push_back(host_ptr);
    }
  }
  // If some hosts don't have valid weight, then update them with default weight.
  if (!hosts_with_default_weight.empty()) {
    // Calculate the default weight as median of all valid weights.
    uint32_t default_weight = 1;
    if (!weights.empty()) {
      const auto median_it = weights.begin() + weights.size() / 2;
      std::nth_element(weights.begin(), median_it, weights.end());
      if (weights.size() % 2 == 1) {
        default_weight = *median_it;
      } else {
        // If the number of weights is even, then the median is the average of the two middle
        // elements.
        const auto lower_median_it = std::max_element(weights.begin(), median_it);
        // Use uint64_t to avoid potential overflow of the weights sum.
        default_weight = static_cast<uint32_t>(
            (static_cast<uint64_t>(*lower_median_it) + static_cast<uint64_t>(*median_it)) / 2);
      }
    }
    // Update the hosts with default weight.
    for (const auto& host_ptr : hosts_with_default_weight) {
      if (default_weight != host_ptr->weight()) {
        host_ptr->weight(default_weight);
        ENVOY_LOG(trace, "updateWeights default hostWeight {} = {}", getHostAddress(host_ptr.get()),
                  host_ptr->weight());
        weights_updated = true;
      }
    }
  }
  return weights_updated;
}

void OrcaWeightManager::addLbPolicyDataToHosts(const Upstream::HostVector& hosts) {
  for (const auto& host_ptr : hosts) {
    if (!host_ptr->typedLbPolicyData<OrcaHostLbPolicyData>().has_value()) {
      ENVOY_LOG(trace, "Adding LB policy data to Host {}", getHostAddress(host_ptr.get()));
      host_ptr->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(report_handler_));
    }
  }
}

absl::optional<uint32_t>
OrcaWeightManager::getWeightIfValidFromHost(const Upstream::Host& host,
                                            MonotonicTime max_non_empty_since,
                                            MonotonicTime min_last_update_time) {
  auto client_side_data = host.typedLbPolicyData<OrcaHostLbPolicyData>();
  if (!client_side_data.has_value()) {
    ENVOY_LOG_MISC(trace, "Host does not have OrcaHostLbPolicyData {}", getHostAddress(&host));
    return absl::nullopt;
  }
  return client_side_data->getWeightIfValid(max_non_empty_since, min_last_update_time);
}

// ============================================================
// OOB session management
// ============================================================

void OrcaWeightManager::onHostsAdded(const Upstream::HostVector& hosts) {
  if (!oob_enabled_) {
    return;
  }
  for (const auto& host_ptr : hosts) {
    schedulePendingOobSession(host_ptr);
  }
}

void OrcaWeightManager::onHostsRemoved(const Upstream::HostVector& hosts) {
  if (!oob_enabled_) {
    return;
  }
  for (const auto& host_ptr : hosts) {
    Upstream::HostConstSharedPtr key = host_ptr;
    // Always evict any pending stagger entry: this both cancels a pre-fire
    // timer AND clears the post-fire marker we leave behind to prevent
    // self-destructing-from-callback UB.
    if (pending_oob_session_timers_.erase(key) > 0) {
      ENVOY_LOG(debug, "Cancelled / cleaned up OOB session timer for {}",
                getHostAddress(host_ptr.get()));
    }
    auto it = oob_sessions_.find(key);
    if (it != oob_sessions_.end()) {
      ENVOY_LOG(debug, "Closing active OOB session for {}", getHostAddress(host_ptr.get()));
      Envoy::Orca::OrcaOobSessionPtr session = std::move(it->second);
      oob_sessions_.erase(it);
      oob_stats_.active_sessions_.set(oob_sessions_.size());
      if (session != nullptr) {
        session->close();
        dispatcher_.deferredDelete(std::move(session));
      }
    }
  }
}

void OrcaWeightManager::schedulePendingOobSession(const Upstream::HostSharedPtr& host) {
  Upstream::HostConstSharedPtr key = host;
  // If we already have a pending start or active session for this host, do
  // nothing — schedulers must be idempotent against duplicate add notifications
  // (e.g. priority moves).
  if (pending_oob_session_timers_.contains(key) || oob_sessions_.contains(key)) {
    return;
  }

  // Capture the host as a shared_ptr so it lives at least until the timer
  // fires. We deliberately avoid capturing `this` only with the host weak_ptr
  // because the timer is owned by the manager; if the manager dies, the
  // TimerPtr is destroyed before the callback can fire.
  //
  // We deliberately do NOT erase the timer from pending_oob_session_timers_
  // from inside its own callback: doing so would destroy the TimerPtr we are
  // executing under (UB in MockTimer, fragile in prod). Instead, leave the
  // entry in the map as a "fired but not yet swept" marker and let
  // startOobSession transfer the host into oob_sessions_. The host removal
  // path handles both pending and active maps. The timer object itself is
  // one-shot at this point and lives harmlessly in the map until it is
  // either rescheduled (we never do) or evicted on host removal / manager
  // destruction.
  Event::TimerPtr timer = dispatcher_.createTimer([this, host]() -> void {
    if (!pending_oob_session_timers_.contains(host)) {
      // Host was removed (and entry erased) between scheduling and firing.
      return;
    }
    startOobSession(host);
  });

  const std::chrono::milliseconds delay = computeStaggerDelay();
  timer->enableTimer(delay);
  pending_oob_session_timers_.emplace(std::move(key), std::move(timer));
  ENVOY_LOG(debug, "Scheduled OOB session start for {} in {}ms", getHostAddress(host.get()),
            delay.count());
}

std::chrono::milliseconds OrcaWeightManager::computeStaggerDelay() {
  // Spread initial sessions uniformly across the first reporting period, so a
  // freshly-loaded cluster doesn't reach for every upstream connection at the
  // same instant. This mirrors the initial-jitter strategy used by the active
  // health checkers.
  const uint64_t period_ms = static_cast<uint64_t>(oob_reporting_period_.count());
  if (period_ms == 0) {
    return std::chrono::milliseconds(0);
  }
  return std::chrono::milliseconds(random_.random() % period_ms);
}

void OrcaWeightManager::startOobSession(const Upstream::HostSharedPtr& host) {
  if (!oob_enabled_) {
    return;
  }
  ENVOY_LOG(debug, "Starting OOB session for {}", getHostAddress(host.get()));

  Upstream::HostConstSharedPtr key = host;
  // Defensive: if the same host already has an active session, do nothing
  // (e.g. a duplicate add raced through). The pending timer entry is left in
  // place and will be cleaned up on host removal or manager destruction.
  if (oob_sessions_.contains(key)) {
    return;
  }

  // Per-attempt codec client factory: this lambda is invoked by OrcaOobSession
  // every time it needs to (re)open a connection. CRITICAL: do not call this
  // from outside that context; it constructs a CodecClientProd which connects
  // synchronously.
  auto create_codec_client_cb = [this, host]() -> Http::CodecClientPtr {
    Upstream::Host::CreateConnectionData connection_data = host->createOrcaReportingConnection(
        dispatcher_, transport_socket_options_, host->metadata().get());
    return codec_client_factory_->create(std::move(connection_data), dispatcher_);
  };

  // We deliberately use HostConstSharedPtr keys (not raw pointers) so that the
  // session callbacks below remain tied to the same host identity even if the
  // priority set replaces the HostSharedPtr.
  auto on_report_cb = [this, key](const xds::data::orca::v3::OrcaLoadReport& report) {
    oob_stats_.reports_received_.inc();
    auto data_opt = key->typedLbPolicyData<OrcaHostLbPolicyData>();
    if (!data_opt.has_value()) {
      // Host has no LB policy data attached (shouldn't happen post-initialize);
      // still count the report as an error so it's visible.
      oob_stats_.report_errors_.inc();
      ENVOY_LOG(warn, "OOB report received for host without OrcaHostLbPolicyData {}",
                getHostAddress(key.get()));
      return;
    }
    const absl::Status status =
        report_handler_->updateClientSideDataFromOrcaLoadReport(report, *data_opt);
    if (!status.ok()) {
      oob_stats_.report_errors_.inc();
      ENVOY_LOG(debug, "OOB report failed to update host {}: {}", getHostAddress(key.get()),
                status.message());
    }
  };

  auto on_lifecycle_cb = [this](Envoy::Orca::OrcaOobLifecycleEvent event) {
    switch (event) {
    case Envoy::Orca::OrcaOobLifecycleEvent::StreamOpen:
      oob_stats_.stream_opens_.inc();
      break;
    case Envoy::Orca::OrcaOobLifecycleEvent::StreamFailure:
      oob_stats_.stream_failures_.inc();
      break;
    }
  };

  // Terminal callback: drop the session from oob_sessions_ and deferred-delete
  // it. It is safe to do work in this frame (the session promises not to
  // touch its own members after firing this callback), but we MUST NOT
  // destroy the session synchronously here — defer it through the dispatcher.
  auto on_terminated_cb = [this, key](Grpc::Status::GrpcStatus status, absl::string_view message) {
    oob_stats_.stream_terminated_.inc();
    auto it = oob_sessions_.find(key);
    if (it == oob_sessions_.end()) {
      // Session was already removed (e.g. host removal raced with terminal
      // callback). Nothing more to do.
      return;
    }
    ENVOY_LOG(debug, "OOB session terminated for {}: status={} message={}",
              getHostAddress(key.get()), status, message);
    Envoy::Orca::OrcaOobSessionPtr session = std::move(it->second);
    oob_sessions_.erase(it);
    oob_stats_.active_sessions_.set(oob_sessions_.size());
    if (session != nullptr) {
      dispatcher_.deferredDelete(std::move(session));
    }
  };

  // Backoff strategy used by the session for retries on transient failure.
  // Each session gets its own strategy instance because BackOffStrategy is
  // stateful.
  auto backoff = std::make_unique<JitteredExponentialBackOffStrategy>(
      kOobBackoffBaseIntervalMs, kOobBackoffMaxIntervalMs, random_);

  auto session = std::make_unique<Envoy::Orca::OrcaOobSession>(
      std::move(create_codec_client_cb), dispatcher_, oob_reporting_period_,
      oob_request_cost_names_, std::move(on_report_cb), std::move(on_terminated_cb),
      std::move(on_lifecycle_cb), std::move(backoff));

  // Insert before start() so that re-entrant terminal callbacks (e.g. if a
  // factory throws synchronously) can still find the session in the map.
  Envoy::Orca::OrcaOobSession* session_raw = session.get();
  oob_sessions_.emplace(key, std::move(session));
  oob_stats_.active_sessions_.set(oob_sessions_.size());
  session_raw->start();
}

} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
