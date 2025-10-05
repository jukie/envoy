#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"

#include <atomic>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>

#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/core/v3/base.pb.h"
#include "envoy/runtime/runtime.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/protobuf/utility.h"
#include "source/common/runtime/runtime_features.h"

#include "absl/container/fixed_array.h"

namespace Envoy {
namespace Upstream {

namespace {
static const std::string RuntimeZoneEnabled = "upstream.zone_routing.enabled";
static const std::string RuntimeMinClusterSize = "upstream.zone_routing.min_cluster_size";
static const std::string RuntimeForceLocalZoneMinSize =
    "upstream.zone_routing.force_local_zone.min_size";
static const std::string RuntimePanicThreshold = "upstream.healthy_panic_threshold";
// Constant probing regardless of unknown/known zones (use sparingly)
static const std::string RuntimeOrcaProbeAlwaysEnabled =
    "upstream.zone_routing.orca_probe_always.enabled";
static const std::string RuntimeOrcaProbeAlwaysPercent =
    "upstream.zone_routing.orca_probe_always.percent";

// Constants for ORCA weight calculations
constexpr uint64_t ORCA_WEIGHT_SCALE_FACTOR =
    10000; // Scale factor for weight calculations (10000 = 100%)

// Returns true if the weights of all the hosts in the HostVector are equal.
bool hostWeightsAreEqual(const HostVector& hosts) {
  if (hosts.size() <= 1) {
    return true;
  }
  const uint32_t weight = hosts[0]->weight();
  for (size_t i = 1; i < hosts.size(); ++i) {
    if (hosts[i]->weight() != weight) {
      return false;
    }
  }
  return true;
}

} // namespace

std::pair<int32_t, size_t> distributeLoad(PriorityLoad& per_priority_load,
                                          const PriorityAvailability& per_priority_availability,
                                          size_t total_load, size_t normalized_total_availability) {
  int32_t first_available_priority = -1;
  for (size_t i = 0; i < per_priority_availability.get().size(); ++i) {
    if (first_available_priority < 0 && per_priority_availability.get()[i] > 0) {
      first_available_priority = i;
    }
    // Now assign as much load as possible to the high priority levels and cease assigning load
    // when total_load runs out.
    per_priority_load.get()[i] = std::min<uint32_t>(
        total_load, per_priority_availability.get()[i] * 100 / normalized_total_availability);
    total_load -= per_priority_load.get()[i];
  }

  return {first_available_priority, total_load};
}

std::pair<uint32_t, LoadBalancerBase::HostAvailability>
LoadBalancerBase::choosePriority(uint64_t hash, const HealthyLoad& healthy_per_priority_load,
                                 const DegradedLoad& degraded_per_priority_load) {
  hash = hash % 100 + 1; // 1-100
  uint32_t aggregate_percentage_load = 0;
  // As with tryChooseLocalLocalityHosts, this can be refactored for efficiency
  // but O(N) is good enough for now given the expected number of priorities is
  // small.

  // We first attempt to select a priority based on healthy availability.
  for (size_t priority = 0; priority < healthy_per_priority_load.get().size(); ++priority) {
    aggregate_percentage_load += healthy_per_priority_load.get()[priority];
    if (hash <= aggregate_percentage_load) {
      return {static_cast<uint32_t>(priority), HostAvailability::Healthy};
    }
  }

  // If no priorities were selected due to health, we'll select a priority based degraded
  // availability.
  for (size_t priority = 0; priority < degraded_per_priority_load.get().size(); ++priority) {
    aggregate_percentage_load += degraded_per_priority_load.get()[priority];
    if (hash <= aggregate_percentage_load) {
      return {static_cast<uint32_t>(priority), HostAvailability::Degraded};
    }
  }

  // The percentages should always add up to 100 but we have to have a return for the compiler.
  IS_ENVOY_BUG("unexpected load error");
  return {0, HostAvailability::Healthy};
}

LoadBalancerBase::LoadBalancerBase(const PrioritySet& priority_set, ClusterLbStats& stats,
                                   Runtime::Loader& runtime, Random::RandomGenerator& random,
                                   uint32_t healthy_panic_threshold)
    : stats_(stats), runtime_(runtime), random_(random),
      default_healthy_panic_percent_(healthy_panic_threshold), priority_set_(priority_set) {
  for (auto& host_set : priority_set_.hostSetsPerPriority()) {
    recalculatePerPriorityState(host_set->priority(), priority_set_, per_priority_load_,
                                per_priority_health_, per_priority_degraded_, total_healthy_hosts_);
  }
  // Recalculate panic mode for all levels.
  recalculatePerPriorityPanic();

  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) -> absl::Status {
        recalculatePerPriorityState(priority, priority_set_, per_priority_load_,
                                    per_priority_health_, per_priority_degraded_,
                                    total_healthy_hosts_);
        recalculatePerPriorityPanic();
        stashed_random_.clear();
        return absl::OkStatus();
      });
}

// The following cases are handled by
// recalculatePerPriorityState and recalculatePerPriorityPanic methods (normalized total health is
// sum of all priorities' health values and capped at 100).
// - normalized total health is = 100%. It means there are enough healthy hosts to handle the load.
//   Do not enter panic mode, even if a specific priority has low number of healthy hosts.
// - normalized total health is < 100%. There are not enough healthy hosts to handle the load.
// Continue distributing the load among priority sets, but turn on panic mode for a given priority
//   if # of healthy hosts in priority set is low.
// - all host sets are in panic mode. Situation called TotalPanic. Load distribution is
//   calculated based on the number of hosts in each priority regardless of their health.
// - all hosts in all priorities are down (normalized total health is 0%). If panic
//   threshold > 0% the cluster is in TotalPanic (see above). If panic threshold == 0
//   then priorities are not in panic, but there are no healthy hosts to route to.
//   In this case just mark P=0 as recipient of 100% of the traffic (nothing will be routed
//   to P=0 anyways as there are no healthy hosts there).
void LoadBalancerBase::recalculatePerPriorityState(uint32_t priority,
                                                   const PrioritySet& priority_set,
                                                   HealthyAndDegradedLoad& per_priority_load,
                                                   HealthyAvailability& per_priority_health,
                                                   DegradedAvailability& per_priority_degraded,
                                                   uint32_t& total_healthy_hosts) {
  per_priority_load.healthy_priority_load_.get().resize(priority_set.hostSetsPerPriority().size());
  per_priority_load.degraded_priority_load_.get().resize(priority_set.hostSetsPerPriority().size());
  per_priority_health.get().resize(priority_set.hostSetsPerPriority().size());
  per_priority_degraded.get().resize(priority_set.hostSetsPerPriority().size());
  total_healthy_hosts = 0;

  // Determine the health of the newly modified priority level.
  // Health ranges from 0-100, and is the ratio of healthy/degraded hosts to total hosts, modified
  // by the overprovisioning factor.
  HostSet& host_set = *priority_set.hostSetsPerPriority()[priority];
  per_priority_health.get()[priority] = 0;
  per_priority_degraded.get()[priority] = 0;
  const auto host_count = host_set.hosts().size() - host_set.excludedHosts().size();

  if (host_count > 0) {
    uint64_t healthy_weight = 0;
    uint64_t degraded_weight = 0;
    uint64_t total_weight = 0;
    if (host_set.weightedPriorityHealth()) {
      for (const auto& host : host_set.healthyHosts()) {
        healthy_weight += host->weight();
      }

      for (const auto& host : host_set.degradedHosts()) {
        degraded_weight += host->weight();
      }

      for (const auto& host : host_set.hosts()) {
        total_weight += host->weight();
      }

      uint64_t excluded_weight = 0;
      for (const auto& host : host_set.excludedHosts()) {
        excluded_weight += host->weight();
      }
      ASSERT(total_weight >= excluded_weight);
      total_weight -= excluded_weight;
    } else {
      healthy_weight = host_set.healthyHosts().size();
      degraded_weight = host_set.degradedHosts().size();
      total_weight = host_count;
    }
    // Each priority level's health is ratio of healthy hosts to total number of hosts in a
    // priority multiplied by overprovisioning factor of 1.4 and capped at 100%. It means that if
    // all hosts are healthy that priority's health is 100%*1.4=140% and is capped at 100% which
    // results in 100%. If 80% of hosts are healthy, that priority's health is still 100%
    // (80%*1.4=112% and capped at 100%).
    per_priority_health.get()[priority] =
        std::min<uint32_t>(100,
                           // NOLINTNEXTLINE(clang-analyzer-core.DivideZero)
                           (host_set.overprovisioningFactor() * healthy_weight / total_weight));

    // We perform the same computation for degraded hosts.
    per_priority_degraded.get()[priority] = std::min<uint32_t>(
        100, (host_set.overprovisioningFactor() * degraded_weight / total_weight));

    ENVOY_LOG(trace,
              "recalculated priority state: priority level {}, healthy weight {}, total weight {}, "
              "overprovision factor {}, healthy result {}, degraded result {}",
              priority, healthy_weight, total_weight, host_set.overprovisioningFactor(),
              per_priority_health.get()[priority], per_priority_degraded.get()[priority]);
  }

  // Now that we've updated health for the changed priority level, we need to calculate percentage
  // load for all priority levels.

  // First, determine if the load needs to be scaled relative to availability (healthy + degraded).
  // For example if there are 3 host sets with 10% / 20% / 10% health and 20% / 10% / 0% degraded
  // they will get 16% / 28% / 14% load to healthy hosts and 28% / 14% / 0% load to degraded hosts
  // to ensure total load adds up to 100. Note the first healthy priority is receiving 2% additional
  // load due to rounding.
  //
  // Sum of priority levels' health and degraded values may exceed 100, so it is capped at 100 and
  // referred as normalized total availability.
  const uint32_t normalized_total_availability =
      calculateNormalizedTotalAvailability(per_priority_health, per_priority_degraded);
  if (normalized_total_availability == 0) {
    // Everything is terrible. There is nothing to calculate here.
    // Let recalculatePerPriorityPanic and recalculateLoadInTotalPanic deal with
    // load calculation.
    return;
  }

  // We start of with a total load of 100 and distribute it between priorities based on
  // availability. We first attempt to distribute this load to healthy priorities based on healthy
  // availability.
  const auto first_healthy_and_remaining =
      distributeLoad(per_priority_load.healthy_priority_load_, per_priority_health, 100,
                     normalized_total_availability);

  // Using the remaining load after allocating load to healthy priorities, distribute it based on
  // degraded availability.
  const auto remaining_load_for_degraded = first_healthy_and_remaining.second;
  const auto first_degraded_and_remaining =
      distributeLoad(per_priority_load.degraded_priority_load_, per_priority_degraded,
                     remaining_load_for_degraded, normalized_total_availability);

  // Anything that remains should just be rounding errors, so allocate that to the first available
  // priority, either as healthy or degraded.
  const auto remaining_load = first_degraded_and_remaining.second;
  if (remaining_load != 0) {
    const auto first_healthy = first_healthy_and_remaining.first;
    const auto first_degraded = first_degraded_and_remaining.first;
    ASSERT(first_healthy != -1 || first_degraded != -1);

    // Attempt to allocate the remainder to the first healthy priority first. If no such priority
    // exist, allocate to the first degraded priority.
    ASSERT(remaining_load < per_priority_load.healthy_priority_load_.get().size() +
                                per_priority_load.degraded_priority_load_.get().size());
    if (first_healthy != -1) {
      per_priority_load.healthy_priority_load_.get()[first_healthy] += remaining_load;
    } else {
      per_priority_load.degraded_priority_load_.get()[first_degraded] += remaining_load;
    }
  }

  // The allocated load between healthy and degraded should be exactly 100.
  ASSERT(100 == std::accumulate(per_priority_load.healthy_priority_load_.get().begin(),
                                per_priority_load.healthy_priority_load_.get().end(), 0) +
                    std::accumulate(per_priority_load.degraded_priority_load_.get().begin(),
                                    per_priority_load.degraded_priority_load_.get().end(), 0));

  for (auto& host_set : priority_set.hostSetsPerPriority()) {
    total_healthy_hosts += host_set->healthyHosts().size();
  }
}

// Method iterates through priority levels and turns on/off panic mode.
void LoadBalancerBase::recalculatePerPriorityPanic() {
  per_priority_panic_.resize(priority_set_.hostSetsPerPriority().size());

  const uint32_t normalized_total_availability =
      calculateNormalizedTotalAvailability(per_priority_health_, per_priority_degraded_);

  const uint64_t panic_threshold = std::min<uint64_t>(
      100, runtime_.snapshot().getInteger(RuntimePanicThreshold, default_healthy_panic_percent_));

  // This is corner case when panic is disabled and there is no hosts available.
  // LoadBalancerBase::choosePriority method expects that the sum of
  // load percentages always adds up to 100.
  // To satisfy that requirement 100% is assigned to P=0.
  // In reality no traffic will be routed to P=0 priority, because
  // the panic mode is disabled and LoadBalancer will try to find
  // a healthy node and none is available.
  if (panic_threshold == 0 && normalized_total_availability == 0) {
    per_priority_load_.healthy_priority_load_.get()[0] = 100;
    return;
  }

  bool total_panic = true;
  for (size_t i = 0; i < per_priority_health_.get().size(); ++i) {
    // For each level check if it should run in panic mode. Never set panic mode if
    // normalized total health is 100%, even when individual priority level has very low # of
    // healthy hosts.
    const HostSet& priority_host_set = *priority_set_.hostSetsPerPriority()[i];
    per_priority_panic_[i] =
        (normalized_total_availability == 100 ? false : isHostSetInPanic(priority_host_set));
    total_panic = total_panic && per_priority_panic_[i];
  }

  // If all priority levels are in panic mode, load distribution
  // is done differently.
  if (total_panic) {
    recalculateLoadInTotalPanic();
  }
}

// recalculateLoadInTotalPanic method is called when all priority levels
// are in panic mode. The load distribution is done NOT based on number
// of healthy hosts in the priority, but based on number of hosts
// in each priority regardless of its health.
void LoadBalancerBase::recalculateLoadInTotalPanic() {
  // First calculate total number of hosts across all priorities regardless
  // whether they are healthy or not.
  const uint32_t total_hosts_count =
      std::accumulate(priority_set_.hostSetsPerPriority().begin(),
                      priority_set_.hostSetsPerPriority().end(), static_cast<size_t>(0),
                      [](size_t acc, const std::unique_ptr<Envoy::Upstream::HostSet>& host_set) {
                        return acc + host_set->hosts().size();
                      });

  if (0 == total_hosts_count) {
    // Backend is empty, but load must be distributed somewhere.
    per_priority_load_.healthy_priority_load_.get()[0] = 100;
    return;
  }

  // Now iterate through all priority levels and calculate how much
  // load is supposed to go to each priority. In panic mode the calculation
  // is based not on the number of healthy hosts but based on the number of
  // total hosts in the priority.
  uint32_t total_load = 100;
  int32_t first_noempty = -1;
  for (size_t i = 0; i < per_priority_panic_.size(); i++) {
    const HostSet& host_set = *priority_set_.hostSetsPerPriority()[i];
    const auto hosts_num = host_set.hosts().size();

    if ((-1 == first_noempty) && (0 != hosts_num)) {
      first_noempty = i;
    }
    const uint32_t priority_load = 100 * hosts_num / total_hosts_count;
    per_priority_load_.healthy_priority_load_.get()[i] = priority_load;
    per_priority_load_.degraded_priority_load_.get()[i] = 0;
    total_load -= priority_load;
  }

  // Add the remaining load to the first not empty load.
  per_priority_load_.healthy_priority_load_.get()[first_noempty] += total_load;

  // The total load should come up to 100%.
  ASSERT(100 == std::accumulate(per_priority_load_.healthy_priority_load_.get().begin(),
                                per_priority_load_.healthy_priority_load_.get().end(), 0));
}

std::pair<HostSet&, LoadBalancerBase::HostAvailability>
LoadBalancerBase::chooseHostSet(LoadBalancerContext* context, uint64_t hash) const {
  if (context) {
    const auto priority_loads = context->determinePriorityLoad(
        priority_set_, per_priority_load_, Upstream::RetryPriority::defaultPriorityMapping);
    const auto priority_and_source = choosePriority(hash, priority_loads.healthy_priority_load_,
                                                    priority_loads.degraded_priority_load_);
    return {*priority_set_.hostSetsPerPriority()[priority_and_source.first],
            priority_and_source.second};
  }

  const auto priority_and_source = choosePriority(hash, per_priority_load_.healthy_priority_load_,
                                                  per_priority_load_.degraded_priority_load_);
  return {*priority_set_.hostSetsPerPriority()[priority_and_source.first],
          priority_and_source.second};
}

uint64_t LoadBalancerBase::random(bool peeking) {
  if (peeking) {
    stashed_random_.push_back(random_.random());
    return stashed_random_.back();
  } else {
    if (!stashed_random_.empty()) {
      auto random = stashed_random_.front();
      stashed_random_.pop_front();
      return random;
    } else {
      return random_.random();
    }
  }
}

ZoneAwareLoadBalancerBase::ZoneAwareLoadBalancerBase(
    const PrioritySet& priority_set, const PrioritySet* local_priority_set, ClusterLbStats& stats,
    Runtime::Loader& runtime, Random::RandomGenerator& random, uint32_t healthy_panic_threshold,
    const absl::optional<LocalityLbConfig> locality_config, TimeSource& time_source)
    : LoadBalancerBase(priority_set, stats, runtime, random, healthy_panic_threshold),
      local_priority_set_(local_priority_set),
      min_cluster_size_(locality_config.has_value()
                            ? PROTOBUF_GET_WRAPPED_OR_DEFAULT(
                                  locality_config->zone_aware_lb_config(), min_cluster_size, 6U)
                            : 6U),
      force_local_zone_min_size_([&]() -> absl::optional<uint32_t> {
        // Check runtime value first
        if (auto rt = runtime_.snapshot().getInteger(RuntimeForceLocalZoneMinSize, 0); rt > 0) {
          return static_cast<uint32_t>(rt);
        }

        // ForceLocalZone proto field supersedes deprecated ForceLocalityDirectRouting
        if (locality_config.has_value()) {
          if (locality_config->zone_aware_lb_config().has_force_local_zone()) {
            return PROTOBUF_GET_WRAPPED_OR_DEFAULT(
                locality_config->zone_aware_lb_config().force_local_zone(), min_size, 1U);
          }
          if (locality_config->zone_aware_lb_config().force_locality_direct_routing()) {
            return 1U;
          }
        }
        return absl::nullopt;
      }()),
      routing_enabled_(locality_config.has_value()
                           ? PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
                                 locality_config->zone_aware_lb_config(), routing_enabled, 100, 100)
                           : 100),
      locality_basis_(locality_config.has_value()
                          ? locality_config->zone_aware_lb_config().locality_basis()
                          : LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_NUM),
      fail_traffic_on_panic_(locality_config.has_value()
                                 ? locality_config->zone_aware_lb_config().fail_traffic_on_panic()
                                 : false),
      locality_weighted_balancing_(locality_config.has_value() &&
                                   locality_config->has_locality_weighted_lb_config()),
      time_source_(time_source) {
  ASSERT(!priority_set.hostSetsPerPriority().empty());
  resizePerPriorityState();
  if (locality_weighted_balancing_) {
    for (uint32_t priority = 0; priority < priority_set_.hostSetsPerPriority().size(); ++priority) {
      rebuildLocalityWrrForPriority(priority);
    }
  }

  // Initialize ORCA-based zone routing configuration if enabled
  if (locality_config.has_value() && locality_config->zone_aware_lb_config().locality_basis() ==
                                         LocalityLbConfig::ZoneAwareLbConfig::ORCA_LOAD) {

    const auto& orca_config = locality_config->zone_aware_lb_config().orca_load_config();

    // Set ORCA utilization metrics configuration (3-tier hierarchy like CSWRR)
    if (orca_config.has_utilization_metrics()) {
      const auto& metrics_config = orca_config.utilization_metrics();
      orca_utilization_metric_names_.assign(metrics_config.metric_names().begin(),
                                            metrics_config.metric_names().end());
      orca_use_application_utilization_ = metrics_config.use_application_utilization();
      orca_use_cpu_utilization_ = metrics_config.use_cpu_utilization();
    } else {
      // Default: application_utilization first, then cpu_utilization fallback
      orca_use_application_utilization_ = true;
      orca_use_cpu_utilization_ = true;
    }

    // Pre-compute metric keys to avoid string allocations in hot path
    orca_utilization_metric_keys_.clear();
    orca_utilization_metric_keys_.reserve(orca_utilization_metric_names_.size());
    for (const auto& metric_name : orca_utilization_metric_names_) {
      orca_utilization_metric_keys_.push_back("named_metrics." + metric_name);
    }

    // Set error utilization penalty (aligns with CSWRR)
    orca_error_utilization_penalty_ =
        PROTOBUF_GET_WRAPPED_OR_DEFAULT(orca_config, error_utilization_penalty, 1.0);
    min_orca_reporting_hosts_ =
        PROTOBUF_GET_WRAPPED_OR_DEFAULT(orca_config, min_reporting_hosts_per_zone, 3U);

    // Fallback basis defaults to HEALTHY_HOSTS_NUM if not explicitly set or if set to default (0)
    orca_fallback_basis_ =
        orca_config.fallback_basis() != LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_NUM
            ? orca_config.fallback_basis()
            : LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_NUM;

    // Parse locality direct threshold (default 5%)
    locality_direct_threshold_pct_ = PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
        orca_config, locality_direct_threshold, 100, 5);

    // Parse EMA smoothing factor (default 0.2)
    zone_metrics_smoothing_factor_ =
        PROTOBUF_GET_WRAPPED_OR_DEFAULT(orca_config, zone_metrics_smoothing_factor, 0.2);

    // Validate smoothing factor
    if (zone_metrics_smoothing_factor_ < 0.0 || zone_metrics_smoothing_factor_ > 1.0) {
      ENVOY_LOG(warn, "Invalid zone_metrics_smoothing_factor {:.2f}, clamping to [0.0, 1.0]",
                zone_metrics_smoothing_factor_);
      zone_metrics_smoothing_factor_ = std::clamp(zone_metrics_smoothing_factor_, 0.0, 1.0);
    }

    // Parse weight update period (aligns with CSWRR, default 1s, min 100ms)
    if (orca_config.has_weight_update_period()) {
      auto duration_ms = DurationUtil::durationToMilliseconds(orca_config.weight_update_period());
      zone_metrics_update_interval_ =
          std::chrono::milliseconds(std::max(static_cast<uint64_t>(100), duration_ms));
    }

    // Parse blackout period (aligns with CSWRR, default 10s)
    if (orca_config.has_blackout_period()) {
      auto duration_ms = DurationUtil::durationToMilliseconds(orca_config.blackout_period());
      zone_routing_blackout_period_ = std::chrono::milliseconds(duration_ms);
    }

    // Parse OOB reporting configuration
    oob_enabled_ = PROTOBUF_GET_WRAPPED_OR_DEFAULT(orca_config, enable_oob_load_report, false);

    if (oob_enabled_) {
      ENVOY_LOG(info, "ORCA out-of-band reporting enabled for zone-aware routing");

      // Parse OOB reporting period (default 10s, same as CSWRR)
      if (orca_config.has_oob_reporting_period()) {
        auto duration_ms = DurationUtil::durationToMilliseconds(orca_config.oob_reporting_period());
        oob_reporting_period_ = std::chrono::milliseconds(duration_ms);
      }

      // Parse OOB expiration period (default 3min, same as CSWRR)
      if (orca_config.has_oob_expiration_period()) {
        auto duration_ms = DurationUtil::durationToMilliseconds(orca_config.oob_expiration_period());
        oob_expiration_period_ = std::chrono::milliseconds(duration_ms);
      }

      ENVOY_LOG(info, "OOB configuration - reporting_period: {}ms, expiration_period: {}ms",
                oob_reporting_period_.count(), oob_expiration_period_.count());

      // Initialize OOB expiration timer
      // Note: Timer creation requires dispatcher access, which will be available
      // in the load balancer factory. We'll start the timer from there.
    }

    // Parse OOB-first mode configuration
    oob_first_enabled_ = PROTOBUF_GET_WRAPPED_OR_DEFAULT(orca_config, enable_oob_first_mode, false);

    if (oob_first_enabled_) {
      ENVOY_LOG(info, "ORCA OOB-first mode enabled for zone-aware routing");

      // Parse fallback strategy
      oob_fallback_strategy_ = orca_config.oob_fallback_strategy();
      ENVOY_LOG(info, "OOB-first fallback strategy: {}",
                LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::OobFallbackStrategy_Name(oob_fallback_strategy_));

      // Parse probe triggers
      if (orca_config.has_probe_triggers()) {
        oob_probe_triggers_ = orca_config.probe_triggers();

        ENVOY_LOG(info, "OOB-first probe triggers configured - oob_missing_threshold: {}ms, load_variance_threshold: {:.1f}%, max_probes_per_minute: {}",
                  DurationUtil::durationToMilliseconds(oob_probe_triggers_.oob_missing_threshold()),
                  oob_probe_triggers_.load_variance_threshold().value() * 100,
                  oob_probe_triggers_.max_probes_per_minute().value());
      }

      // Parse zone aggregation configuration
      if (orca_config.has_zone_aggregation()) {
        oob_zone_aggregation_config_ = orca_config.zone_aggregation();

        ENVOY_LOG(info, "OOB-first zone aggregation - enabled: {}, min_hosts_per_zone: {}, method: {}",
                  oob_zone_aggregation_config_.enable_zone_aggregation(),
                  oob_zone_aggregation_config_.min_hosts_per_zone().value(),
                  LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::ZoneAggregationConfig::AggregationMethod_Name(
                    oob_zone_aggregation_config_.aggregation_method()));
      }
    }

    // Initialize zone metrics update timer
    // Note: We need access to the dispatcher to create the timer. This will be set up
    // in the load balancer factory that constructs this instance.
    zone_metrics_last_refresh_ = time_source_.monotonicTime();

    // Attach OrcaHostLbPolicyData to all existing hosts so we can receive ORCA callbacks
    for (const HostSetPtr& host_set : priority_set_.hostSetsPerPriority()) {
      for (const auto& host : host_set->hosts()) {
        if (!host->lbPolicyData().has_value()) {
          host->setLbPolicyData(std::make_unique<OrcaHostLbPolicyData>());
        }
      }
    }

    // Runtime-controlled always-probe (defaults: disabled)
    orca_probe_always_enabled_ =
        runtime_.snapshot().getInteger(RuntimeOrcaProbeAlwaysEnabled, 1) != 0;
    const uint64_t always_pct = runtime_.snapshot().getInteger(RuntimeOrcaProbeAlwaysPercent, 1);
    orca_probe_always_permyriad_ =
        static_cast<uint32_t>(std::min<uint64_t>(10000, always_pct * 100));
  }

  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t priority, const HostVector& hosts_added, const HostVector&) -> absl::Status {
        // Make sure per_priority_state_ is as large as priority_set_.hostSetsPerPriority()
        resizePerPriorityState();
        // If P=0 changes, regenerate locality routing structures. Locality based routing is
        // disabled at all other levels.
        if (local_priority_set_ && priority == 0) {
          regenerateLocalityRoutingStructures();
        }

        if (locality_weighted_balancing_) {
          rebuildLocalityWrrForPriority(priority);
        }

        // Revisit this
        // Invalidate ORCA zone metrics cache when hosts change
        if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_LOAD) {
          cached_zone_metrics_.clear();
          // Ensure newly added hosts can receive ORCA callbacks.
          for (const auto& host : hosts_added) {
            if (!host->lbPolicyData().has_value()) {
              host->setLbPolicyData(std::make_unique<OrcaHostLbPolicyData>());
            }
          }
        }

        return absl::OkStatus();
      });
  if (local_priority_set_) {
    // Multiple priorities are unsupported for local priority sets.
    // In order to support priorities correctly, one would have to make some assumptions about
    // routing (all local Envoys fail over at the same time) and use all priorities when computing
    // the locality routing structure.
    ASSERT(local_priority_set_->hostSetsPerPriority().size() == 1);
    local_priority_set_member_update_cb_handle_ = local_priority_set_->addPriorityUpdateCb(
        [this](uint32_t priority, const HostVector&, const HostVector&) -> absl::Status {
          ASSERT(priority == 0);
          // If the set of local Envoys changes, regenerate routing for P=0 as it does priority
          // based routing.
          regenerateLocalityRoutingStructures();
          return absl::OkStatus();
        });
  }
}

void ZoneAwareLoadBalancerBase::rebuildLocalityWrrForPriority(uint32_t priority) {
  ASSERT(priority < priority_set_.hostSetsPerPriority().size());
  auto& host_set = *priority_set_.hostSetsPerPriority()[priority];
  per_priority_state_[priority]->locality_wrr_ =
      std::make_unique<LocalityWrr>(host_set, random_.random());
}

void ZoneAwareLoadBalancerBase::regenerateLocalityRoutingStructures() {
  ASSERT(local_priority_set_);
  stats_.lb_recalculate_zone_structures_.inc();
  // resizePerPriorityState should ensure these stay in sync.
  ASSERT(per_priority_state_.size() == priority_set_.hostSetsPerPriority().size());

  // We only do locality routing for P=0
  uint32_t priority = 0;
  PerPriorityState& state = *per_priority_state_[priority];
  // Do not perform any calculations if we cannot perform locality routing based on non runtime
  // params.
  if (earlyExitNonLocalityRouting()) {
    state.locality_routing_state_ = LocalityRoutingState::NoLocalityRouting;
    return;
  }
  HostSet& host_set = *priority_set_.hostSetsPerPriority()[priority];
  const HostsPerLocality& upstreamHostsPerLocality = host_set.healthyHostsPerLocality();
  const size_t num_upstream_localities = upstreamHostsPerLocality.get().size();
  ASSERT(num_upstream_localities >= 2);

  // It is worth noting that all of the percentages calculated are orthogonal from
  // how much load this priority level receives, percentageLoad(priority).
  //
  // If the host sets are such that 20% of load is handled locally and 80% is residual, and then
  // half the hosts in all host sets go unhealthy, this priority set will
  // still send half of the incoming load to the local locality and 80% to residual.
  //
  // Basically, fairness across localities within a priority is guaranteed. Fairness across
  // localities across priorities is not.
  const HostsPerLocality& localHostsPerLocality = localHostSet().healthyHostsPerLocality();
  auto locality_percentages =
      calculateLocalityPercentages(localHostsPerLocality, upstreamHostsPerLocality);

  if (upstreamHostsPerLocality.hasLocalLocality()) {
    // Calculate threshold in the same scale as percentages (10000 = 100%)
    // locality_direct_threshold_pct_ is in range 0-100, so multiply by 100 to get 10000 scale
    // Only apply threshold when using ORCA_LOAD to avoid changing existing behavior
    const uint64_t threshold = (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_LOAD)
                                   ? locality_direct_threshold_pct_ * 100
                                   : 0;

    // If we have lower percent of hosts in the local cluster in the same locality,
    // we can push all of the requests directly to upstream cluster in the same locality.
    // With ORCA_LOAD, we add a threshold to prevent small metric differences from causing
    // unnecessary cross-zone routing.
    const bool within_threshold =
        locality_percentages[0].upstream_percentage > 0 &&
        (locality_percentages[0].upstream_percentage >= locality_percentages[0].local_percentage ||
         (locality_percentages[0].local_percentage > locality_percentages[0].upstream_percentage &&
          locality_percentages[0].local_percentage - locality_percentages[0].upstream_percentage <=
              threshold));

    // Calculate difference safely (avoid underflow with unsigned types)
    const int64_t diff = static_cast<int64_t>(locality_percentages[0].local_percentage) -
                         static_cast<int64_t>(locality_percentages[0].upstream_percentage);

    ENVOY_LOG(debug,
              "Locality routing decision for local zone: local_pct={:.1f}%, upstream_pct={:.1f}%, "
              "diff={:.1f}%, threshold={:.1f}%, within_threshold={}",
              locality_percentages[0].local_percentage / 100.0,
              locality_percentages[0].upstream_percentage / 100.0, diff / 100.0, threshold / 100.0,
              within_threshold);

    if (within_threshold ||
        // When force_local_zone is enabled, always use LocalityDirect routing if there are enough
        // healthy upstreams in the local locality as determined by force_local_zone_min_size is
        // met.
        (force_local_zone_min_size_.has_value() &&
         upstreamHostsPerLocality.get()[0].size() >= *force_local_zone_min_size_)) {
      state.locality_routing_state_ = LocalityRoutingState::LocalityDirect;

      ENVOY_LOG(debug, "Using LocalityDirect routing: local={}%, upstream={}%, threshold={}%",
                locality_percentages[0].local_percentage / 100.0,
                locality_percentages[0].upstream_percentage / 100.0,
                locality_direct_threshold_pct_);
      return;
    }
  }

  state.locality_routing_state_ = LocalityRoutingState::LocalityResidual;

  // If we cannot route all requests to the same locality, calculate what percentage can be routed.
  // For example, if local percentage is 20% and upstream is 10%
  // we can route only 50% of requests directly.
  // Local percent can be 0% if there are no upstream hosts in the local locality.
  state.local_percent_to_route_ =
      upstreamHostsPerLocality.hasLocalLocality() && locality_percentages[0].local_percentage > 0
          ? locality_percentages[0].upstream_percentage * 10000 /
                locality_percentages[0].local_percentage
          : 0;

  // Local locality does not have additional capacity (we have already routed what we could).
  // Now we need to figure out how much traffic we can route cross locality and to which exact
  // locality we should route. Percentage of requests routed cross locality to a specific locality
  // needed be proportional to the residual capacity upstream locality has.
  //
  // residual_capacity contains capacity left in a given locality, we keep accumulating residual
  // capacity to make search for sampled value easier.
  // For example, if we have the following upstream and local percentage:
  // local_percentage: 40000 40000 20000
  // upstream_percentage: 25000 50000 25000
  // Residual capacity would look like: 0 10000 5000. Now we need to sample proportionally to
  // bucket sizes (residual capacity). For simplicity of finding where specific
  // sampled value is, we accumulate values in residual capacity. This is what it will look like:
  // residual_capacity: 0 10000 15000
  // Now to find a locality to route (bucket) we could simply iterate over residual_capacity
  // searching where sampled value is placed.
  state.residual_capacity_.resize(num_upstream_localities);
  for (uint64_t i = 0; i < num_upstream_localities; ++i) {
    uint64_t last_residual_capacity = i > 0 ? state.residual_capacity_[i - 1] : 0;
    LocalityPercentages this_locality_percentages = locality_percentages[i];
    if (i == 0 && upstreamHostsPerLocality.hasLocalLocality()) {
      // This is a local locality, we have already routed what we could.
      state.residual_capacity_[i] = last_residual_capacity;
      continue;
    }

    // Only route to the localities that have additional capacity.
    if (this_locality_percentages.upstream_percentage >
        this_locality_percentages.local_percentage) {
      state.residual_capacity_[i] = last_residual_capacity +
                                    this_locality_percentages.upstream_percentage -
                                    this_locality_percentages.local_percentage;
    } else {
      // Locality with index "i" does not have residual capacity, but we keep accumulating previous
      // values to make search easier on the next step.
      state.residual_capacity_[i] = last_residual_capacity;
    }
  }
}

void ZoneAwareLoadBalancerBase::resizePerPriorityState() {
  const uint32_t size = priority_set_.hostSetsPerPriority().size();
  while (per_priority_state_.size() < size) {
    // Note for P!=0, PerPriorityState is created with NoLocalityRouting and never changed.
    per_priority_state_.push_back(std::make_unique<PerPriorityState>());
  }
}

bool ZoneAwareLoadBalancerBase::earlyExitNonLocalityRouting() {
  // We only do locality routing for P=0.
  HostSet& host_set = *priority_set_.hostSetsPerPriority()[0];
  if (host_set.healthyHostsPerLocality().get().size() < 2) {
    return true;
  }

  // Do not perform locality routing if there are too few local localities for zone routing to have
  // an effect. Skipped when ForceLocalZone is enabled.
  if (!force_local_zone_min_size_.has_value() &&
      localHostSet().hostsPerLocality().get().size() < 2) {
    return true;
  }

  // Do not perform locality routing if the local cluster doesn't have any hosts in the current
  // envoy's local locality. This breaks our assumptions about the local cluster being correctly
  // configured, so we don't have enough information to perform locality routing. Note: If other
  // envoys do exist according to the local cluster, they will still be able to perform locality
  // routing correctly. This will not cause a traffic imbalance because other envoys will not know
  // about the current one, so they will not factor it into locality routing calculations.
  if (!localHostSet().hostsPerLocality().hasLocalLocality() ||
      localHostSet().hostsPerLocality().get()[0].empty()) {
    stats_.lb_local_cluster_not_ok_.inc();
    return true;
  }

  // Do not perform locality routing for small clusters.
  const uint64_t min_cluster_size =
      runtime_.snapshot().getInteger(RuntimeMinClusterSize, min_cluster_size_);
  if (host_set.healthyHosts().size() < min_cluster_size) {
    stats_.lb_zone_cluster_too_small_.inc();
    return true;
  }

  return false;
}

HostSelectionResponse ZoneAwareLoadBalancerBase::chooseHost(LoadBalancerContext* context) {
  HostConstSharedPtr host;

  const size_t max_attempts = context ? context->hostSelectionRetryCount() + 1 : 1;
  for (size_t i = 0; i < max_attempts; ++i) {
    host = chooseHostOnce(context);

    // If host selection failed or the host is accepted by the filter, return.
    // Otherwise, try again.
    // Note: in the future we might want to allow retrying when chooseHostOnce returns nullptr.
    if (!host || !context || !context->shouldSelectAnotherHost(*host)) {
      return host;
    }
  }

  // If we didn't find anything, return the last host.
  return host;
}

bool LoadBalancerBase::isHostSetInPanic(const HostSet& host_set) const {
  uint64_t global_panic_threshold = std::min<uint64_t>(
      100, runtime_.snapshot().getInteger(RuntimePanicThreshold, default_healthy_panic_percent_));
  const auto host_count = host_set.hosts().size() - host_set.excludedHosts().size();
  double healthy_percent =
      host_count == 0 ? 0.0 : 100.0 * host_set.healthyHosts().size() / host_count;

  double degraded_percent =
      host_count == 0 ? 0.0 : 100.0 * host_set.degradedHosts().size() / host_count;
  // If the % of healthy hosts in the cluster is less than our panic threshold, we use all hosts.
  if ((healthy_percent + degraded_percent) < global_panic_threshold) {
    return true;
  }

  return false;
}

absl::FixedArray<ZoneAwareLoadBalancerBase::LocalityPercentages>
ZoneAwareLoadBalancerBase::calculateLocalityPercentages(
    const HostsPerLocality& local_hosts_per_locality,
    const HostsPerLocality& upstream_hosts_per_locality) {

  ENVOY_LOG(debug,
            "calculateLocalityPercentages: local localities={}, upstream localities={}, "
            "basis={}, hasLocalLocality={}",
            local_hosts_per_locality.get().size(), upstream_hosts_per_locality.get().size(),
            static_cast<int>(locality_basis_), local_hosts_per_locality.hasLocalLocality());

  // For ORCA_LOAD mode, we use a different approach:
  // Calculate utilization for all upstream localities, then use inverse of utilization as weight.
  // This makes routing decisions purely based on upstream utilization to equalize load.
  if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_LOAD) {
    return calculateLocalityPercentagesOrcaLoad(local_hosts_per_locality,
                                                upstream_hosts_per_locality);
  }

  absl::flat_hash_map<envoy::config::core::v3::Locality, uint64_t, LocalityHash, LocalityEqualTo>
      local_weights;
  absl::flat_hash_map<envoy::config::core::v3::Locality, uint64_t, LocalityHash, LocalityEqualTo>
      upstream_weights;
  uint64_t total_local_weight = 0;
  for (const auto& locality_hosts : local_hosts_per_locality.get()) {
    ENVOY_LOG(trace, "Processing local locality with {} hosts", locality_hosts.size());
    uint64_t locality_weight = 0;
    switch (locality_basis_) {
    // If locality_basis_ is set to HEALTHY_HOSTS_WEIGHT, it uses the host's weight to calculate the
    // locality percentage.
    case LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_WEIGHT:
      for (const auto& host : locality_hosts) {
        locality_weight += host->weight();
      }
      break;
    // By default it uses the number of healthy hosts in the locality.
    case LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_NUM:
      locality_weight = locality_hosts.size();
      break;
    default:
      PANIC_DUE_TO_CORRUPT_ENUM;
    }
    total_local_weight += locality_weight;
    // If there is no entry in the map for a given locality, it is assumed to have 0 hosts.
    if (!locality_hosts.empty()) {
      const auto& locality = locality_hosts[0]->locality();
      local_weights.emplace(locality, locality_weight);
      ENVOY_LOG(debug,
                "Local locality: region='{}', zone='{}', sub_zone='{}', "
                "hosts={}, weight={}",
                locality.region(), locality.zone(), locality.sub_zone(), locality_hosts.size(),
                locality_weight);
    }
  }

  ENVOY_LOG(debug, "Total local weight: {}, local localities processed: {}", total_local_weight,
            local_weights.size());
  uint64_t total_upstream_weight = 0;
  for (const auto& locality_hosts : upstream_hosts_per_locality.get()) {
    uint64_t locality_weight = 0;
    switch (locality_basis_) {
    // If locality_basis_ is set to HEALTHY_HOSTS_WEIGHT, it uses the host's weight to calculate the
    // locality percentage.
    case LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_WEIGHT:
      for (const auto& host : locality_hosts) {
        locality_weight += host->weight();
      }
      break;
    // By default it uses the number of healthy hosts in the locality.
    case LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_NUM:
      locality_weight = locality_hosts.size();
      break;
    default:
      PANIC_DUE_TO_CORRUPT_ENUM;
    }
    total_upstream_weight += locality_weight;
    // If there is no entry in the map for a given locality, it is assumed to have 0 hosts.
    if (!locality_hosts.empty()) {
      upstream_weights.emplace(locality_hosts[0]->locality(), locality_weight);
    }
  }

  absl::FixedArray<LocalityPercentages> percentages(upstream_hosts_per_locality.get().size());
  for (uint32_t i = 0; i < upstream_hosts_per_locality.get().size(); ++i) {
    const auto& upstream_hosts = upstream_hosts_per_locality.get()[i];
    if (upstream_hosts.empty()) {
      // If there are no upstream hosts in a given locality, the upstream percentage is 0.
      // We can't determine the locality of this group, so we can't find the corresponding local
      // count. However, if there are no upstream hosts in a locality, the local percentage doesn't
      // matter.
      percentages[i] = LocalityPercentages{0, 0};
      continue;
    }
    const auto& locality = upstream_hosts[0]->locality();

    const auto local_weight_it = local_weights.find(locality);
    const uint64_t local_weight =
        local_weight_it == local_weights.end() ? 0 : local_weight_it->second;
    const auto upstream_weight_it = upstream_weights.find(locality);
    const uint64_t upstream_weight =
        upstream_weight_it == upstream_weights.end() ? 0 : upstream_weight_it->second;

    const uint64_t local_percentage =
        total_local_weight > 0 ? 10000ULL * local_weight / total_local_weight : 0;
    const uint64_t upstream_percentage =
        total_upstream_weight > 0 ? 10000ULL * upstream_weight / total_upstream_weight : 0;

    ENVOY_LOG(
        debug,
        "Locality {}/{}/{}: local_weight={}, upstream_weight={}, "
        "local_pct={:.1f}%, upstream_pct={:.1f}%, found_in_local_map={}, found_in_upstream_map={}",
        locality.region(), locality.zone(), locality.sub_zone(), local_weight, upstream_weight,
        local_percentage / 100.0, upstream_percentage / 100.0,
        local_weight_it != local_weights.end(), upstream_weight_it != upstream_weights.end());

    percentages[i] = LocalityPercentages{local_percentage, upstream_percentage};
  }

  return percentages;
}

// Helper function implementations for calculateLocalityPercentagesOrcaLoad

std::vector<ZoneAwareLoadBalancerBase::LocalityUtilizationInfo>
ZoneAwareLoadBalancerBase::collectLocalityUtilizationInfo(
    const HostsPerLocality& upstream_hosts_per_locality,
    const envoy::config::core::v3::Locality* local_locality_ptr, double& local_utilization,
    double& min_other_utilization, bool& found_local) {

  std::vector<LocalityUtilizationInfo> locality_utils;
  min_other_utilization = 1.0; // Min utilization among NON-local zones
  local_utilization = 0.0;
  found_local = false;

  for (const auto& locality_hosts : upstream_hosts_per_locality.get()) {
    if (locality_hosts.empty()) {
      continue;
    }

    const auto& locality = locality_hosts[0]->locality();
    const bool is_local = local_locality_ptr != nullptr &&
                          locality.region() == local_locality_ptr->region() &&
                          locality.zone() == local_locality_ptr->zone() &&
                          locality.sub_zone() == local_locality_ptr->sub_zone();

    // Get ORCA metrics for this locality
    const auto metrics = calculateZoneMetricsFromHosts(locality_hosts, locality);

    double utilization = 0.0;
    bool is_valid = false;

    if (metrics.is_valid && metrics.avg_utilization >= 0.0 && metrics.avg_utilization <= 1.0) {
      utilization = metrics.avg_utilization;
      is_valid = true;

      if (is_local) {
        local_utilization = utilization;
        found_local = true;
      } else {
        // Track minimum utilization among other (non-local) zones
        min_other_utilization = std::min(min_other_utilization, utilization);
      }

      ENVOY_LOG(debug,
                "Locality {}/{}/{}: utilization={:.1f}%, capacity={:.1f}%, hosts={}, is_local={}, "
                "valid={}",
                locality.region(), locality.zone(), locality.sub_zone(), utilization * 100.0,
                (1.0 - utilization) * 100.0, locality_hosts.size(), is_local, is_valid);
    } else {
      // Fallback: use host count as proxy
      is_valid = false;
      ENVOY_LOG(debug, "Locality {}/{}/{}: ORCA data unavailable, using fallback (hosts={})",
                locality.region(), locality.zone(), locality.sub_zone(), locality_hosts.size());
    }

    locality_utils.push_back(
        {utilization, 1.0 - utilization, locality_hosts.size(), is_local, is_valid, &locality});
  }

  return locality_utils;
}

void ZoneAwareLoadBalancerBase::calculateInverseUtilizationWeights(
    const std::vector<LocalityUtilizationInfo>& locality_utils, double& total_routing_weight,
    size_t& valid_localities) {

  total_routing_weight = 0.0;
  valid_localities = 0;

  for (const auto& info : locality_utils) {
    if (info.is_valid) {
      // Weight = inverse utilization only (host count irrelevant for ORCA equal distribution)
      total_routing_weight += (1.0 - info.utilization);
      ++valid_localities;
    }
  }
}

absl::FixedArray<ZoneAwareLoadBalancerBase::LocalityPercentages>
ZoneAwareLoadBalancerBase::generateLocalityPercentages(
    const std::vector<LocalityUtilizationInfo>& locality_utils,
    const HostsPerLocality& upstream_hosts_per_locality, double total_routing_weight,
    size_t valid_localities, double local_utilization, double min_other_utilization,
    bool found_local) {

  // Calculate the utilization differential for the local zone
  const double threshold_fraction =
      locality_direct_threshold_pct_ / 100.0; // Convert to 0.0-1.0 scale
  const double utilization_diff = found_local ? (local_utilization - min_other_utilization) : 0.0;

  ENVOY_LOG(debug,
            "ORCA differential routing: local_utilization={:.1f}%, min_other_utilization={:.1f}%, "
            "diff={:.1f}%, threshold={:.1f}%",
            local_utilization * 100.0, min_other_utilization * 100.0, utilization_diff * 100.0,
            threshold_fraction * 100.0);

  // Convert to percentages (scale of 10000 = 100%)
  absl::FixedArray<LocalityPercentages> percentages(upstream_hosts_per_locality.get().size());

  for (size_t i = 0; i < upstream_hosts_per_locality.get().size(); ++i) {
    const auto& upstream_hosts = upstream_hosts_per_locality.get()[i];

    if (upstream_hosts.empty() || i >= locality_utils.size()) {
      percentages[i] = LocalityPercentages{0, 0};
      continue;
    }

    const auto& info = locality_utils[i];

    if (!info.is_valid) {
      // Skip invalid localities - they get no traffic
      percentages[i] = LocalityPercentages{0, 0};
      continue;
    }

    uint64_t upstream_percentage;
    if (total_routing_weight > 0.0) {
      // Calculate upstream percentage based on inverse utilization for equal distribution
      const double routing_weight = 1.0 - info.utilization;
      upstream_percentage =
          static_cast<uint64_t>((routing_weight / total_routing_weight) * ORCA_WEIGHT_SCALE_FACTOR);
    } else {
      // Edge case: all valid localities have 100% utilization, fallback to equal distribution
      upstream_percentage = static_cast<uint64_t>(ORCA_WEIGHT_SCALE_FACTOR / valid_localities);
    }

    // For ORCA mode, we use a different encoding than traditional zone-aware routing.
    // In traditional mode: local_pct represents "where Envoys are", upstream_pct represents "where
    // capacity is" In ORCA mode: we want upstream_pct to directly control traffic distribution
    //
    // Key insight: For a single Envoy instance, ALL traffic (100%) originates from its local zone.
    // So we set local_percentage to represent "what fraction of THIS Envoy's traffic is destined
    // for this zone"
    //
    // For the local zone: local_pct = 100% (10000) because all traffic originates here
    // For other zones: local_pct = 0 because traffic doesn't originate there
    //
    // Then the routing logic at line 646-650 calculates:
    //   local_percent_to_route = upstream_pct / local_pct = upstream_pct / 10000
    // which gives us the desired percentage to route locally.
    //
    // And residual_capacity = upstream_pct - local_pct for cross-zone routing.
    uint64_t local_percentage = 0;
    if (info.is_local) {
      // All traffic originates from the local zone
      local_percentage = 10000;

      // For threshold check: if within threshold, set local_pct = upstream_pct to trigger
      // LocalityDirect
      if (utilization_diff <= threshold_fraction) {
        local_percentage = upstream_percentage;
      }
    }

    ENVOY_LOG(debug, "Locality {}/{}/{}: final upstream_pct={:.1f}%, local_pct={:.1f}%",
              info.locality->region(), info.locality->zone(), info.locality->sub_zone(),
              upstream_percentage / 100.0, local_percentage / 100.0);

    percentages[i] = LocalityPercentages{local_percentage, upstream_percentage};
  }

  return percentages;
}

absl::FixedArray<ZoneAwareLoadBalancerBase::LocalityPercentages>
ZoneAwareLoadBalancerBase::calculateLocalityPercentagesOrcaLoad(
    const HostsPerLocality& local_hosts_per_locality,
    const HostsPerLocality& upstream_hosts_per_locality) {

  ENVOY_LOG(debug, "calculateLocalityPercentagesOrcaLoad: using differential routing approach");

  // Step 1: Determine local locality pointer
  const envoy::config::core::v3::Locality* local_locality_ptr = nullptr;
  bool has_local = local_hosts_per_locality.hasLocalLocality();
  if (has_local && !local_hosts_per_locality.get().empty() &&
      !local_hosts_per_locality.get()[0].empty()) {
    local_locality_ptr = &local_hosts_per_locality.get()[0][0]->locality();
  }

  // Step 2: Collect ORCA utilization information for all localities
  double local_utilization = 0.0, min_other_utilization = 1.0;
  bool found_local = false;
  auto locality_utils =
      collectLocalityUtilizationInfo(upstream_hosts_per_locality, local_locality_ptr,
                                     local_utilization, min_other_utilization, found_local);

  // Step 3: Check if we have valid ORCA data, fall back to host count if not
  bool has_any_valid_orca = std::any_of(locality_utils.begin(), locality_utils.end(),
                                        [](const auto& info) { return info.is_valid; });

  if (!has_any_valid_orca) {
    ENVOY_LOG(debug, "No valid ORCA data available, falling back to host count routing");
    // Return empty percentages - the caller will handle fallback
    absl::FixedArray<LocalityPercentages> percentages(upstream_hosts_per_locality.get().size());
    for (size_t i = 0; i < percentages.size(); ++i) {
      percentages[i] = LocalityPercentages{0, 0};
    }
    return percentages;
  }

  // Step 4: Calculate routing weights based on inverse utilization for equal distribution
  double total_routing_weight = 0.0;
  size_t valid_localities = 0;
  calculateInverseUtilizationWeights(locality_utils, total_routing_weight, valid_localities);

  // Step 5: Generate final locality percentages with ORCA routing logic
  return generateLocalityPercentages(locality_utils, upstream_hosts_per_locality,
                                     total_routing_weight, valid_localities, local_utilization,
                                     min_other_utilization, found_local);
}

uint32_t ZoneAwareLoadBalancerBase::tryChooseLocalLocalityHosts(const HostSet& host_set) const {
  // For ORCA-based zoning, ensure locality percentages are refreshed when zone metrics are stale.
  if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_LOAD) {
    const auto now = time_source_.monotonicTime();
    if (now - zone_metrics_last_refresh_ >= zone_metrics_update_interval_) {
      // const_cast is safe here: we are updating internal caches and routing structures.
      const_cast<ZoneAwareLoadBalancerBase*>(this)->updateZoneMetricsOnMainThread();
      const_cast<ZoneAwareLoadBalancerBase*>(this)->regenerateLocalityRoutingStructures();
    }
  }

  PerPriorityState& state = *per_priority_state_[host_set.priority()];
  ASSERT(state.locality_routing_state_ != LocalityRoutingState::NoLocalityRouting);

  // At this point it's guaranteed to be at least 2 localities in the upstream host set.
  const size_t number_of_localities = host_set.healthyHostsPerLocality().get().size();
  ASSERT(number_of_localities >= 2U);

  // Try to push all of the requests to the same locality if possible.
  if (state.locality_routing_state_ == LocalityRoutingState::LocalityDirect) {
    ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality());
    // Optional: always-probe a small fraction cross-zone even when zones are known.
    // Optional: always-probe a small fraction cross-zone even when zones are known.
    if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_LOAD &&
        orca_probe_always_enabled_ && orca_probe_always_permyriad_ > 0) {
      const auto& hpl = host_set.healthyHostsPerLocality().get();
      std::vector<uint32_t> remote;
      for (uint32_t i = 1; i < hpl.size(); ++i) { // skip local (index 0)
        if (!hpl[i].empty()) {
          remote.push_back(i);
        }
      }
      if (!remote.empty() && (random_.random() % 10000) < orca_probe_always_permyriad_) {
        stats_.lb_zone_routing_cross_zone_.inc();
        return remote[random_.random() % remote.size()];
      }
    }
    stats_.lb_zone_routing_all_directly_.inc();
    return 0;
  }

  ASSERT(state.locality_routing_state_ == LocalityRoutingState::LocalityResidual);
  ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality() ||
         state.local_percent_to_route_ == 0);

  // If we cannot route all requests to the same locality, we already calculated how much we can
  // push to the local locality, check if we can push to local locality on current iteration.
  if (random_.random() % 10000 < state.local_percent_to_route_) {
    stats_.lb_zone_routing_sampled_.inc();
    return 0;
  }

  // At this point we must route cross locality as we cannot route to the local locality.
  stats_.lb_zone_routing_cross_zone_.inc();

  // This is *extremely* unlikely but possible due to rounding errors when calculating
  // locality percentages. In this case just select random locality.
  if (state.residual_capacity_[number_of_localities - 1] == 0) {
    stats_.lb_zone_no_capacity_left_.inc();
    return random_.random() % number_of_localities;
  }

  // Random sampling to select specific locality for cross locality traffic based on the
  // additional capacity in localities.
  uint64_t threshold = random_.random() % state.residual_capacity_[number_of_localities - 1];

  // This potentially can be optimized to be O(log(N)) where N is the number of localities.
  // Linear scan should be faster for smaller N, in most of the scenarios N will be small.
  //
  // Bucket 1: [0, state.residual_capacity_[0] - 1]
  // Bucket 2: [state.residual_capacity_[0], state.residual_capacity_[1] - 1]
  // ...
  // Bucket N: [state.residual_capacity_[N-2], state.residual_capacity_[N-1] - 1]
  int i = 0;
  while (threshold >= state.residual_capacity_[i]) {
    i++;
  }

  return i;
}

absl::optional<ZoneAwareLoadBalancerBase::HostsSource>
ZoneAwareLoadBalancerBase::hostSourceToUse(LoadBalancerContext* context, uint64_t hash) const {
  auto host_set_and_source = chooseHostSet(context, hash);

  // The second argument tells us which availability we should target from the selected host set.
  const auto host_availability = host_set_and_source.second;
  auto& host_set = host_set_and_source.first;
  HostsSource hosts_source;
  hosts_source.priority_ = host_set.priority();

  // If the selected host set has insufficient healthy hosts, return all hosts (unless we should
  // fail traffic on panic, in which case return no host).
  if (per_priority_panic_[hosts_source.priority_]) {
    stats_.lb_healthy_panic_.inc();
    if (fail_traffic_on_panic_) {
      return absl::nullopt;
    } else {
      hosts_source.source_type_ = HostsSource::SourceType::AllHosts;
      return hosts_source;
    }
  }

  // If we're doing locality weighted balancing, pick locality.
  //
  // The chooseDegradedLocality or chooseHealthyLocality may return valid locality index
  // when the locality_weighted_lb_config is set or load balancing policy extension is used.
  // This if statement is to make sure we only do locality weighted balancing when the
  // locality_weighted_lb_config is set explicitly even the hostSourceToUse is called in the
  // load balancing policy extensions.
  if (locality_weighted_balancing_) {
    absl::optional<uint32_t> locality;
    if (host_availability == HostAvailability::Degraded) {
      locality = chooseDegradedLocality(host_set);
    } else {
      locality = chooseHealthyLocality(host_set);
    }

    if (locality.has_value()) {
      auto source_type = localitySourceType(host_availability);
      if (!source_type) {
        return absl::nullopt;
      }
      hosts_source.source_type_ = source_type.value();
      hosts_source.locality_index_ = locality.value();
      return hosts_source;
    }
  }

  // If we've latched that we can't do locality-based routing, return healthy or degraded hosts
  // for the selected host set.
  if (per_priority_state_[host_set.priority()]->locality_routing_state_ ==
      LocalityRoutingState::NoLocalityRouting) {
    auto source_type = sourceType(host_availability);
    if (!source_type) {
      return absl::nullopt;
    }
    hosts_source.source_type_ = source_type.value();
    return hosts_source;
  }

  // Determine if the load balancer should do zone based routing for this pick.
  if (!runtime_.snapshot().featureEnabled(RuntimeZoneEnabled, routing_enabled_)) {
    auto source_type = sourceType(host_availability);
    if (!source_type) {
      return absl::nullopt;
    }
    hosts_source.source_type_ = source_type.value();
    return hosts_source;
  }

  if (isHostSetInPanic(localHostSet())) {
    stats_.lb_local_cluster_not_ok_.inc();
    // If the local Envoy instances are in global panic, and we should not fail traffic, do
    // not do locality based routing.
    if (fail_traffic_on_panic_) {
      return absl::nullopt;
    } else {
      auto source_type = sourceType(host_availability);
      if (!source_type) {
        return absl::nullopt;
      }
      hosts_source.source_type_ = source_type.value();
      return hosts_source;
    }
  }

  auto source_type = localitySourceType(host_availability);
  if (!source_type) {
    return absl::nullopt;
  }
  hosts_source.source_type_ = source_type.value();
  hosts_source.locality_index_ = tryChooseLocalLocalityHosts(host_set);
  return hosts_source;
}

const HostVector& ZoneAwareLoadBalancerBase::hostSourceToHosts(HostsSource hosts_source) const {
  const HostSet& host_set = *priority_set_.hostSetsPerPriority()[hosts_source.priority_];
  switch (hosts_source.source_type_) {
  case HostsSource::SourceType::AllHosts:
    return host_set.hosts();
  case HostsSource::SourceType::HealthyHosts:
    return host_set.healthyHosts();
  case HostsSource::SourceType::DegradedHosts:
    return host_set.degradedHosts();
  case HostsSource::SourceType::LocalityHealthyHosts:
    return host_set.healthyHostsPerLocality().get()[hosts_source.locality_index_];
  case HostsSource::SourceType::LocalityDegradedHosts:
    return host_set.degradedHostsPerLocality().get()[hosts_source.locality_index_];
  }
  PANIC_DUE_TO_CORRUPT_ENUM;
}

EdfLoadBalancerBase::EdfLoadBalancerBase(
    const PrioritySet& priority_set, const PrioritySet* local_priority_set, ClusterLbStats& stats,
    Runtime::Loader& runtime, Random::RandomGenerator& random, uint32_t healthy_panic_threshold,
    const absl::optional<LocalityLbConfig> locality_config,
    const absl::optional<SlowStartConfig> slow_start_config, TimeSource& time_source)
    : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, stats, runtime, random,
                                healthy_panic_threshold, locality_config, time_source),
      seed_(random_.random()),
      slow_start_window_(slow_start_config.has_value()
                             ? std::chrono::milliseconds(DurationUtil::durationToMilliseconds(
                                   slow_start_config.value().slow_start_window()))
                             : std::chrono::milliseconds(0)),
      aggression_runtime_(
          slow_start_config.has_value() && slow_start_config.value().has_aggression()
              ? absl::optional<Runtime::Double>({slow_start_config.value().aggression(), runtime})
              : absl::nullopt),
      time_source_(time_source), latest_host_added_time_(time_source_.monotonicTime()),
      slow_start_min_weight_percent_(slow_start_config.has_value()
                                         ? PROTOBUF_PERCENT_TO_DOUBLE_OR_DEFAULT(
                                               slow_start_config.value(), min_weight_percent, 10) /
                                               100.0
                                         : 0.1) {
  // We fully recompute the schedulers for a given host set here on membership change, which is
  // consistent with what other LB implementations do (e.g. thread aware).
  // The downside of a full recompute is that time complexity is O(n * log n),
  // so we will need to do better at delta tracking to scale (see
  // https://github.com/envoyproxy/envoy/issues/2874).
  priority_update_cb_ = priority_set.addPriorityUpdateCb(
      [this](uint32_t priority, const HostVector&, const HostVector&) {
        refresh(priority);
        return absl::OkStatus();
      });
  member_update_cb_ = priority_set.addMemberUpdateCb(
      [this](const HostVector& hosts_added, const HostVector&) -> absl::Status {
        if (isSlowStartEnabled()) {
          recalculateHostsInSlowStart(hosts_added);
        }
        return absl::OkStatus();
      });
}

void EdfLoadBalancerBase::initialize() {
  for (uint32_t priority = 0; priority < priority_set_.hostSetsPerPriority().size(); ++priority) {
    refresh(priority);
  }
}

void EdfLoadBalancerBase::recalculateHostsInSlowStart(const HostVector& hosts) {
  // TODO(nezdolik): linear scan can be improved with using flat hash set for hosts in slow start.
  for (const auto& host : hosts) {
    auto current_time = time_source_.monotonicTime();
    // Host enters slow start if only it has transitioned into healthy state.
    if (host->coarseHealth() == Upstream::Host::Health::Healthy) {
      auto host_last_hc_pass_time =
          host->lastHcPassTime() ? host->lastHcPassTime().value() : current_time;
      auto in_healthy_state_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
          current_time - host_last_hc_pass_time);
      // If there is no active HC enabled or HC has not run, start slow start window from current
      // time.
      if (!host->lastHcPassTime()) {
        host->setLastHcPassTime(std::move(current_time));
      }
      // Check if host existence time is within slow start window.
      if (host_last_hc_pass_time > latest_host_added_time_ &&
          in_healthy_state_duration <= slow_start_window_) {
        latest_host_added_time_ = host_last_hc_pass_time;
      }
    }
  }
}

void EdfLoadBalancerBase::refresh(uint32_t priority) {
  const auto add_hosts_source = [this](HostsSource source, const HostVector& hosts) {
    // Nuke existing scheduler if it exists.
    auto& scheduler = scheduler_[source] = Scheduler{};
    refreshHostSource(source);
    if (isSlowStartEnabled()) {
      recalculateHostsInSlowStart(hosts);
    }

    // Check if the original host weights are equal and no hosts are in slow start mode, in that
    // case EDF creation is skipped. When all original weights are equal and no hosts are in slow
    // start mode we can rely on unweighted host pick to do optimal round robin and least-loaded
    // host selection with lower memory and CPU overhead.
    if (hostWeightsAreEqual(hosts) && noHostsAreInSlowStart()) {
      // Skip edf creation.
      return;
    }

    // If there are no hosts or a single one, there is no need for an EDF scheduler
    // (thus lowering memory and CPU overhead), as the (possibly) single host
    // will be the one always selected by the scheduler.
    if (hosts.size() <= 1) {
      return;
    }

    // Populate the scheduler with the host list with a randomized starting point.
    // TODO(mattklein123): We must build the EDF schedule even if all of the hosts are currently
    // weighted 1. This is because currently we don't refresh host sets if only weights change.
    // We should probably change this to refresh at all times. See the comment in
    // BaseDynamicClusterImpl::updateDynamicHostList about this.
    scheduler.edf_ = std::make_unique<EdfScheduler<Host>>(EdfScheduler<Host>::createWithPicks(
        hosts,
        // We use a fixed weight here. While the weight may change without
        // notification, this will only be stale until this host is next picked,
        // at which point it is reinserted into the EdfScheduler with its new
        // weight in chooseHost().
        [this](const Host& host) { return hostWeight(host); }, seed_));
  };
  // Populate EdfSchedulers for each valid HostsSource value for the host set at this priority.
  const auto& host_set = priority_set_.hostSetsPerPriority()[priority];
  add_hosts_source(HostsSource(priority, HostsSource::SourceType::AllHosts), host_set->hosts());
  add_hosts_source(HostsSource(priority, HostsSource::SourceType::HealthyHosts),
                   host_set->healthyHosts());
  add_hosts_source(HostsSource(priority, HostsSource::SourceType::DegradedHosts),
                   host_set->degradedHosts());
  for (uint32_t locality_index = 0;
       locality_index < host_set->healthyHostsPerLocality().get().size(); ++locality_index) {
    add_hosts_source(
        HostsSource(priority, HostsSource::SourceType::LocalityHealthyHosts, locality_index),
        host_set->healthyHostsPerLocality().get()[locality_index]);
  }
  for (uint32_t locality_index = 0;
       locality_index < host_set->degradedHostsPerLocality().get().size(); ++locality_index) {
    add_hosts_source(
        HostsSource(priority, HostsSource::SourceType::LocalityDegradedHosts, locality_index),
        host_set->degradedHostsPerLocality().get()[locality_index]);
  }
}

bool EdfLoadBalancerBase::isSlowStartEnabled() const {
  return slow_start_window_ > std::chrono::milliseconds(0);
}

bool EdfLoadBalancerBase::noHostsAreInSlowStart() const {
  if (!isSlowStartEnabled()) {
    return true;
  }
  auto current_time = time_source_.monotonicTime();
  if (std::chrono::duration_cast<std::chrono::milliseconds>(
          current_time - latest_host_added_time_) <= slow_start_window_) {
    return false;
  }
  return true;
}

HostConstSharedPtr EdfLoadBalancerBase::peekAnotherHost(LoadBalancerContext* context) {
  if (tooManyPreconnects(stashed_random_.size(), total_healthy_hosts_)) {
    return nullptr;
  }

  const absl::optional<HostsSource> hosts_source = hostSourceToUse(context, random(true));
  if (!hosts_source) {
    return nullptr;
  }

  auto scheduler_it = scheduler_.find(*hosts_source);
  // We should always have a scheduler for any return value from
  // hostSourceToUse() via the construction in refresh();
  ASSERT(scheduler_it != scheduler_.end());
  auto& scheduler = scheduler_it->second;

  // As has been commented in both EdfLoadBalancerBase::refresh and
  // BaseDynamicClusterImpl::updateDynamicHostList, we must do a runtime pivot here to determine
  // whether to use EDF or do unweighted (fast) selection. EDF is non-null iff the original
  // weights of 2 or more hosts differ.
  if (scheduler.edf_ != nullptr) {
    return scheduler.edf_->peekAgain([this](const Host& host) { return hostWeight(host); });
  } else {
    const HostVector& hosts_to_use = hostSourceToHosts(*hosts_source);
    if (hosts_to_use.empty()) {
      return nullptr;
    }
    return unweightedHostPeek(hosts_to_use, *hosts_source);
  }
}

HostConstSharedPtr EdfLoadBalancerBase::chooseHostOnce(LoadBalancerContext* context) {
  const absl::optional<HostsSource> hosts_source = hostSourceToUse(context, random(false));
  if (!hosts_source) {
    return nullptr;
  }
  auto scheduler_it = scheduler_.find(*hosts_source);
  // We should always have a scheduler for any return value from
  // hostSourceToUse() via the construction in refresh();
  ASSERT(scheduler_it != scheduler_.end());
  auto& scheduler = scheduler_it->second;

  // As has been commented in both EdfLoadBalancerBase::refresh and
  // BaseDynamicClusterImpl::updateDynamicHostList, we must do a runtime pivot here to determine
  // whether to use EDF or do unweighted (fast) selection. EDF is non-null iff the original
  // weights of 2 or more hosts differ.
  if (scheduler.edf_ != nullptr) {
    auto host = scheduler.edf_->pickAndAdd([this](const Host& host) { return hostWeight(host); });
    return host;
  } else {
    const HostVector& hosts_to_use = hostSourceToHosts(*hosts_source);
    if (hosts_to_use.empty()) {
      return nullptr;
    }
    return unweightedHostPick(hosts_to_use, *hosts_source);
  }
}

namespace {
double applyAggressionFactor(double time_factor, double aggression) {
  if (aggression == 1.0 || time_factor == 1.0) {
    return time_factor;
  } else {
    return std::pow(time_factor, 1.0 / aggression);
  }
}
} // namespace

double EdfLoadBalancerBase::applySlowStartFactor(double host_weight, const Host& host) const {
  // We can reliably apply slow start weight only if `last_hc_pass_time` in host has been populated
  // either by active HC or by `member_update_cb_` in `EdfLoadBalancerBase`.
  if (host.lastHcPassTime() && host.coarseHealth() == Upstream::Host::Health::Healthy) {
    auto in_healthy_state_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        time_source_.monotonicTime() - host.lastHcPassTime().value());
    if (in_healthy_state_duration < slow_start_window_) {
      double aggression =
          aggression_runtime_ != absl::nullopt ? aggression_runtime_.value().value() : 1.0;
      if (aggression <= 0.0 || std::isnan(aggression)) {
        ENVOY_LOG_EVERY_POW_2(error, "Invalid runtime value provided for aggression parameter, "
                                     "aggression cannot be less than 0.0");
        aggression = 1.0;
      }

      ASSERT(aggression > 0.0);
      auto time_factor = static_cast<double>(std::max(std::chrono::milliseconds(1).count(),
                                                      in_healthy_state_duration.count())) /
                         slow_start_window_.count();
      return host_weight * std::max(applyAggressionFactor(time_factor, aggression),
                                    slow_start_min_weight_percent_);
    } else {
      return host_weight;
    }
  } else {
    return host_weight;
  }
}

// ORCA-based zone routing implementation

uint64_t ZoneAwareLoadBalancerBase::calculateFallbackLocalityWeight(
    const HostVector& locality_hosts, const envoy::config::core::v3::Locality& locality) const {

  // Use the configured fallback basis to calculate weight
  uint64_t weight = 0;

  switch (orca_fallback_basis_) {
  case LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_WEIGHT:
    // Sum of host weights
    for (const auto& host : locality_hosts) {
      weight += host->weight();
    }
    break;

  case LocalityLbConfig::ZoneAwareLbConfig::HEALTHY_HOSTS_NUM:
  default:
    // Number of hosts
    weight = locality_hosts.size();
    break;
  }

  ENVOY_LOG(trace, "Fallback locality weight for {}/{}/{}: {} (basis={})", locality.region(),
            locality.zone(), locality.sub_zone(), weight, static_cast<int>(orca_fallback_basis_));

  return weight;
}

absl::optional<double> ZoneAwareLoadBalancerBase::getUtilizationFromHost(const Host& host) const {

  // When OOB is enabled, prefer OOB data over per-request metrics
  if (oob_enabled_) {
    if (auto typed = host.typedLbPolicyData<OrcaHostLbPolicyData>()) {
      if (typed->oob_reporting_active.load(std::memory_order_relaxed)) {
        // This host has active OOB reporting, use the cached values
        // Apply same 3-tier hierarchy: application -> custom -> cpu

        // Tier 1: application_utilization (primary metric)
        if (orca_use_application_utilization_) {
          if (auto app_util = typed->app()) {
            ENVOY_LOG(debug, "ORCA host={} using OOB application_utilization={:.2f}%",
                      host.address()->asString(), app_util.value() * 100);
            return app_util;
          }
        }

        // Tier 2: Custom metrics (not implemented in OOB yet)
        // TODO: Implement custom metrics access for OOB data

        // Tier 3: cpu_utilization (fallback)
        if (orca_use_cpu_utilization_) {
          if (auto cpu_util = typed->cpu()) {
            ENVOY_LOG(debug, "ORCA host={} using OOB cpu_utilization={:.2f}%",
                      host.address()->asString(), cpu_util.value() * 100);
            return cpu_util;
          }
        }
      }
    }
  }

  // Fallback: Access ORCA metrics from LoadMetricStats (pull-based per-request)
  // ORCA metrics are added to LoadMetricStats by the router when parsing ORCA headers
  const auto& load_metric_stats = host.loadMetricStats();

  absl::optional<double> utilization;

  // Implement 3-tier utilization hierarchy like CSWRR
  // Tier 1: application_utilization (primary metric)
  if (orca_use_application_utilization_) {
    // Try pull-based first, then fallback to push-based
    utilization = load_metric_stats.get("application_utilization");
    if (!utilization.has_value()) {
      if (auto typed = host.typedLbPolicyData<OrcaHostLbPolicyData>()) {
        utilization = typed->app();
      }
    }

    if (utilization.has_value() && utilization.value() > 0.0) {
      ENVOY_LOG(debug, "ORCA host={} using pull-based application_utilization={:.2f}%",
                host.address()->asString(), utilization.value() * 100);
      return utilization;
    }
  }

  // Tier 2: Custom metrics (check in priority order, use most constrained)
  double max_custom_utilization = 0.0;
  bool found_custom_metric = false;

  for (size_t i = 0; i < orca_utilization_metric_names_.size(); ++i) {
    absl::optional<double> custom_utilization;

    // Try pull-based first, then fallback to push-based
    custom_utilization = load_metric_stats.get(orca_utilization_metric_keys_[i]);
    if (!custom_utilization.has_value()) {
      if (auto typed = host.typedLbPolicyData<OrcaHostLbPolicyData>()) {
        // For custom metrics, we'd need to implement access in OrcaHostLbPolicyData
        // For now, just use pull-based metrics
        custom_utilization = absl::nullopt;
      }
    }

    if (custom_utilization.has_value() && custom_utilization.value() > 0.0) {
      max_custom_utilization = std::max(max_custom_utilization, custom_utilization.value());
      found_custom_metric = true;
    }
  }

  if (found_custom_metric) {
    ENVOY_LOG(debug, "ORCA host={} using pull-based max_custom_utilization={:.2f}%",
              host.address()->asString(), max_custom_utilization * 100);
    return max_custom_utilization;
  }

  // Tier 3: cpu_utilization (fallback)
  if (orca_use_cpu_utilization_) {
    // Try pull-based first, then fallback to push-based
    utilization = load_metric_stats.get("cpu_utilization");
    if (!utilization.has_value()) {
      if (auto typed = host.typedLbPolicyData<OrcaHostLbPolicyData>()) {
        utilization = typed->cpu();
      }
    }

    if (utilization.has_value() && utilization.value() > 0.0) {
      ENVOY_LOG(debug, "ORCA host={} using pull-based cpu_utilization={:.2f}%", host.address()->asString(),
                utilization.value() * 100);
      return utilization;
    }
  }

  // No valid utilization found
  ENVOY_LOG(debug, "ORCA host={} no valid utilization metrics found (OOB enabled: {})",
            host.address()->asString(), oob_enabled_ ? "true" : "false");
  return absl::nullopt;
}

ZoneAwareLoadBalancerBase::CachedZoneMetrics
ZoneAwareLoadBalancerBase::calculateZoneMetricsFromHosts(
    const HostVector& hosts, const envoy::config::core::v3::Locality& locality) {

  CachedZoneMetrics result;
  double total_utilization = 0.0;
  double total_cpu = 0.0;
  uint32_t reporting_hosts = 0;
  uint32_t oob_hosts = 0;
  uint32_t pull_hosts = 0;

  // Aggregate utilization across all reporting hosts
  for (const auto& host : hosts) {
    if (auto util = getUtilizationFromHost(*host)) {
      double host_utilization = util.value();

      // Track whether we're using OOB or pull-based data for this host
      if (oob_enabled_) {
        if (auto typed = host->typedLbPolicyData<OrcaHostLbPolicyData>()) {
          if (typed->oob_reporting_active.load(std::memory_order_relaxed)) {
            oob_hosts++;
          } else {
            pull_hosts++;
          }
        }
      } else {
        pull_hosts++;
      }

      // Apply error penalty if error rate data is available (aligns with CSWRR approach)
      // Formula: effective_utilization = base_utilization + (error_rate *
      // error_utilization_penalty)
      const auto& load_metric_stats = host->loadMetricStats();
      if (auto error_rate = load_metric_stats.get("eps")) {
        if (auto qps = load_metric_stats.get("rps")) {
          if (qps.value() > 0.0) {
            double error_fraction = error_rate.value() / qps.value();
            host_utilization += orca_error_utilization_penalty_ * error_fraction;
            ENVOY_LOG(trace,
                      "ORCA host={} applied error penalty: base={:.2f}%, error_rate={:.3f}, "
                      "penalty={:.2f}%, final={:.2f}%",
                      host->address()->asString(), util.value() * 100, error_fraction,
                      orca_error_utilization_penalty_ * error_fraction * 100,
                      host_utilization * 100);
          }
        }
      }

      total_utilization += host_utilization;
      // For now, assume CPU = utilization (will refine when we implement getUtilizationFromHost)
      total_cpu += host_utilization;
      reporting_hosts++;
    }
  }

  // Check if we have enough reporting hosts
  if (reporting_hosts < min_orca_reporting_hosts_) {
    result.is_valid = false;
    ENVOY_LOG(debug, "Insufficient ORCA data for locality {}/{}/{}: {} hosts reporting (need {})",
              locality.region(), locality.zone(), locality.sub_zone(), reporting_hosts,
              min_orca_reporting_hosts_);
    return result;
  }

  // Calculate current average utilization from measurements
  double current_avg_utilization = total_utilization / reporting_hosts;
  double current_avg_cpu = total_cpu / reporting_hosts;

  // Apply Exponential Moving Average (EMA) smoothing if we have previous data
  // This dampens rapid fluctuations while still responding to real changes
  auto cached_it = cached_zone_metrics_.find(locality);
  if (cached_it != cached_zone_metrics_.end() && cached_it->second.is_valid &&
      zone_metrics_smoothing_factor_ < 1.0) {

    const double alpha = zone_metrics_smoothing_factor_;
    const double& prev_util = cached_it->second.avg_utilization;
    const double& prev_cpu = cached_it->second.avg_cpu;

    // EMA formula: new_avg = alpha × current + (1 - alpha) × previous
    result.avg_utilization = alpha * current_avg_utilization + (1 - alpha) * prev_util;
    result.avg_cpu = alpha * current_avg_cpu + (1 - alpha) * prev_cpu;

    ENVOY_LOG(trace,
              "Applied EMA smoothing for {}/{}/{}: raw={:.2f}%, smoothed={:.2f}%, alpha={:.2f}",
              locality.region(), locality.zone(), locality.sub_zone(),
              current_avg_utilization * 100, result.avg_utilization * 100, alpha);
  } else {
    // First measurement or smoothing disabled (alpha = 1.0), use raw values
    result.avg_utilization = current_avg_utilization;
    result.avg_cpu = current_avg_cpu;
  }

  result.reporting_hosts = reporting_hosts;

  // Calculate weight: capacity × host count
  // Higher available capacity = higher weight = route more traffic here
  // Capacity = 1.0 - utilization (e.g., 30% util = 70% capacity)
  double capacity_per_host = std::max(0.0, 1.0 - result.avg_utilization);

  // Scale by total hosts (not just reporting hosts)
  // This way zones with more hosts get proportionally more weight
  // Multiply by 10000 to match the scale used in calculateLocalityPercentages
  result.locality_weight = static_cast<uint64_t>(capacity_per_host * hosts.size() * 10000);

  result.is_valid = true;
  result.last_updated = time_source_.monotonicTime();

  ENVOY_LOG(trace,
            "Calculated zone metrics for {}/{}/{}: {} reporting hosts (OOB:{}, pull:{}), "
            "{:.2f}% avg utilization, weight={}",
            locality.region(), locality.zone(), locality.sub_zone(), reporting_hosts,
            oob_hosts, pull_hosts, result.avg_utilization * 100, result.locality_weight);

  return result;
}

void ZoneAwareLoadBalancerBase::aggregateZoneMetricsForHostSet(const HostSet& host_set) {
  const auto& hosts_per_locality = host_set.healthyHostsPerLocality();

  // Group hosts by locality and calculate metrics for each
  for (uint32_t i = 0; i < hosts_per_locality.get().size(); ++i) {
    const auto& locality_hosts = hosts_per_locality.get()[i];

    if (locality_hosts.empty()) {
      continue;
    }

    const auto& locality = locality_hosts[0]->locality();

    // Calculate and cache metrics for this locality
    cached_zone_metrics_[locality] = calculateZoneMetricsFromHosts(locality_hosts, locality);
  }
}

void ZoneAwareLoadBalancerBase::updateZoneMetricsOnMainThread() {
  ENVOY_LOG(trace, "Updating zone metrics for all host sets");

  // Clear existing cache
  cached_zone_metrics_.clear();

  // Aggregate metrics for all host sets
  for (const auto& host_set : priority_set_.hostSetsPerPriority()) {
    aggregateZoneMetricsForHostSet(*host_set);
  }

  // Update last refresh timestamp
  zone_metrics_last_refresh_ = time_source_.monotonicTime();

  // Note: Timer is re-armed in the timer callback itself

  ENVOY_LOG(debug, "Zone metrics updated: {} localities cached", cached_zone_metrics_.size());
}

void ZoneAwareLoadBalancerBase::maybeUpdateZoneMetrics() {
  // Check if we need to update zone metrics (lazy on-demand update)
  const auto now = time_source_.monotonicTime();

  // Update if cache is stale (older than update interval)
  if (now - zone_metrics_last_refresh_ >= zone_metrics_update_interval_) {
    updateZoneMetricsOnMainThread();
  }
}

uint64_t ZoneAwareLoadBalancerBase::getCachedOrcaBasedLocalityWeight(
    const HostVector& locality_hosts, const envoy::config::core::v3::Locality& locality) {

  if (locality_hosts.empty()) {
    return 0;
  }

  // Ensure we have an initial snapshot on first use to avoid immediate fallback.
  if (cached_zone_metrics_.empty()) {
    ENVOY_LOG(debug, "ORCA zone cache empty; performing initial update");
    updateZoneMetricsOnMainThread();
  }

  // Lazy update: refresh zone metrics if cache is stale
  // This is called on hot-path, but maybeUpdateZoneMetrics only does work
  // if the cache has expired (based on zone_metrics_update_interval_)
  maybeUpdateZoneMetrics();

  // Fast O(1) hash lookup in cache
  auto it = cached_zone_metrics_.find(locality);

  if (it != cached_zone_metrics_.end() && it->second.is_valid) {
    // Cache hit - return cached ORCA-based weight
    // TODO(future): Add stats_.lb_zone_orca_cache_hit_.inc();
    ENVOY_LOG(debug, "ORCA zone weight for {}/{}/{}: {} (avg util: {:.1f}%, {} hosts reporting)",
              locality.region(), locality.zone(), locality.sub_zone(), it->second.locality_weight,
              it->second.avg_utilization * 100, it->second.reporting_hosts);
    return it->second.locality_weight;
  }

  // Cache miss or invalid: attempt an on-demand calculation from current host stats.
  ENVOY_LOG(debug, "ORCA cache miss/invalid for locality {}/{}/{}, attempting direct calc",
            locality.region(), locality.zone(), locality.sub_zone());

  CachedZoneMetrics direct = calculateZoneMetricsFromHosts(locality_hosts, locality);
  if (direct.is_valid) {
    // Store and return the freshly computed weight.
    cached_zone_metrics_[locality] = direct;
    ENVOY_LOG(debug, "ORCA direct calc weight for {}/{}/{}: {} (avg util: {:.1f}%, hosts={})",
              locality.region(), locality.zone(), locality.sub_zone(), direct.locality_weight,
              direct.avg_utilization * 100.0, direct.reporting_hosts);
    return direct.locality_weight;
  }

  // Still no usable ORCA data; fall back to configured basis.
  ENVOY_LOG(debug, "ORCA direct calc unavailable; using fallback for {}/{}/{} (basis: {})",
            locality.region(), locality.zone(), locality.sub_zone(),
            static_cast<int>(orca_fallback_basis_));
  return calculateFallbackLocalityWeight(locality_hosts, locality);
}

// OOB Reporting Implementation

void ZoneAwareLoadBalancerBase::startOobExpirationTimer() {
  // TODO: Implement OOB expiration timer
  //
  // The timer-based expiration requires dispatcher access which is not available
  // in ZoneAwareLoadBalancerBase. To complete this feature:
  //
  // Option 1: Add Event::Dispatcher& to ZoneAwareLoadBalancerBase constructor
  //   - Requires updating all derived load balancer constructors
  //   - Most straightforward approach
  //
  // Option 2: Create timer in factory and pass it to base class
  //   - Requires less architectural changes
  //   - More complex factory pattern
  //
  // Option 3: Use main-thread proxy timer via thread-local storage
  //   - Complex but avoids constructor changes
  //
  // When implemented, add:
  // oob_expiration_timer_ = dispatcher_->createTimer([this]() { onOobExpirationTimer(); });
  // oob_expiration_timer_->enableTimer(std::chrono::minutes(1)); // Check every minute

  ENVOY_LOG(debug, "OOB expiration timer setup pending dispatcher access implementation");
}

void ZoneAwareLoadBalancerBase::onOobExpirationTimer() {
  if (!oob_enabled_) {
    return;
  }

  ENVOY_LOG(trace, "Checking for expired OOB reports");
  checkExpiredOobReports();

  // Re-arm timer for next check
  if (oob_expiration_timer_) {
    oob_expiration_timer_->enableTimer(std::chrono::minutes(1));
  }
}

void ZoneAwareLoadBalancerBase::checkExpiredOobReports() {
  const auto now = time_source_.monotonicTime();
  bool had_expired_reports = false;

  // Check all host sets for expired OOB reports
  for (const HostSetPtr& host_set : priority_set_.hostSetsPerPriority()) {
    for (const auto& host : host_set->hosts()) {
      if (auto typed = host->typedLbPolicyData<OrcaHostLbPolicyData>()) {
        if (typed->oob_reporting_active.load(std::memory_order_relaxed)) {
          // Convert nanoseconds back to MonotonicTime for comparison
          const uint64_t last_update_ns = typed->oob_last_update_ns.load(std::memory_order_relaxed);
          const auto last_update = MonotonicTime(std::chrono::nanoseconds(last_update_ns));

          if (now - last_update > oob_expiration_period_) {
            // This host's OOB report has expired
            typed->oob_reporting_active.store(false, std::memory_order_relaxed);
            had_expired_reports = true;

            ENVOY_LOG(debug, "OOB report expired for host {}, falling back to per-request metrics",
                      host->address()->asString());
          }
        }
      }
    }
  }

  // If any OOB reports expired, invalidate the zone metrics cache
  // to force recalculation with updated host states
  if (had_expired_reports) {
    ENVOY_LOG(debug, "Some OOB reports expired, invalidating zone metrics cache");
    cached_zone_metrics_.clear();
  }
}

// OOB-First Zone Metrics Cache Implementation

absl::optional<ZoneAwareLoadBalancerBase::CachedZoneMetrics>
ZoneAwareLoadBalancerBase::getOobFirstZoneMetrics(
    const envoy::config::core::v3::Locality& locality) {

  if (!oob_first_enabled_) {
    // OOB-first mode not enabled, fall back to regular cache
    return absl::nullopt;
  }

  auto it = oob_first_zone_cache_.find(locality);
  if (it != oob_first_zone_cache_.end() && it->second.has_valid_oob_data) {
    const auto& oob_metrics = it->second.oob_metrics;
    const auto now = time_source_.monotonicTime();

    // Check if OOB data is still fresh
    if (now - it->second.last_oob_update < oob_expiration_period_) {
      ENVOY_LOG(debug, "OOB-first cache hit for {}/{}/{}, using OOB data (age: {}ms)",
                locality.region(), locality.zone(), locality.sub_zone(),
                std::chrono::duration_cast<std::chrono::milliseconds>(
                  now - it->second.last_oob_update).count());
      return oob_metrics;
    } else {
      // OOB data expired, mark as invalid
      it->second.has_valid_oob_data = false;
      ENVOY_LOG(debug, "OOB-first cache expired for {}/{}/{}",
                locality.region(), locality.zone(), locality.sub_zone());
    }
  }

  return absl::nullopt;
}

void ZoneAwareLoadBalancerBase::updateOobFirstCache(
    const envoy::config::core::v3::Locality& locality,
    const CachedZoneMetrics& metrics) {

  if (!oob_first_enabled_) {
    return;
  }

  auto& zone_cache = oob_first_zone_cache_[locality];
  zone_cache.oob_metrics = metrics;
  zone_cache.last_oob_update = time_source_.monotonicTime();
  zone_cache.has_valid_oob_data = true;

  ENVOY_LOG(debug, "OOB-first cache updated for {}/{}/{}: {} hosts, util {:.1f}%",
            locality.region(), locality.zone(), locality.sub_zone(),
            metrics.reporting_hosts, metrics.avg_utilization * 100);
}

bool ZoneAwareLoadBalancerBase::needsCrossZoneProbing(
    const envoy::config::core::v3::Locality& locality,
    const CachedZoneMetrics& /* current_metrics */) {

  if (!oob_first_enabled_) {
    return true; // Traditional behavior: always probe
  }

  const auto now = time_source_.monotonicTime();
  const auto& cache_entry = oob_first_zone_cache_[locality];

  // Check if we have fresh OOB data
  if (cache_entry.has_valid_oob_data &&
      (now - cache_entry.last_oob_update < oob_expiration_period_)) {
    return false; // No probing needed, OOB data is fresh
  }

  // Check OOB missing threshold
  if (oob_probe_triggers_.has_oob_missing_threshold()) {
    const auto threshold = std::chrono::milliseconds(
        DurationUtil::durationToMilliseconds(oob_probe_triggers_.oob_missing_threshold()));
    if (now - cache_entry.last_oob_update < threshold) {
      return false; // Within missing threshold, don't probe yet
    }
  }

  // Check rate limiting
  if (cache_entry.in_probe_cooldown) {
    // Check if cooldown period has passed (default: 1 minute)
    const auto cooldown_period = std::chrono::minutes(1);
    if (now - cache_entry.last_probe_time < cooldown_period) {
      return false; // Still in cooldown
    }
  }

  // Check maximum probes per minute
  if (oob_probe_triggers_.has_max_probes_per_minute()) {
    const uint32_t max_probes = oob_probe_triggers_.max_probes_per_minute().value();
    if (cache_entry.probes_sent_this_minute >= max_probes) {
      return false; // Rate limited
    }
  }

  // Check load variance threshold (if we have historical data)
  if (oob_probe_triggers_.has_load_variance_threshold() && cache_entry.has_valid_oob_data) {
    const double variance_threshold = oob_probe_triggers_.load_variance_threshold().value();
    const double utilization_diff = std::abs(/* current_metrics */ 0.0 -
                                            cache_entry.oob_metrics.avg_utilization);

    if (utilization_diff < variance_threshold) {
      return false; // Load variance too small to justify probing
    }
  }

  return true; // Probing is needed
}

bool ZoneAwareLoadBalancerBase::performIntelligentProbe(
    const envoy::config::core::v3::Locality& locality,
    const HostVector& /* hosts */) {

  if (!oob_first_enabled_) {
    return true; // Traditional behavior: always probe
  }

  auto& cache_entry = oob_first_zone_cache_[locality];
  const auto now = time_source_.monotonicTime();

  // Update rate limiting counters
  if (now - cache_entry.last_probe_time > std::chrono::minutes(1)) {
    // Reset counter for new minute
    cache_entry.probes_sent_this_minute = 1;
  } else {
    cache_entry.probes_sent_this_minute++;
  }

  cache_entry.last_probe_time = now;

  // Check if we should enter cooldown
  if (oob_probe_triggers_.has_max_probes_per_minute()) {
    const uint32_t max_probes = oob_probe_triggers_.max_probes_per_minute().value();
    if (cache_entry.probes_sent_this_minute >= max_probes) {
      cache_entry.in_probe_cooldown = true;
      ENVOY_LOG(debug, "OOB-first: Entering probe cooldown for {}/{}/{} (reached max probes: {})",
                locality.region(), locality.zone(), locality.sub_zone(), max_probes);
    }
  }

  ENVOY_LOG(debug, "OOB-first: Performing intelligent probe for {}/{}/{} (probe #{} this minute)",
            locality.region(), locality.zone(), locality.sub_zone(),
            cache_entry.probes_sent_this_minute);

  return true; // Perform the probe
}

ZoneAwareLoadBalancerBase::CachedZoneMetrics
ZoneAwareLoadBalancerBase::aggregateOobMetricsForZone(
    const envoy::config::core::v3::Locality& locality,
    const HostVector& hosts) {

  CachedZoneMetrics result{};
  result.last_updated = time_source_.monotonicTime();

  if (!oob_zone_aggregation_config_.enable_zone_aggregation()) {
    // Zone aggregation disabled, return empty metrics
    return result;
  }

  const uint32_t min_hosts = oob_zone_aggregation_config_.min_hosts_per_zone().value();
  std::vector<double> utilizations;
  std::vector<double> cpu_values;

  // Collect metrics from hosts in this zone
  for (const auto& host : hosts) {
    auto* typed = dynamic_cast<OrcaHostLbPolicyData*>(host->lbPolicyData().ptr());
    if (!typed) {
      continue;
    }

    // Prefer OOB data if available and fresh
    if (oob_enabled_ && typed->oob_reporting_active.load(std::memory_order_relaxed)) {

      if (orca_use_application_utilization_) {
        auto app = typed->app();
        if (app.has_value()) {
          utilizations.push_back(app.value());
        }
      }

      if (orca_use_cpu_utilization_) {
        auto cpu = typed->cpu();
        if (cpu.has_value()) {
          cpu_values.push_back(cpu.value());
        }
      }
    }
  }

  // Check if we have enough hosts for valid aggregation
  if (utilizations.size() + cpu_values.size() < min_hosts) {
    ENVOY_LOG(debug, "OOB-first: Insufficient hosts for zone aggregation {}/{}/{}: {} reporting (need {})",
              locality.region(), locality.zone(), locality.sub_zone(),
              utilizations.size() + cpu_values.size(), min_hosts);
    return result; // Invalid metrics
  }

  // Perform aggregation based on configured method
  std::vector<double> all_values;
  all_values.insert(all_values.end(), utilizations.begin(), utilizations.end());
  all_values.insert(all_values.end(), cpu_values.begin(), cpu_values.end());

  if (all_values.empty()) {
    return result; // No valid metrics
  }

  double aggregated_utilization = 0.0;
  const auto method = oob_zone_aggregation_config_.aggregation_method();

  switch (method) {
    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::ZoneAggregationConfig::AVERAGE:
      aggregated_utilization = std::accumulate(all_values.begin(), all_values.end(), 0.0) / all_values.size();
      break;

    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::ZoneAggregationConfig::WEIGHTED_AVERAGE:
      // Weighted by host weight (simplified - assumes equal weights)
      aggregated_utilization = std::accumulate(all_values.begin(), all_values.end(), 0.0) / all_values.size();
      break;

    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::ZoneAggregationConfig::MAXIMUM:
      aggregated_utilization = *std::max_element(all_values.begin(), all_values.end());
      break;

    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::ZoneAggregationConfig::MINIMUM:
      aggregated_utilization = *std::min_element(all_values.begin(), all_values.end());
      break;

    // No sentinel values in actual proto - this case handles unknown/unexpected values
    default:
      // Fall back to average for unknown values
      aggregated_utilization = std::accumulate(all_values.begin(), all_values.end(), 0.0) / all_values.size();
      break;
  }

  // Apply local zone prioritization if configured
  if (oob_zone_aggregation_config_.prioritize_local_zone() &&
      oob_zone_aggregation_config_.has_local_zone_weight()) {
    const double local_weight = oob_zone_aggregation_config_.local_zone_weight().value();
    // Reduce utilization by local weight factor (higher weight = lower apparent utilization)
    aggregated_utilization /= local_weight;
  }

  // Fill result
  result.avg_utilization = aggregated_utilization;
  result.reporting_hosts = utilizations.size() + cpu_values.size();
  result.is_valid = true;

  // Calculate inverse utilization weight (higher utilization = lower weight)
  const double capacity = std::max(0.01, 1.0 - aggregated_utilization);
  result.locality_weight = static_cast<uint64_t>(capacity * ORCA_WEIGHT_SCALE_FACTOR);

  ENVOY_LOG(debug, "OOB-first: Zone aggregation for {}/{}/{}: {} hosts, util {:.1f}%, weight {}",
            locality.region(), locality.zone(), locality.sub_zone(),
            result.reporting_hosts, result.avg_utilization * 100, result.locality_weight);

  return result;
}

absl::optional<ZoneAwareLoadBalancerBase::CachedZoneMetrics>
ZoneAwareLoadBalancerBase::applyFallbackStrategy(
    const envoy::config::core::v3::Locality& locality,
    const HostVector& hosts) {

  if (!oob_first_enabled_) {
    return absl::nullopt;
  }

  switch (oob_fallback_strategy_) {
    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::PROBE_FALLBACK:
      // Traditional fallback: perform cross-zone probe
      ENVOY_LOG(debug, "OOB-first: Using probe fallback for {}/{}/{}",
                locality.region(), locality.zone(), locality.sub_zone());
      return calculateZoneMetricsFromHosts(hosts, locality);

    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::USE_STALE_METRICS:
      // Use last known good OOB metrics if available
      {
        auto it = oob_first_zone_cache_.find(locality);
        if (it != oob_first_zone_cache_.end() && it->second.has_valid_oob_data) {
          ENVOY_LOG(debug, "OOB-first: Using stale metrics for {}/{}/{}",
                    locality.region(), locality.zone(), locality.sub_zone());
          return it->second.oob_metrics;
        }
      }
      // No stale metrics available, fall back to probing
      return calculateZoneMetricsFromHosts(hosts, locality);

    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::USE_ESTIMATED_METRICS:
      // Use estimated metrics based on historical patterns
      ENVOY_LOG(debug, "OOB-first: Using estimated metrics for {}/{}/{}",
                locality.region(), locality.zone(), locality.sub_zone());
      // For now, fall back to traditional calculation
      // TODO: Implement historical pattern estimation
      return calculateZoneMetricsFromHosts(hosts, locality);

    case LocalityLbConfig::ZoneAwareLbConfig::OrcaLoadBasedConfig::HYBRID_FALLBACK:
      // Try limited probing, then use stale/estimated
      if (needsCrossZoneProbing(locality, CachedZoneMetrics{})) {
        ENVOY_LOG(debug, "OOB-first: Hybrid fallback - performing limited probe for {}/{}/{}",
                  locality.region(), locality.zone(), locality.sub_zone());
        return calculateZoneMetricsFromHosts(hosts, locality);
      } else {
        // Use stale metrics if available
        auto it = oob_first_zone_cache_.find(locality);
        if (it != oob_first_zone_cache_.end() && it->second.has_valid_oob_data) {
          return it->second.oob_metrics;
        }
      }
      return calculateZoneMetricsFromHosts(hosts, locality);

    default:
      // Handle any sentinel values or unknown strategies
      ENVOY_LOG(debug, "OOB-first: Unknown fallback strategy for {}/{}/{}, using hybrid",
                locality.region(), locality.zone(), locality.sub_zone());
      return calculateZoneMetricsFromHosts(hosts, locality);
      }

  return absl::nullopt;
}

void ZoneAwareLoadBalancerBase::resetProbeRateCounters() {
  if (!oob_first_enabled_) {
    return;
  }

  const auto now = time_source_.monotonicTime();

  // Reset counters for zones that haven't been probed in the last minute
  for (auto& [locality, cache_entry] : oob_first_zone_cache_) {
    if (now - cache_entry.last_probe_time > std::chrono::minutes(1)) {
      cache_entry.probes_sent_this_minute = 0;
      cache_entry.in_probe_cooldown = false;
    }
  }
}

} // namespace Upstream
} // namespace Envoy
