#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "envoy/event/dispatcher.h"
#include "envoy/event/timer.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/logger.h"
#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace OrcaLocality {

/**
 * Per-locality utilization state tracked by OrcaLocalityManager.
 */
struct LocalityState {
  double ema_utilization = 0.0;
  uint32_t valid_host_count = 0; // Hosts with valid ORCA data.
};

/**
 * The computed locality routing split, pushed to worker threads.
 */
struct LocalityRoutingSplit {
  // Percentage of requests to route to the local locality (scaled by 10000).
  uint64_t local_percent_to_route = 10000; // 100% by default.

  // Cumulative residual capacity for remote localities.
  // Used for weighted random sampling of remote locality.
  // Element i = sum of headroom for remote localities 0..i.
  // Indexed by upstream locality index (skipping local which is always 0).
  std::vector<uint64_t> residual_capacity;

  // The total number of upstream localities.
  uint32_t num_localities = 0;

  // Whether locality routing is active (requires >= 2 localities and local locality present).
  bool active = false;
};

/**
 * Configuration for OrcaLocalityManager.
 */
struct OrcaLocalityManagerConfig {
  std::vector<std::string> metric_names_for_computing_utilization;
  std::chrono::milliseconds update_period{1000};
  double ema_alpha = 0.3;
  uint32_t probe_traffic_percent = 5;
  double utilization_variance_threshold = 0.05;
  uint32_t minimum_local_percent = 50;
  std::chrono::milliseconds blackout_period{10000};
  std::chrono::milliseconds weight_expiration_period{180000};
};

/**
 * Aggregates per-locality utilization from ORCA host data and computes
 * the locality routing split. Runs on the main thread.
 *
 * The algorithm:
 * 1. For each locality, compute average utilization across hosts with valid ORCA data.
 * 2. Apply EMA smoothing.
 * 3. Compute global weighted average utilization.
 * 4. If local utilization is within variance_threshold of global avg: route locally (minus probe).
 * 5. If local is overloaded: spill excess to remote localities proportional to headroom.
 * 6. If local is underloaded: route locally (minus probe).
 * 7. Enforce minimum_local_percent and probe_traffic_percent.
 */
class OrcaLocalityManager : protected Logger::Loggable<Logger::Id::upstream> {
public:
  using SplitUpdatedCb = std::function<void(const LocalityRoutingSplit&)>;

  OrcaLocalityManager(const OrcaLocalityManagerConfig& config,
                      const Upstream::PrioritySet& priority_set, TimeSource& time_source,
                      Event::Dispatcher& dispatcher, SplitUpdatedCb on_split_updated);

  // Start the periodic update timer. Must be called once after construction,
  // after all other components (e.g., OrcaWeightManager) are initialized.
  // Matches the OrcaWeightManager::initialize() contract.
  absl::Status initialize();

  // Compute the locality split from current ORCA data.
  // Called periodically on the main thread timer.
  void updateLocalitySplit();

  // Accessors for testing.
  const std::vector<LocalityState>& localityStates() const { return locality_states_; }
  const LocalityRoutingSplit& currentSplit() const { return current_split_; }

private:
  // Compute raw per-locality utilization from host ORCA data.
  void computePerLocalityUtilization(const Upstream::HostSet& host_set,
                                     std::vector<double>& raw_utilization,
                                     std::vector<bool>& has_data);

  // Apply EMA smoothing to per-locality utilization.
  void applyEmaSmoothing(const std::vector<double>& raw_utilization,
                         const std::vector<bool>& has_data);

  // Compute the split based on smoothed utilization values.
  // Caller ensures hasLocalLocality() is true and num_localities >= 2.
  void computeSplit();

  const OrcaLocalityManagerConfig config_;
  const Upstream::PrioritySet& priority_set_;
  TimeSource& time_source_;

  // Per-locality state, indexed by locality index in the upstream HostSet.
  std::vector<LocalityState> locality_states_;
  // Previous EMA values (kept separately so we can detect first update).
  bool first_update_ = true;

  // Current computed split.
  LocalityRoutingSplit current_split_;

  // Scratch buffers reused across updateLocalitySplit() calls to avoid per-tick allocations.
  std::vector<double> raw_utilization_;
  std::vector<bool> has_data_;
  LocalityRoutingSplit new_split_; // Scratch for computeSplit().

  // Timer for periodic updates.
  Event::TimerPtr update_timer_;

  // Callback invoked when the split changes.
  SplitUpdatedCb on_split_updated_;

  // Timing parameters for ORCA data validity.
  std::chrono::milliseconds blackout_period_;
  std::chrono::milliseconds weight_expiration_period_;
};

} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
