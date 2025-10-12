#include "source/extensions/load_balancing_policies/wrr_locality/wrr_locality_lb.h"

#include <algorithm>
#include <optional>

#include "envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3/client_side_weighted_round_robin.pb.h"

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/orca/orca_load_metrics.h"
#include "source/common/protobuf/utility.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {

namespace {

constexpr absl::string_view kClientSideWrrPolicyName =
    "envoy.load_balancing_policies.client_side_weighted_round_robin";

envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
    ClientSideWeightedRoundRobin
unwrapClientSideConfig(const envoy::config::cluster::v3::LoadBalancingPolicy& policy) {
  for (const auto& typed_extension : policy.policies()) {
    if (typed_extension.typed_extension_config().name() == kClientSideWrrPolicyName) {
      envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
          ClientSideWeightedRoundRobin child_config;
      const auto& any = typed_extension.typed_extension_config().typed_config();
      absl::Status status = MessageUtil::unpackTo(any, child_config);
      RELEASE_ASSERT(status.ok(), "Failed to unpack ClientSideWeightedRoundRobin config");
      return child_config;
    }
  }
  // This should only be reached if loadConfig() validation failed to catch missing policy.
  // WrrLocality requires ClientSideWeightedRoundRobin as the endpoint picking policy
  // to extract ORCA weight calculation parameters (blackout_period, weight_expiration_period, etc.)
  RELEASE_ASSERT(false, "WrrLocality requires ClientSideWeightedRoundRobin endpoint_picking_policy");
}

} // namespace

WrrLocalityLbConfig::WrrLocalityLbConfig(
    const envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality& lb_config,
    Server::Configuration::ServerFactoryContext& context)
    : main_thread_dispatcher_(context.mainThreadDispatcher()),
      tls_slot_allocator_(context.threadLocal()) {
  if (lb_config.has_locality_lb_config()) {
    *round_robin_config_.mutable_locality_lb_config() = lb_config.locality_lb_config();
  }
  if (lb_config.has_slow_start_config()) {
    *round_robin_config_.mutable_slow_start_config() = lb_config.slow_start_config();
  }

  const envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin endpoint_config =
          unwrapClientSideConfig(lb_config.endpoint_picking_policy());

  // Copy metric names from the endpoint config
  const auto& metric_names = endpoint_config.metric_names_for_computing_utilization();
  metric_names_for_computing_utilization_.assign(metric_names.begin(), metric_names.end());

  if (endpoint_config.has_error_utilization_penalty()) {
    error_utilization_penalty_ = endpoint_config.error_utilization_penalty().value();
  }

  blackout_period_ = std::chrono::milliseconds(
      PROTOBUF_GET_MS_OR_DEFAULT(endpoint_config, blackout_period, kDefaultBlackoutPeriod.count()));
  weight_expiration_period_ = std::chrono::milliseconds(
      PROTOBUF_GET_MS_OR_DEFAULT(endpoint_config, weight_expiration_period,
                                 kDefaultWeightExpirationPeriod.count()));
  weight_update_period_ = std::chrono::milliseconds(
      PROTOBUF_GET_MS_OR_DEFAULT(endpoint_config, weight_update_period,
                                 kDefaultWeightUpdatePeriod.count()));
}

absl::Status WrrLocalityLoadBalancer::HostLbPolicyData::onOrcaLoadReport(
    const Upstream::OrcaLoadReport& report, const StreamInfo::StreamInfo& /*stream_info*/) {
  ASSERT(report_handler_ != nullptr);
  return report_handler_->updateHostDataFromOrcaLoadReport(report, *this);
}

absl::Status WrrLocalityLoadBalancer::OrcaLoadReportHandler::updateHostDataFromOrcaLoadReport(
    const OrcaLoadReportProto& report, HostLbPolicyData& host_data) {
  absl::StatusOr<uint32_t> weight =
      calculateWeightFromOrcaReport(report, metric_names_for_computing_utilization_,
                                    error_utilization_penalty_);
  if (!weight.ok()) {
    return weight.status();
  }
  host_data.updateWeightNow(weight.value(), time_source_.monotonicTime());
  return absl::OkStatus();
}

double WrrLocalityLoadBalancer::OrcaLoadReportHandler::getUtilizationFromOrcaReport(
    const OrcaLoadReportProto& report,
    const std::vector<std::string>& metric_names_for_computing_utilization) {
  double utilization = report.application_utilization();
  if (utilization > 0) {
    return utilization;
  }
  utilization =
      Envoy::Orca::getMaxUtilization(metric_names_for_computing_utilization, report);
  if (utilization > 0) {
    return utilization;
  }
  return report.cpu_utilization();
}

absl::StatusOr<uint32_t> WrrLocalityLoadBalancer::OrcaLoadReportHandler::calculateWeightFromOrcaReport(
    const OrcaLoadReportProto& report,
    const std::vector<std::string>& metric_names_for_computing_utilization,
    double error_utilization_penalty) {
  const double qps = report.rps_fractional();
  if (qps <= 0) {
    return absl::InvalidArgumentError("QPS must be positive");
  }

  double utilization =
      getUtilizationFromOrcaReport(report, metric_names_for_computing_utilization);

  // Add error penalty contribution (validated to avoid division by zero above)
  utilization += error_utilization_penalty * report.eps() / qps;

  if (utilization <= 0) {
    return absl::InvalidArgumentError("Utilization must be positive");
  }

  // Calculate weight with overflow protection
  const double weight = qps / utilization;
  if (weight > std::numeric_limits<uint32_t>::max()) {
    return std::numeric_limits<uint32_t>::max();
  }
  if (weight < static_cast<double>(kMinimumHostWeight)) {
    return kMinimumHostWeight;
  }
  return static_cast<uint32_t>(weight);
}

WrrLocalityLoadBalancer::WorkerLocalLb::WorkerLocalLb(
    const WrrLocalityLbConfig& config, const Upstream::PrioritySet& priority_set,
    const Upstream::PrioritySet* local_priority_set, Upstream::ClusterLbStats& stats,
    Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source,
    OptRef<ThreadLocalShim> tls_shim, uint32_t healthy_panic_threshold)
    : RoundRobinLoadBalancer(priority_set, local_priority_set, stats, runtime, random,
                             healthy_panic_threshold, config.roundRobinConfig(), time_source) {
  if (tls_shim.has_value()) {
    auto& helper = tls_shim->refresh_cb_helper_;
    refresh_cb_handle_ = helper.add([this]() -> void { refreshAll(); });
  }
}

void WrrLocalityLoadBalancer::WorkerLocalLb::refreshAll() {
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    if (host_set != nullptr) {
      refresh(host_set->priority());
    }
  }
}

WrrLocalityLoadBalancer::WorkerLocalLbFactory::WorkerLocalLbFactory(
    const WrrLocalityLbConfig& config, const Upstream::ClusterInfo& cluster_info,
    Runtime::Loader& runtime, Random::RandomGenerator& random, TimeSource& time_source,
    ThreadLocal::SlotAllocator& tls)
    : config_(config), cluster_info_(cluster_info), runtime_(runtime),
      random_(random), time_source_(time_source) {
  tls_slot_ = ThreadLocal::TypedSlot<ThreadLocalShim>::makeUnique(tls);
  tls_slot_->set([](Event::Dispatcher&) { return std::make_shared<ThreadLocalShim>(); });
}

Upstream::LoadBalancerPtr
WrrLocalityLoadBalancer::WorkerLocalLbFactory::create(Upstream::LoadBalancerParams params) {
  uint32_t healthy_panic_threshold = PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
      cluster_info_.lbConfig(), healthy_panic_threshold, 100, 50);
  return std::make_unique<WorkerLocalLb>(config_, params.priority_set, params.local_priority_set,
                                         cluster_info_.lbStats(), runtime_, random_, time_source_,
                                         tls_slot_->get(), healthy_panic_threshold);
}

void WrrLocalityLoadBalancer::WorkerLocalLbFactory::refreshAllWorkers() {
  tls_slot_->runOnAllThreads([](OptRef<ThreadLocalShim> shim) {
    if (shim.has_value()) {
      shim->refresh_cb_helper_.runCallbacks();
    }
  });
}

WrrLocalityLoadBalancer::WrrLocalityLoadBalancer(
    OptRef<const Upstream::LoadBalancerConfig> lb_config, const Upstream::ClusterInfo& cluster_info,
    const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
    Random::RandomGenerator& random, TimeSource& time_source)
    : priority_set_(priority_set), time_source_(time_source) {

  const auto* typed_config = dynamic_cast<const WrrLocalityLbConfig*>(lb_config.ptr());
  RELEASE_ASSERT(typed_config != nullptr, "Invalid config type for WrrLocalityLoadBalancer");

  orca_report_handler_ =
      std::make_shared<OrcaLoadReportHandler>(*typed_config, time_source_);
  factory_ = std::make_shared<WorkerLocalLbFactory>(*typed_config, cluster_info, runtime, random,
                                                    time_source, typed_config->tlsSlotAllocator());
  initFromConfig(*typed_config);

  weight_update_timer_ =
      typed_config->mainThreadDispatcher().createTimer([this]() -> void {
        updateWeightsOnMainThread();
        weight_update_timer_->enableTimer(weight_update_period_);
      });
}

absl::Status WrrLocalityLoadBalancer::initialize() {
  for (const Upstream::HostSetPtr& host_set : priority_set_.hostSetsPerPriority()) {
    addHostLbPolicyData(host_set->hosts());
  }

  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t, const Upstream::HostVector& hosts_added, const Upstream::HostVector&) {
        // Add LB policy data to newly added hosts.
        // Weight updates are handled by the periodic timer (debouncing optimization).
        addHostLbPolicyData(hosts_added);
      });

  weight_update_timer_->enableTimer(weight_update_period_);
  return absl::OkStatus();
}

void WrrLocalityLoadBalancer::initFromConfig(const WrrLocalityLbConfig& config) {
  blackout_period_ = config.blackoutPeriod();
  weight_expiration_period_ = config.weightExpirationPeriod();
  weight_update_period_ = config.weightUpdatePeriod();
}

void WrrLocalityLoadBalancer::addHostLbPolicyData(const Upstream::HostVector& hosts) {
  for (const auto& host : hosts) {
    if (!host->lbPolicyData().has_value()) {
      host->setLbPolicyData(std::make_unique<HostLbPolicyData>(orca_report_handler_));
    }
  }
}

void WrrLocalityLoadBalancer::updateWeightsOnMainThread() {
  bool changed = false;
  for (const Upstream::HostSetPtr& host_set : priority_set_.hostSetsPerPriority()) {
    changed = updateWeightsOnHosts(host_set->hosts()) || changed;
  }
  if (changed) {
    factory_->refreshAllWorkers();
  }
}

bool WrrLocalityLoadBalancer::updateWeightsOnHosts(const Upstream::HostVector& hosts) {
  std::vector<uint32_t> weights;
  Upstream::HostVector hosts_with_default_weight;
  bool weights_updated = false;

  const MonotonicTime now = time_source_.monotonicTime();
  const MonotonicTime max_non_empty_since = now - blackout_period_;
  const MonotonicTime min_last_update_time = now - weight_expiration_period_;

  weights.reserve(hosts.size());
  hosts_with_default_weight.reserve(hosts.size());

  for (const auto& host : hosts) {
    absl::optional<uint32_t> weight =
        getWeightIfValidFromHost(*host, max_non_empty_since, min_last_update_time);
    if (weight.has_value()) {
      const uint32_t new_weight = weight.value();
      weights.push_back(new_weight);
      if (new_weight != host->weight()) {
        host->weight(new_weight);
        weights_updated = true;
      }
    } else {
      hosts_with_default_weight.push_back(host);
    }
  }

  if (!hosts_with_default_weight.empty()) {
    uint32_t default_weight = 1;
    if (!weights.empty()) {
      // Use average of valid weights as the default for hosts without ORCA reports.
      // Average is simpler and more predictable than median for weight assignment.
      // Note: sum uses uint64_t to avoid overflow. In practice, overflow would require
      // billions of hosts with maximum weights, which is beyond any realistic deployment.
      uint64_t sum = 0;
      for (uint32_t w : weights) {
        sum += w;
      }
      default_weight = static_cast<uint32_t>(sum / weights.size());
      // Ensure minimum weight is 1
      if (default_weight == 0) {
        default_weight = 1;
      }
    }
    for (const auto& host : hosts_with_default_weight) {
      if (host->weight() != default_weight) {
        host->weight(default_weight);
        weights_updated = true;
      }
    }
  }

  return weights_updated;
}

absl::optional<uint32_t> WrrLocalityLoadBalancer::getWeightIfValidFromHost(
    const Upstream::Host& host, MonotonicTime max_non_empty_since,
    MonotonicTime min_last_update_time) {
  auto policy_data = host.typedLbPolicyData<HostLbPolicyData>();
  if (!policy_data.has_value()) {
    return absl::nullopt;
  }
  return policy_data->getWeightIfValid(max_non_empty_since, min_last_update_time);
}

} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
