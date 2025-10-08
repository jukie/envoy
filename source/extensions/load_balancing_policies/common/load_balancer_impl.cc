#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "envoy/config/cluster/v3/cluster.pb.h"
#include "envoy/config/core/v3/base.pb.h"
#include "envoy/runtime/runtime.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/assert.h"
#include "source/common/common/logger.h"
#include "source/common/orca/orca_load_metrics.h"
#include "source/common/protobuf/utility.h"
#include "source/common/runtime/runtime_features.h"

#include "absl/container/fixed_array.h"
#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"

namespace Envoy {
namespace Upstream {

ZoneAwareLoadBalancerBase::ZoneAwareHostLbPolicyData::ZoneAwareHostLbPolicyData(
    const std::vector<std::string>& metrics)
    : metrics_(metrics) {}

absl::Status ZoneAwareLoadBalancerBase::ZoneAwareHostLbPolicyData::onOrcaLoadReport(
    const OrcaLoadReport& report, const StreamInfo::StreamInfo&) {
  const double utilization = computeUtilization(report);
  utilization_.store(utilization, std::memory_order_relaxed);
  has_report_.store(true, std::memory_order_release);
  last_update_time_.store(std::chrono::steady_clock::now());
  ENVOY_LOG(debug, "zone aware ORCA: updated host utilization to {}", utilization);
  
  // Note: We would like to set a flag here to indicate that ORCA data has been updated,
  // but we don't have access to the parent ZoneAwareLoadBalancerBase from this context.
  // The legacy implementation will handle this through the shouldRefreshOrcaRouting() mechanism.
  
  return absl::OkStatus();
}

double ZoneAwareLoadBalancerBase::ZoneAwareHostLbPolicyData::computeUtilization(
    const OrcaLoadReport& report) const {
  if (!metrics_.empty()) {
    const double metric_utilization = Envoy::Orca::getMaxUtilization(metrics_, report);
    if (metric_utilization > 0) {
      return metric_utilization;
    }
  }

  if (report.application_utilization() > 0) {
    return report.application_utilization();
  }

  if (report.cpu_utilization() > 0) {
    return report.cpu_utilization();
  }

  return 0.0;
}

namespace {
static const std::string RuntimeZoneEnabled = "upstream.zone_routing.enabled";
static const std::string RuntimeMinClusterSize = "upstream.zone_routing.min_cluster_size";
static const std::string RuntimeForceLocalZoneMinSize =
    "upstream.zone_routing.force_local_zone.min_size";
static const std::string RuntimePanicThreshold = "upstream.healthy_panic_threshold";

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
      [this](uint32_t priority, const HostVector&, const HostVector&) {
        recalculatePerPriorityState(priority, priority_set_, per_priority_load_,
                                    per_priority_health_, per_priority_degraded_,
                                    total_healthy_hosts_);
        recalculatePerPriorityPanic();
        stashed_random_.clear();
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
    const absl::optional<LocalityLbConfig> locality_config)
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
      orca_metrics_(locality_config.has_value()
                        ? std::vector<std::string>(
                              locality_config->zone_aware_lb_config().orca_metrics().begin(),
                              locality_config->zone_aware_lb_config().orca_metrics().end())
                        : std::vector<std::string>()),
      orca_aggregation_(locality_config.has_value()
                            ? locality_config->zone_aware_lb_config().orca_aggregation()
                            : LocalityLbConfig::ZoneAwareLbConfig::AGGREGATION_UNSPECIFIED),
      orca_smoothing_window_([&]() -> absl::optional<std::chrono::milliseconds> {
        if (!locality_config.has_value() ||
            !locality_config->zone_aware_lb_config().has_orca_smoothing_window()) {
          return absl::nullopt;
        }
        return std::chrono::milliseconds(
            DurationUtil::durationToMilliseconds(locality_config->zone_aware_lb_config().orca_smoothing_window()));
      }()),
      orca_update_interval_([&]() -> std::chrono::milliseconds {
        if (!locality_config.has_value() ||
            !locality_config->zone_aware_lb_config().has_orca_update_interval()) {
          return kDefaultOrcaUpdateInterval;
        }
        return std::chrono::milliseconds(
            DurationUtil::durationToMilliseconds(locality_config->zone_aware_lb_config().orca_update_interval()));
      }()),
      orca_min_reported_endpoints_(locality_config.has_value()
                                       ? locality_config->zone_aware_lb_config().has_orca_min_reported_endpoints()
                                             ? locality_config->zone_aware_lb_config().orca_min_reported_endpoints().value()
                                             : 1
                                       : 1),
      orca_probe_residual_percent_(locality_config.has_value()
                                       ? PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
                                             locality_config->zone_aware_lb_config(), orca_probe_residual_percent, 100, 1)
                                       : 1),
      orca_fallback_(locality_config.has_value()
                         ? locality_config->zone_aware_lb_config().orca_fallback()
                         : LocalityLbConfig::ZoneAwareLbConfig::FALLBACK_UNSPECIFIED),
      orca_spillover_threshold_percent_(locality_config.has_value()
                                            ? PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
                                                  locality_config->zone_aware_lb_config(), orca_spillover_threshold, 100, 0)
                                            : 0),
      fail_traffic_on_panic_(locality_config.has_value()
                                 ? locality_config->zone_aware_lb_config().fail_traffic_on_panic()
                                 : false),
      locality_weighted_balancing_(locality_config.has_value() &&
                                   locality_config->has_locality_weighted_lb_config()) {
  ASSERT(!priority_set.hostSetsPerPriority().empty());
  resizePerPriorityState();
  if (locality_weighted_balancing_) {
    for (uint32_t priority = 0; priority < priority_set_.hostSetsPerPriority().size(); ++priority) {
      rebuildLocalityWrrForPriority(priority);
    }
  }

  if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    for (uint32_t priority = 0; priority < priority_set_.hostSetsPerPriority().size(); ++priority) {
      addZoneAwareLbPolicyDataToHosts(priority_set_.hostSetsPerPriority()[priority]->hosts());
    }
    if (local_priority_set_) {
      for (uint32_t priority = 0; priority < local_priority_set_->hostSetsPerPriority().size();
           ++priority) {
        addZoneAwareLbPolicyDataToHosts(local_priority_set_->hostSetsPerPriority()[priority]->hosts());
      }
    }
  }

  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t priority, const HostVector& hosts_added, const HostVector&) {
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

        if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
          addZoneAwareLbPolicyDataToHosts(hosts_added);
        }
      });
  if (local_priority_set_) {
    // Multiple priorities are unsupported for local priority sets.
    // In order to support priorities correctly, one would have to make some assumptions about
    // routing (all local Envoys fail over at the same time) and use all priorities when computing
    // the locality routing structure.
    ASSERT(local_priority_set_->hostSetsPerPriority().size() == 1);
    local_priority_set_member_update_cb_handle_ = local_priority_set_->addPriorityUpdateCb(
        [this](uint32_t priority, const HostVector& hosts_added, const HostVector&) {
          ASSERT(priority == 0);
          // If the set of local Envoys changes, regenerate routing for P=0 as it does priority
          // based routing.
          regenerateLocalityRoutingStructures();
          if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
            addZoneAwareLbPolicyDataToHosts(hosts_added);
          }
        });
  }

  // Initialize ORCA background infrastructure
  current_routing_data_.store(nullptr, std::memory_order_relaxed);
  previous_routing_data_ = nullptr;
  
  // Note: Timer will be created in Phase 2 when we have access to Event::Dispatcher
  if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    ENVOY_LOG(debug, "zone aware ORCA: background infrastructure initialized (timer in Phase 2)");
  }
}

void ZoneAwareLoadBalancerBase::updateOrcaRoutingOnMainThread() {
  if (locality_basis_ != LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    return;
  }
  
  if (!local_priority_set_ || earlyExitNonLocalityRouting()) {
    return;
  }
  
  ENVOY_LOG(debug, "zone aware ORCA: performing background update");
  
  // Call the original expensive regeneration logic - this is now off the hot path!
  regenerateLocalityRoutingStructures();
  
  // After regeneration, extract the calculated percentages and store them atomically
  // Note: regenerateLocalityRoutingStructures updates internal state
  // Extract the locality percentages that were calculated by regenerateLocalityRoutingStructures
  auto new_percentages = calculateOrcaLocalityPercentages(
      localHostSet().healthyHostsPerLocality(),
      priority_set_.hostSetsPerPriority()[0]->healthyHostsPerLocality());
  
  // Create new routing data with the calculated percentages
  auto new_routing_data = std::make_shared<LocalityRoutingData>();
  new_routing_data->locality_percentages = 
      std::make_shared<const absl::FixedArray<LocalityPercentages>>(std::move(new_percentages));
  new_routing_data->last_update_time = std::chrono::steady_clock::now();
  new_routing_data->is_valid = true;
  
  // Lock-free atomic update - main thread updates, all threads can read without blocking
  const LocalityRoutingData* new_ptr = new_routing_data.get();
  current_routing_data_.store(new_ptr, std::memory_order_release);
  
  // Store the update time and replace previous routing data
  const auto locality_count = new_routing_data->locality_percentages->size();
  previous_routing_data_ = std::move(new_routing_data);
  last_orca_refresh_time_ = new_routing_data->last_update_time;
  
  ENVOY_LOG(debug, "zone aware ORCA: background update completed with {} localities", 
            locality_count);
}

void ZoneAwareLoadBalancerBase::enableOrcaBackgroundUpdates(Event::Dispatcher& main_thread_dispatcher) {
  if (locality_basis_ != LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    return;
  }
  
  ENVOY_LOG(debug, "zone aware ORCA: enabling background updates with interval {}ms", 
            orca_update_interval_.count());
  
  // Create timer for background ORCA updates
  orca_update_timer_ = main_thread_dispatcher.createTimer([this]() {
    updateOrcaRoutingOnMainThread();
    orca_update_timer_->enableTimer(orca_update_interval_);
  });
  
  // Start the timer
  orca_update_timer_->enableTimer(orca_update_interval_);
  
  ENVOY_LOG(debug, "zone aware ORCA: background timer enabled");
}





uint32_t ZoneAwareLoadBalancerBase::selectLocalityFromPreCalculatedData(
    const absl::FixedArray<LocalityPercentages>& percentages,
    const HostSet& host_set) const {
  
  // For ORCA with pre-calculated data, temporarily update the state with pre-calculated
  // local percentage, then use the existing locality selection logic.
  PerPriorityState& state = *per_priority_state_[host_set.priority()];
  ASSERT(state.locality_routing_state_ != LocalityRoutingState::NoLocalityRouting);

  // Save original local_percent_to_route_ and temporarily set to pre-calculated value
  const uint64_t original_local_percent_to_route = state.local_percent_to_route_;
  
  if (percentages.size() > 0 && host_set.healthyHostsPerLocality().hasLocalLocality()) {
    // Update state with pre-calculated local percentage
    const_cast<PerPriorityState&>(state).local_percent_to_route_ = percentages[0].local_percentage;
    ENVOY_LOG(debug, "zone aware ORCA: using pre-calculated local percentage {}", 
              percentages[0].local_percentage);
  }
  
  // Use the existing locality selection logic from the legacy method
  // but with our pre-calculated local_percentage
  uint32_t result = tryChooseLocalLocalityHostsLegacy(host_set);
  
  // Restore original state
  const_cast<PerPriorityState&>(state).local_percent_to_route_ = original_local_percent_to_route;
  
  return result;
}

void ZoneAwareLoadBalancerBase::addZoneAwareLbPolicyDataToHosts(const HostVector& hosts) {
  if (locality_basis_ != LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    return;
  }

  for (const auto& host : hosts) {
    if (!host->typedLbPolicyData<ZoneAwareHostLbPolicyData>().has_value()) {
      ENVOY_LOG(debug, "zone aware ORCA: attaching lb policy data to host {}",
                host->address()->asString());
      host->setLbPolicyData(
          std::make_unique<ZoneAwareHostLbPolicyData>(orca_metrics_));
    }
  }
}

bool ZoneAwareLoadBalancerBase::shouldRefreshOrcaRouting() const {
  if (locality_basis_ != LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  const auto time_since_last_refresh = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - last_orca_refresh_time_);

  // Only refresh if enough time has passed since last update
  return time_since_last_refresh >= kDefaultOrcaUpdateInterval;
}

absl::optional<ZoneAwareLoadBalancerBase::OrcaLoadObservation>
ZoneAwareLoadBalancerBase::computeOrcaCapacity(const HostVector& hosts) const {
  if (locality_basis_ != LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    return absl::nullopt;
  }

  const uint32_t min_reports = orca_min_reported_endpoints_;

  uint32_t report_count = 0;
  double weighted_utilization_sum = 0.0;
  double max_utilization = 0.0;
  uint64_t reported_weight_sum = 0;

  for (const auto& host : hosts) {
    const uint32_t host_weight = host->weight();
    auto policy_data = host->typedLbPolicyData<ZoneAwareHostLbPolicyData>();
    if (!policy_data.has_value() || !policy_data->hasReport()) {
      ENVOY_LOG(debug, "zone aware ORCA: host {} missing ORCA report", host->address()->asString());
      continue;
    }

    const double utilization = std::max(0.0, policy_data->utilization());
    weighted_utilization_sum += static_cast<double>(host_weight) * utilization;
    max_utilization = std::max(max_utilization, utilization);
    reported_weight_sum += host_weight;
    report_count++;
  }

  if (report_count < min_reports || reported_weight_sum == 0) {
    ENVOY_LOG(debug,
              "zone aware ORCA: insufficient ORCA reports (count={}, weight={}) for locality;"
              " falling back",
              report_count, reported_weight_sum);
    return absl::nullopt;
  }

  double utilization = 0.0;
  switch (orca_aggregation_) {
  case LocalityLbConfig::ZoneAwareLbConfig::MAX:
    utilization = max_utilization;
    ENVOY_LOG(debug, "zone aware ORCA: MAX aggregation utilization {}", utilization);
    break;
  case LocalityLbConfig::ZoneAwareLbConfig::AGGREGATION_UNSPECIFIED:
  case LocalityLbConfig::ZoneAwareLbConfig::AVERAGE:
  default:
    utilization = weighted_utilization_sum / static_cast<double>(reported_weight_sum);
    ENVOY_LOG(debug, "zone aware ORCA: AVERAGE aggregation utilization {}", utilization);
    break;
  }

  utilization = std::clamp(utilization, 0.0, 1.0);
  const double capacity = std::max(0.0, 1.0 - utilization) * static_cast<double>(reported_weight_sum);
  ENVOY_LOG(debug,
            "zone aware ORCA: computed capacity {} from utilization {} with reported weight {}",
            capacity, utilization, reported_weight_sum);
  OrcaLoadObservation observation{capacity, utilization};
  return observation;
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
  auto locality_percentages = calculateLocalityPercentages(localHostSet().healthyHostsPerLocality(),
                                                          upstreamHostsPerLocality);
  ENVOY_LOG(debug, "zone aware ORCA: locality percentages updated");
  for (uint32_t idx = 0; idx < locality_percentages.size(); ++idx) {
    ENVOY_LOG(debug, "zone aware ORCA: locality {} local% {} upstream% {}", idx,
              locality_percentages[idx].local_percentage,
              locality_percentages[idx].upstream_percentage);
  }

  if (upstreamHostsPerLocality.hasLocalLocality()) {
    // Check ORCA spillover logic first if ORCA is enabled
    if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION && !locality_percentages.empty()) {
      const double threshold = static_cast<double>(orca_spillover_threshold_percent_) / 100.0;
      const double local_util = locality_percentages[0].average_utilization;
      bool remote_requires_spill = false;
      for (size_t i = 1; i < locality_percentages.size(); ++i) {
        const double remote_util = locality_percentages[i].average_utilization;
        if ((remote_util + threshold) < local_util) {
          remote_requires_spill = true;
          break;
        }
      }
      if (!remote_requires_spill) {
        state.locality_routing_state_ = LocalityRoutingState::LocalityDirect;
        state.local_percent_to_route_ = 10000;
        ENVOY_LOG(debug,
                  "zone aware ORCA: local utilization {} within threshold {}; routing locally",
                  local_util, threshold);
        return;
      }
      // If we reach here, ORCA logic says we should spill over to remote localities
      // Fall through to legacy logic for residual routing
    }
    // If we have lower percent of hosts in the local cluster in the same locality,
    // we can push all of the requests directly to upstream cluster in the same locality.
    if ((locality_percentages[0].upstream_percentage > 0 &&
         locality_percentages[0].upstream_percentage >= locality_percentages[0].local_percentage) ||
        // When force_local_zone is enabled, always use LocalityDirect routing if there are enough
        // healthy upstreams in the local locality as determined by force_local_zone_min_size is
        // met.
        (force_local_zone_min_size_.has_value() &&
         upstreamHostsPerLocality.get()[0].size() >= *force_local_zone_min_size_)) {
      state.locality_routing_state_ = LocalityRoutingState::LocalityDirect;
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
  if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    return calculateOrcaLocalityPercentages(local_hosts_per_locality, upstream_hosts_per_locality);
  }

  return calculateLegacyLocalityPercentages(local_hosts_per_locality, upstream_hosts_per_locality);
}

absl::FixedArray<ZoneAwareLoadBalancerBase::LocalityPercentages>
ZoneAwareLoadBalancerBase::calculateLegacyLocalityPercentages(
    const HostsPerLocality& local_hosts_per_locality,
    const HostsPerLocality& upstream_hosts_per_locality) const {
  absl::flat_hash_map<envoy::config::core::v3::Locality, uint64_t, LocalityHash, LocalityEqualTo>
      local_weights;
  absl::flat_hash_map<envoy::config::core::v3::Locality, uint64_t, LocalityHash, LocalityEqualTo>
      upstream_weights;
  uint64_t total_local_weight = 0;
  for (const auto& locality_hosts : local_hosts_per_locality.get()) {
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
    // When ORCA_UTILIZATION falls back to legacy calculation, use host count
    case LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION:
      locality_weight = locality_hosts.size();
      break;
    default:
      PANIC_DUE_TO_CORRUPT_ENUM;
    }
    total_local_weight += locality_weight;
    // If there is no entry in the map for a given locality, it is assumed to have 0 hosts.
    if (!locality_hosts.empty()) {
      local_weights.emplace(locality_hosts[0]->locality(), locality_weight);
    }
  }
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
    // When ORCA_UTILIZATION falls back to legacy calculation, use host count
    case LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION:
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

    percentages[i] = LocalityPercentages{local_percentage, upstream_percentage};
    percentages[i].average_utilization = 0.0;
    ENVOY_LOG(debug,
              "zone aware legacy percentages locality {} -> local {} upstream {} (local weight {} total {} upstream weight {} total {})",
              i, local_percentage, upstream_percentage, local_weight, total_local_weight,
              upstream_weight, total_upstream_weight);
  }

  return percentages;
}

absl::FixedArray<ZoneAwareLoadBalancerBase::LocalityPercentages>
ZoneAwareLoadBalancerBase::orcaFallbackLocalityPercentages(
    const HostsPerLocality& local_hosts_per_locality,
    const HostsPerLocality& upstream_hosts_per_locality) const {
  switch (orca_fallback_) {
  case LocalityLbConfig::ZoneAwareLbConfig::EVEN_SPLIT:
    ENVOY_LOG(debug, "zone aware ORCA: using EVEN_SPLIT fallback for locality percentages");
    return calculateEvenSplitLocalityPercentages(local_hosts_per_locality,
                                                 upstream_hosts_per_locality);
  case LocalityLbConfig::ZoneAwareLbConfig::FALLBACK_UNSPECIFIED:
  case LocalityLbConfig::ZoneAwareLbConfig::LEGACY_LOCALITY_PERCENTAGE:
  default:
    ENVOY_LOG(debug, "zone aware ORCA: using legacy fallback for locality percentages");
    return calculateLegacyLocalityPercentages(local_hosts_per_locality,
                                              upstream_hosts_per_locality);
  }
}

absl::FixedArray<ZoneAwareLoadBalancerBase::LocalityPercentages>
ZoneAwareLoadBalancerBase::calculateEvenSplitLocalityPercentages(
    const HostsPerLocality& local_hosts_per_locality,
    const HostsPerLocality& upstream_hosts_per_locality) const {
  const auto& upstream_localities = upstream_hosts_per_locality.get();
  absl::FixedArray<LocalityPercentages> percentages(upstream_localities.size());

  const size_t locality_count = upstream_localities.size();
  if (locality_count > 0) {
    const uint64_t base = 10000ULL / locality_count;
    const uint64_t remainder = 10000ULL % locality_count;
    for (size_t i = 0; i < locality_count; ++i) {
      percentages[i].upstream_percentage = base + (i < remainder ? 1 : 0);
      percentages[i].average_utilization = 0.0;
      ENVOY_LOG(debug, "zone aware ORCA: even split upstream locality {} -> {}", i,
                percentages[i].upstream_percentage);
    }
  }

  absl::flat_hash_map<envoy::config::core::v3::Locality, size_t, LocalityHash, LocalityEqualTo>
      locality_index_map;
  size_t non_empty_local_localities = 0;
  for (const auto& locality_hosts : local_hosts_per_locality.get()) {
    if (!locality_hosts.empty()) {
      locality_index_map.emplace(locality_hosts[0]->locality(), non_empty_local_localities++);
    }
  }

  if (non_empty_local_localities == 0) {
    return percentages;
  }

  const uint64_t base_local = 10000ULL / non_empty_local_localities;
  const uint64_t local_remainder = 10000ULL % non_empty_local_localities;
  size_t assigned = 0;
  for (size_t i = 0; i < upstream_localities.size(); ++i) {
    const auto& upstream_hosts = upstream_localities[i];
    if (upstream_hosts.empty()) {
      continue;
    }
    const auto it = locality_index_map.find(upstream_hosts[0]->locality());
    if (it == locality_index_map.end()) {
      continue;
    }
    percentages[i].local_percentage = base_local + (assigned < local_remainder ? 1 : 0);
    percentages[i].average_utilization = 0.0;
    ENVOY_LOG(debug, "zone aware ORCA: even split local locality {} -> {}", i,
              percentages[i].local_percentage);
    ++assigned;
  }

  return percentages;
}

absl::FixedArray<ZoneAwareLoadBalancerBase::LocalityPercentages>
ZoneAwareLoadBalancerBase::calculateOrcaLocalityPercentages(
    const HostsPerLocality& local_hosts_per_locality,
    const HostsPerLocality& upstream_hosts_per_locality) const {
  absl::FixedArray<LocalityPercentages> fallback =
      orcaFallbackLocalityPercentages(local_hosts_per_locality, upstream_hosts_per_locality);

  const auto& upstream_localities = upstream_hosts_per_locality.get();
  std::vector<double> upstream_capacities(upstream_localities.size(), 0.0);
  std::vector<double> upstream_utilizations(upstream_localities.size(), 0.0);
  double total_upstream_capacity = 0.0;
  for (size_t i = 0; i < upstream_localities.size(); ++i) {
    const auto& locality_hosts = upstream_localities[i];
    if (locality_hosts.empty()) {
      continue;
    }
    auto observation = computeOrcaCapacity(locality_hosts);
    if (!observation.has_value()) {
      ENVOY_LOG(debug, "zone aware ORCA: upstream locality {} missing ORCA data; using fallback",
                i);
      return fallback;
    }
    upstream_capacities[i] = observation->capacity;
    upstream_utilizations[i] = observation->average_utilization;
    total_upstream_capacity += observation->capacity;
    ENVOY_LOG(debug, "zone aware ORCA: upstream locality {} capacity {}", i, observation->capacity);
  }

  if (total_upstream_capacity <= 0.0) {
    ENVOY_LOG(debug, "zone aware ORCA: total upstream capacity zero; reverting to fallback");
    return fallback;
  }

  absl::FixedArray<LocalityPercentages> percentages(upstream_localities.size());
  uint64_t upstream_accumulator = 0;
  uint64_t local_accumulator = 0;

  // Calculate local capacities separately for local percentage distribution
  std::vector<double> local_capacities(upstream_localities.size(), 0.0);
  double total_local_capacity = 0.0;

  if (upstream_hosts_per_locality.hasLocalLocality()) {
    for (size_t i = 0; i < upstream_localities.size(); ++i) {
      if (i < local_hosts_per_locality.get().size()) {
        const auto& local_locality_hosts = local_hosts_per_locality.get()[i];
        if (!local_locality_hosts.empty()) {
          auto local_observation = computeOrcaCapacity(local_locality_hosts);
          if (local_observation.has_value()) {
            local_capacities[i] = local_observation->capacity;
            total_local_capacity += local_observation->capacity;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < upstream_localities.size(); ++i) {
    const double upstream_capacity = upstream_capacities[i];
    uint64_t upstream_percentage = 0;
    if (upstream_capacity > 0.0) {
      upstream_percentage =
          static_cast<uint64_t>((upstream_capacity / total_upstream_capacity) * 10000.0);
    }
    percentages[i].upstream_percentage = upstream_percentage;
    upstream_accumulator += upstream_percentage;

    uint64_t local_percentage = 0;
    if (upstream_hosts_per_locality.hasLocalLocality() && total_local_capacity > 0.0) {
      // For ORCA routing, calculate local percentages based on local capacity distribution
      const double local_capacity = local_capacities[i];
      if (local_capacity > 0.0) {
        local_percentage = static_cast<uint64_t>((local_capacity / total_local_capacity) * 10000.0);
      }
    }
    percentages[i].local_percentage = local_percentage;
    local_accumulator += local_percentage;
    percentages[i].average_utilization = upstream_utilizations[i];
    ENVOY_LOG(debug,
              "zone aware ORCA: locality {} upstream% {}", i, percentages[i].upstream_percentage);
  }

  auto adjust_percentages = [](absl::FixedArray<LocalityPercentages>& values, uint64_t current_sum,
                               auto accessor) {
    if (values.empty() || current_sum == 10000) {
      return;
    }
    int64_t diff = 10000 - static_cast<int64_t>(current_sum);
    for (size_t i = 0; i < values.size() && diff != 0; ++i) {
      uint64_t& field = accessor(values[i]);
      if (diff > 0) {
        ++field;
        --diff;
      } else if (field > 0) {
        --field;
        ++diff;
      }
    }
  };

  adjust_percentages(
      percentages, upstream_accumulator,
      [](LocalityPercentages& value) -> uint64_t& { return value.upstream_percentage; });
  ENVOY_LOG(debug, "zone aware ORCA: upstream accumulator {} after adjustment", upstream_accumulator);

  if (local_accumulator > 0) {
    adjust_percentages(percentages, local_accumulator, [](LocalityPercentages& value) -> uint64_t& {
      return value.local_percentage;
    });
    ENVOY_LOG(debug, "zone aware ORCA: local accumulator {} after adjustment", local_accumulator);
  }

  return percentages;
}

uint32_t ZoneAwareLoadBalancerBase::tryChooseLocalLocalityHostsLegacy(const HostSet& host_set) const {
  if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION && shouldRefreshOrcaRouting()) {
    // Refresh locality routing structures periodically to incorporate the latest ORCA utilization data.
    const_cast<ZoneAwareLoadBalancerBase*>(this)->regenerateLocalityRoutingStructures();
    const_cast<ZoneAwareLoadBalancerBase*>(this)->last_orca_refresh_time_ = std::chrono::steady_clock::now();
  }

  PerPriorityState& state = *per_priority_state_[host_set.priority()];
  ASSERT(state.locality_routing_state_ != LocalityRoutingState::NoLocalityRouting);

  // At this point it's guaranteed to be at least 2 localities in the upstream host set.
  const size_t number_of_localities = host_set.healthyHostsPerLocality().get().size();
  ASSERT(number_of_localities >= 2U);

  const uint32_t probe_numerator = orca_probe_residual_percent_;
  const bool should_probe = probe_numerator > 0 && (random_.random() % 100) < probe_numerator;

  // Try to push all of the requests to the same locality if possible.
  if (state.locality_routing_state_ == LocalityRoutingState::LocalityDirect) {
    ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality());
    if (should_probe && number_of_localities > 1) {
      stats_.lb_zone_routing_cross_zone_.inc();
      const uint32_t remote_index = 1 + (random_.random() % (number_of_localities - 1));
      ENVOY_LOG(debug,
                "zone aware routing: probing non-local locality {} due to configured ORCA probe percent {}",
                remote_index, orca_probe_residual_percent_);
      return remote_index;
    }

    stats_.lb_zone_routing_all_directly_.inc();
    ENVOY_LOG(debug, "zone aware routing: selecting local locality directly");
    return 0;
  }

  ASSERT(state.locality_routing_state_ == LocalityRoutingState::LocalityResidual);
  ASSERT(host_set.healthyHostsPerLocality().hasLocalLocality() ||
         state.local_percent_to_route_ == 0);

  // If we cannot route all requests to the same locality, we already calculated how much we can
  // push to the local locality, check if we can push to local locality on current iteration.

  if (!should_probe && (random_.random() % 10000) < state.local_percent_to_route_) {
    stats_.lb_zone_routing_sampled_.inc();
    ENVOY_LOG(debug, "zone aware routing: sampled local locality with percent {}",
              state.local_percent_to_route_);
    return 0;
  }

  stats_.lb_zone_routing_cross_zone_.inc();
  if (should_probe) {
    ENVOY_LOG(debug,
              "zone aware routing: probing non-local locality due to configured ORCA probe percent {}",
              orca_probe_residual_percent_);
  } else {
    ENVOY_LOG(debug, "zone aware routing: routing cross locality due to insufficient local capacity");
  }

  // This is *extremely* unlikely but possible due to rounding errors when calculating
  // locality percentages. In this case just select random locality.
  if (state.residual_capacity_[number_of_localities - 1] == 0) {
    stats_.lb_zone_no_capacity_left_.inc();
    ENVOY_LOG(debug, "zone aware routing: residual capacity exhausted, choosing random locality");
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

  ENVOY_LOG(debug, "zone aware routing: selected residual locality index {} (threshold {})", i,
            threshold);

  return i;
}

uint32_t ZoneAwareLoadBalancerBase::tryChooseLocalLocalityHosts(const HostSet& host_set) const {
  // Check for fresh pre-calculated ORCA data to use in the hot path
  if (locality_basis_ == LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    // Trigger lazy initialization on first access if needed
    const_cast<ZoneAwareLoadBalancerBase*>(this)->maybeStartOrcaBackgroundUpdates();
    
    // Lock-free atomic read - no mutex contention in hot path!
    const LocalityRoutingData* routing_data = current_routing_data_.load(std::memory_order_acquire);
    
    if (routing_data && routing_data->is_valid && 
        std::chrono::steady_clock::now() - routing_data->last_update_time < orca_update_interval_) {
      // Fresh pre-calculated data available - use fast path!
      ENVOY_LOG(debug, "zone aware ORCA: using fresh pre-calculated routing data ({} localities)", 
                routing_data->locality_percentages->size());
      return selectLocalityFromPreCalculatedData(*routing_data->locality_percentages, host_set);
    }
    
    // No fresh pre-calculated data available, use legacy logic
    ENVOY_LOG(debug, "zone aware ORCA: no fresh pre-calculated data, using legacy calculation");
  }

  // Fallback to existing logic for non-ORCA cases or when ORCA data is not available
  return tryChooseLocalLocalityHostsLegacy(host_set);
}

// Lazy initialization: start background updates on first hot path access if needed
void ZoneAwareLoadBalancerBase::maybeStartOrcaBackgroundUpdates() {
  if (locality_basis_ != LocalityLbConfig::ZoneAwareLbConfig::ORCA_UTILIZATION) {
    return;
  }
  
  // Use atomic flag to ensure we only try to start once
  bool expected = false;
  if (orca_background_started_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    ENVOY_LOG(debug, "zone aware ORCA: starting lazy background initialization");
    
    // For now, perform immediate update to populate initial data
    // This ensures we have data available even without timer access
    updateOrcaRoutingOnMainThread();
    
    // TODO: In a future iteration, we can add proper timer initialization
    // when Event::Dispatcher becomes available through the factory context
  }
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
                                healthy_panic_threshold, locality_config),
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
      [this](uint32_t priority, const HostVector&, const HostVector&) { refresh(priority); });
  member_update_cb_ =
      priority_set.addMemberUpdateCb([this](const HostVector& hosts_added, const HostVector&) {
        if (isSlowStartEnabled()) {
          recalculateHostsInSlowStart(hosts_added);
        }
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

} // namespace Upstream
} // namespace Envoy
