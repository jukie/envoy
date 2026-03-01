#include "source/extensions/load_balancing_policies/orca_locality/orca_locality_manager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace OrcaLocality {

OrcaLocalityManager::OrcaLocalityManager(const OrcaLocalityManagerConfig& config,
                                         const Upstream::PrioritySet& priority_set,
                                         TimeSource& time_source, Event::Dispatcher& dispatcher,
                                         SplitUpdatedCb on_split_updated)
    : config_(config), priority_set_(priority_set), time_source_(time_source),
      on_split_updated_(std::move(on_split_updated)), blackout_period_(config.blackout_period),
      weight_expiration_period_(config.weight_expiration_period) {
  update_timer_ = dispatcher.createTimer([this]() -> void {
    updateLocalitySplit();
    update_timer_->enableTimer(config_.update_period);
  });
}

absl::Status OrcaLocalityManager::initialize() {
  update_timer_->enableTimer(config_.update_period);
  return absl::OkStatus();
}

void OrcaLocalityManager::updateLocalitySplit() {
  ENVOY_LOG(trace, "OrcaLocalityManager::updateLocalitySplit");

  // We only do locality routing for P=0.
  if (priority_set_.hostSetsPerPriority().empty()) {
    return;
  }
  const auto& host_set = *priority_set_.hostSetsPerPriority()[0];
  const auto& hosts_per_locality = host_set.healthyHostsPerLocality();
  const size_t num_localities = hosts_per_locality.get().size();
  const bool has_local_locality = hosts_per_locality.hasLocalLocality();

  // Need at least 2 localities and a local locality for locality routing.
  if (num_localities < 2 || !has_local_locality) {
    if (current_split_.active) {
      current_split_.active = false;
      current_split_.local_percent_to_route = 10000;
      current_split_.residual_capacity.clear();
      current_split_.num_localities = static_cast<uint32_t>(num_localities);
      on_split_updated_(current_split_);
    }
    return;
  }

  // Resize locality states if needed (may grow on host set update).
  if (locality_states_.size() != num_localities) {
    locality_states_.resize(num_localities);
    first_update_ = true;
  }

  // Step 1: Compute raw per-locality utilization (reuse member buffers).
  raw_utilization_.assign(num_localities, 0.0);
  has_data_.assign(num_localities, false);
  computePerLocalityUtilization(host_set, raw_utilization_, has_data_);

  // Step 2: Apply EMA smoothing.
  applyEmaSmoothing(raw_utilization_, has_data_);

  // Step 3-8: Compute the split.
  computeSplit();
}

void OrcaLocalityManager::computePerLocalityUtilization(const Upstream::HostSet& host_set,
                                                        std::vector<double>& raw_utilization,
                                                        std::vector<bool>& has_data) {
  const auto& hosts_per_locality = host_set.healthyHostsPerLocality();
  const MonotonicTime now = time_source_.monotonicTime();
  const MonotonicTime max_non_empty_since = now - blackout_period_;
  const MonotonicTime min_last_update_time = now - weight_expiration_period_;

  for (size_t i = 0; i < hosts_per_locality.get().size(); ++i) {
    const auto& hosts = hosts_per_locality.get()[i];
    double total_utilization = 0.0;
    uint32_t valid_count = 0;

    for (const auto& host : hosts) {
      auto client_side_data = host->typedLbPolicyData<Common::OrcaHostLbPolicyData>();
      if (!client_side_data.has_value()) {
        continue;
      }
      // Check validity: not in blackout and not expired.
      // Use isDataValid() (read-only) rather than getWeightIfValid() to avoid
      // resetting the blackout timer as a side effect of a read operation.
      if (!client_side_data->isDataValid(max_non_empty_since, min_last_update_time)) {
        continue;
      }
      // Include all hosts with valid data, even at 0% utilization (genuinely idle).
      total_utilization += client_side_data->lastUtilization();
      ++valid_count;
    }

    locality_states_[i].valid_host_count = valid_count;

    if (valid_count > 0) {
      raw_utilization[i] = total_utilization / valid_count;
      has_data[i] = true;
    }
  }
}

void OrcaLocalityManager::applyEmaSmoothing(const std::vector<double>& raw_utilization,
                                            const std::vector<bool>& has_data) {
  for (size_t i = 0; i < locality_states_.size(); ++i) {
    if (!has_data[i]) {
      // No valid data for this locality - keep previous EMA.
      continue;
    }

    if (first_update_) {
      // First observation: initialize EMA to the raw value.
      locality_states_[i].ema_utilization = raw_utilization[i];
    } else {
      // EMA: ema = alpha * raw + (1 - alpha) * prev_ema
      locality_states_[i].ema_utilization =
          config_.ema_alpha * raw_utilization[i] +
          (1.0 - config_.ema_alpha) * locality_states_[i].ema_utilization;
    }
  }
  first_update_ = false;
}

void OrcaLocalityManager::computeSplit() {
  const size_t num_localities = locality_states_.size();

  // Compute global weighted-average utilization (weighted by valid host count).
  double total_weighted_util = 0.0;
  uint32_t total_valid_hosts = 0;
  for (const auto& state : locality_states_) {
    if (state.valid_host_count > 0) {
      total_weighted_util += state.ema_utilization * state.valid_host_count;
      total_valid_hosts += state.valid_host_count;
    }
  }

  if (total_valid_hosts == 0) {
    // No ORCA data available yet. Don't change the split.
    return;
  }

  const double global_avg_util = total_weighted_util / total_valid_hosts;

  // Local locality is always index 0 when hasLocalLocality() is true.
  const double local_util = locality_states_[0].ema_utilization;
  const double local_deviation = local_util - global_avg_util;

  uint64_t local_percent; // In range [0, 100].
  bool needs_spill = false;

  if (std::abs(local_deviation) <= config_.utilization_variance_threshold) {
    // Close enough to balanced - stay local (minus probe traffic).
    local_percent = 100 - config_.probe_traffic_percent;
  } else if (local_deviation > config_.utilization_variance_threshold) {
    // Local zone is overloaded - spill excess.
    // spill_percent is proportional to deviation, clamped to leave at least minimum_local_percent.
    double max_spill = 100.0 - static_cast<double>(config_.minimum_local_percent);
    double raw_spill = local_deviation * 100.0;
    uint64_t spill_percent = static_cast<uint64_t>(std::min(raw_spill, max_spill));
    // Ensure at least probe_traffic_percent goes remote.
    spill_percent = std::max(spill_percent, static_cast<uint64_t>(config_.probe_traffic_percent));
    local_percent = 100 - spill_percent;
    needs_spill = true;
  } else {
    // Local zone is underloaded - route locally (minus probe traffic).
    local_percent = 100 - config_.probe_traffic_percent;
  }

  // Enforce minimum_local_percent.
  local_percent = std::max(local_percent, static_cast<uint64_t>(config_.minimum_local_percent));

  // Enforce probe_traffic: remote zones always get at least probe_traffic_percent.
  const uint64_t max_local = 100 - config_.probe_traffic_percent;
  local_percent = std::min(local_percent, max_local);

  // Build the split (reuse scratch buffer to avoid per-tick allocations).
  new_split_.active = true;
  new_split_.num_localities = static_cast<uint32_t>(num_localities);
  new_split_.local_percent_to_route = local_percent * 100; // Scale to 10000.

  // Compute cumulative residual capacity for remote localities.
  new_split_.residual_capacity.assign(num_localities, 0);

  if (needs_spill) {
    // Distribute spill to remote zones proportional to headroom (1 - utilization).
    double total_headroom = 0.0;
    for (size_t i = 1; i < num_localities; ++i) {
      double headroom = std::max(0.0, 1.0 - locality_states_[i].ema_utilization);
      total_headroom += headroom;
    }

    uint64_t cumulative = 0;
    if (total_headroom > 0.0) {
      for (size_t i = 1; i < num_localities; ++i) {
        double headroom = std::max(0.0, 1.0 - locality_states_[i].ema_utilization);
        uint64_t share = static_cast<uint64_t>((headroom / total_headroom) * 10000);
        cumulative += share;
        new_split_.residual_capacity[i] = cumulative;
      }
    } else {
      // All remote zones at capacity - distribute evenly.
      uint64_t equal_share = 10000 / (num_localities - 1);
      for (size_t i = 1; i < num_localities; ++i) {
        cumulative += equal_share;
        new_split_.residual_capacity[i] = cumulative;
      }
    }
  } else {
    // Probe traffic - distribute evenly across remote zones.
    uint64_t equal_share = 10000 / (num_localities - 1);
    uint64_t cumulative = 0;
    for (size_t i = 1; i < num_localities; ++i) {
      cumulative += equal_share;
      new_split_.residual_capacity[i] = cumulative;
    }
  }

  // Only push the update if the split actually changed.
  if (new_split_.local_percent_to_route != current_split_.local_percent_to_route ||
      new_split_.num_localities != current_split_.num_localities ||
      new_split_.active != current_split_.active ||
      new_split_.residual_capacity != current_split_.residual_capacity) {
    std::swap(current_split_, new_split_);
    on_split_updated_(current_split_);
  }
}

} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
