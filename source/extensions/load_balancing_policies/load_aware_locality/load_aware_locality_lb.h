#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "envoy/common/random_generator.h"
#include "envoy/event/dispatcher.h"
#include "envoy/stats/stats_macros.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/locality.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/logger.h"
#include "source/common/upstream/upstream_impl.h"
#include "source/extensions/load_balancing_policies/common/load_balancer_impl.h"

#include "absl/container/flat_hash_map.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace LoadAwareLocality {

#define ALL_LOAD_AWARE_LOCALITY_STATS(COUNTER)                                                     \
  COUNTER(all_overloaded_total)                                                                    \
  COUNTER(local_preferred_total)                                                                   \
  COUNTER(probe_active_total)                                                                      \
  COUNTER(stale_locality_total)

struct LoadAwareLocalityStats {
  ALL_LOAD_AWARE_LOCALITY_STATS(GENERATE_COUNTER_STRUCT)
};

/**
 * Per-host LB policy data for the load-aware locality policy.
 * Receives ORCA load reports via the multi-slot HostLbPolicyData mechanism
 * and stores a lock-free utilization value for the main-thread weight computation.
 *
 * Written by worker threads via onOrcaLoadReport(); read by the main thread
 * during locality weight computation. The release/acquire on last_update_time_ms_
 * ensures the reader sees the utilization that was stored before it.
 *
 * Use lastUpdateTimeMs() == 0 to distinguish "never reported" from
 * "reported 0.0 utilization".
 */
class LocalityLbHostData : public Upstream::HostLbPolicyData {
public:
  LocalityLbHostData(TimeSource& time_source, std::vector<std::string> metric_names)
      : time_source_(time_source), metric_names_(std::move(metric_names)) {}

  bool receivesOrcaLoadReport() const override { return true; }

  absl::Status onOrcaLoadReport(const Upstream::OrcaLoadReport& report,
                                const StreamInfo::StreamInfo&) override;

  double utilization() const { return utilization_.load(std::memory_order_relaxed); }
  int64_t lastUpdateTimeMs() const { return last_update_time_ms_.load(std::memory_order_acquire); }

private:
  void storeUtilization(double util, int64_t monotonic_time_ms) {
    if (!std::isfinite(util)) {
      return;
    }
    utilization_.store(std::clamp(util, 0.0, 1.0), std::memory_order_relaxed);
    last_update_time_ms_.store(monotonic_time_ms, std::memory_order_release);
  }

  static_assert(std::atomic<double>::is_always_lock_free,
                "std::atomic<double> must be lock-free for safe cross-thread utilization updates");
  std::atomic<double> utilization_{0.0};
  std::atomic<int64_t> last_update_time_ms_{0};
  TimeSource& time_source_;
  const std::vector<std::string> metric_names_;
};

/**
 * Load balancer config for the load-aware locality policy.
 */
// Shared ownership wrapper for the child LB config. The config is created during loadConfig()
// and shared between LoadAwareLocalityLbConfig (which is const when accessed) and
// WorkerLocalLbFactory (which needs it for the lifetime of the cluster).
using LoadBalancerConfigSharedPtr = std::shared_ptr<Upstream::LoadBalancerConfig>;

class LoadAwareLocalityLbConfig : public Upstream::LoadBalancerConfig {
public:
  LoadAwareLocalityLbConfig(Upstream::TypedLoadBalancerFactory& endpoint_picking_policy_factory,
                            std::string endpoint_picking_policy_name,
                            LoadBalancerConfigSharedPtr endpoint_picking_policy_config,
                            std::chrono::milliseconds weight_update_period,
                            double utilization_variance_threshold, double ewma_alpha,
                            double remote_probe_fraction,
                            std::chrono::milliseconds weight_expiration_period,
                            std::vector<std::string> metric_names_for_computing_utilization,
                            Event::Dispatcher& main_thread_dispatcher,
                            ThreadLocal::SlotAllocator& tls_slot_allocator)
      : endpoint_picking_policy_factory_(endpoint_picking_policy_factory),
        endpoint_picking_policy_name_(std::move(endpoint_picking_policy_name)),
        endpoint_picking_policy_config_(std::move(endpoint_picking_policy_config)),
        weight_update_period_(weight_update_period),
        utilization_variance_threshold_(utilization_variance_threshold), ewma_alpha_(ewma_alpha),
        remote_probe_fraction_(remote_probe_fraction),
        weight_expiration_period_(weight_expiration_period),
        metric_names_for_computing_utilization_(std::move(metric_names_for_computing_utilization)),
        main_thread_dispatcher_(main_thread_dispatcher), tls_slot_allocator_(tls_slot_allocator) {}

  Upstream::TypedLoadBalancerFactory& endpointPickingPolicyFactory() const {
    return endpoint_picking_policy_factory_;
  }
  const std::string& endpointPickingPolicyName() const { return endpoint_picking_policy_name_; }
  const LoadBalancerConfigSharedPtr& endpointPickingPolicyConfig() const {
    return endpoint_picking_policy_config_;
  }
  std::chrono::milliseconds weightUpdatePeriod() const { return weight_update_period_; }
  double utilizationVarianceThreshold() const { return utilization_variance_threshold_; }
  double ewmaAlpha() const { return ewma_alpha_; }
  double remoteProbeFraction() const { return remote_probe_fraction_; }
  std::chrono::milliseconds weightExpirationPeriod() const { return weight_expiration_period_; }
  const std::vector<std::string>& metricNamesForComputingUtilization() const {
    return metric_names_for_computing_utilization_;
  }
  Event::Dispatcher& mainThreadDispatcher() const { return main_thread_dispatcher_; }
  ThreadLocal::SlotAllocator& tlsSlotAllocator() const { return tls_slot_allocator_; }

private:
  Upstream::TypedLoadBalancerFactory& endpoint_picking_policy_factory_;
  const std::string endpoint_picking_policy_name_;
  LoadBalancerConfigSharedPtr endpoint_picking_policy_config_;
  std::chrono::milliseconds weight_update_period_;
  double utilization_variance_threshold_;
  double ewma_alpha_;
  double remote_probe_fraction_;
  std::chrono::milliseconds weight_expiration_period_;
  std::vector<std::string> metric_names_for_computing_utilization_;
  Event::Dispatcher& main_thread_dispatcher_;
  ThreadLocal::SlotAllocator& tls_slot_allocator_;
};

// Advisory locality weights keyed by Locality identity (not HostsPerLocality position). Keying by
// identity makes the main-thread→worker mapping robust to a locality being added/removed between
// the snapshot tick and the worker's live membership: the lexicographic index of later localities
// shifts on add/remove, but their identity does not. Uses the same comparators as
// load_balancer_impl.cc.
using LocalityWeightsMap = absl::flat_hash_map<envoy::config::core::v3::Locality, double,
                                               Upstream::LocalityHash, Upstream::LocalityEqualTo>;

/**
 * Immutable snapshot of per-locality routing weights for a single priority.
 */
struct PriorityRoutingWeights {
  enum class SelectionSource : uint8_t { Healthy = 0, Degraded = 1, AllHosts = 2 };

  struct SourceWeights {
    // Per-locality routing weights (host_count * headroom), keyed by Locality identity.
    LocalityWeightsMap weights;
    // Informational flag: true if local preference was triggered (variance below threshold).
    // Not consulted on the hot path — the routing decision is fully encoded in weights.
    // With remote_probe_fraction > 0, weights still include a small remote share even when true.
    bool all_local{false};
  };

  // Per-source routing weights: [Healthy=0, Degraded=1, AllHosts=2].
  std::array<SourceWeights, 3> by_source;
  // True when the hosts-per-locality has a local locality (index 0 is "local").
  // Workers use this to pick the correct zone routing stat counter.
  bool has_local_locality{false};

  const LocalityWeightsMap& weightsFor(SelectionSource s) const {
    return by_source[static_cast<size_t>(s)].weights;
  }
  bool allLocalFor(SelectionSource s) const { return by_source[static_cast<size_t>(s)].all_local; }
};

/**
 * Immutable snapshot of advisory per-priority locality weights shared between the main thread and
 * workers. Priority/health/panic selection is live worker state (LoadBalancerBase), not
 * snapshotted.
 */
struct RoutingWeightsSnapshot {
  // Per-priority routing weights, indexed by cluster priority.
  std::vector<PriorityRoutingWeights> priority_weights;
};

using RoutingWeightsSnapshotConstSharedPtr = std::shared_ptr<const RoutingWeightsSnapshot>;

/**
 * Thread-local shim holding the routing weights pointer, pushed from the main thread via TLS.
 */
struct ThreadLocalShim : public ThreadLocal::ThreadLocalObject {
  RoutingWeightsSnapshotConstSharedPtr routing_weights;
};

class WorkerLocalLb;

/**
 * Factory shared across workers. Holds routing weights and child policy factory.
 * The main thread updates routing weights; workers read them to create WorkerLocalLb instances.
 */
class WorkerLocalLbFactory : public Upstream::LoadBalancerFactory {
public:
  WorkerLocalLbFactory(Upstream::TypedLoadBalancerFactory& child_factory,
                       std::string child_factory_name, LoadBalancerConfigSharedPtr child_config,
                       const Upstream::ClusterInfo& cluster_info,
                       const Upstream::PrioritySet& cluster_priority_set, Runtime::Loader& runtime,
                       Envoy::Random::RandomGenerator& random, TimeSource& time_source,
                       ThreadLocal::SlotAllocator& tls_slot_allocator);

  // Upstream::LoadBalancerFactory
  Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override;
  bool recreateOnHostChangeDeprecated() const override { return false; }

  // Called by the main thread (from LoadAwareLocalityLoadBalancer::initialize()) to initialize
  // the shared child ThreadAwareLoadBalancer. Must be called before any worker creates a LB.
  absl::Status initializeChildLb();

  // Called by the main thread to publish new routing weights via TLS.
  void updateRoutingWeights(RoutingWeightsSnapshotConstSharedPtr snapshot) {
    tls_->runOnAllThreads([snapshot](OptRef<ThreadLocalShim> shim) {
      if (shim.has_value()) {
        shim->routing_weights = snapshot;
      }
    });
  }

  // Called by workers to get the current routing weights (lock-free TLS read).
  // SAFETY: Must only be called on a thread that owns a TLS slot instance (worker or main
  // thread). The returned pointer is valid only for the duration of the current task; do not
  // store it across yield points or after the TLS slot could be updated.
  const RoutingWeightsSnapshot* routingWeights() const {
    auto shim = tls_->get();
    return shim.has_value() ? shim->routing_weights.get() : nullptr;
  }

  // Called by workers to create a per-locality worker LB from the shared child factory.
  // Must only be called after initializeChildLb() has returned on the main thread.
  Upstream::LoadBalancerPtr
  createWorkerChildLb(Upstream::PrioritySetImpl& per_locality_priority_set);

  // Whether the child policy requires the worker LB to be recreated on host membership changes.
  bool recreateChildOnHostChange() const;

  Envoy::Random::RandomGenerator& random() const { return random_; }
  Runtime::Loader& runtime() const { return runtime_; }
  uint32_t healthyPanicThreshold() const { return healthy_panic_threshold_; }
  bool failTrafficOnPanic() const { return fail_traffic_on_panic_; }
  Upstream::ClusterLbStats& lbStats() const { return cluster_info_.lbStats(); }

private:
  std::string child_factory_name_;
  LoadBalancerConfigSharedPtr child_config_;
  const Upstream::ClusterInfo& cluster_info_;
  Envoy::Random::RandomGenerator& random_;
  Runtime::Loader& runtime_;
  uint32_t healthy_panic_threshold_;
  const bool fail_traffic_on_panic_;

  // Single child ThreadAwareLoadBalancer created and initialized on the main thread.
  // Workers only call factory()->create() from it.
  Upstream::ThreadAwareLoadBalancerPtr child_thread_aware_lb_;

  std::unique_ptr<ThreadLocal::TypedSlot<ThreadLocalShim>> tls_;
};

/**
 * Per-locality state in the worker-local LB. Holds a child PrioritySet and child LB for
 * one locality.
 */
struct PerSourceLocalityState {
  // PrioritySet containing only the hosts that can be selected for one source in one locality.
  std::unique_ptr<Upstream::PrioritySetImpl> priority_set;
  // The worker-local LB for this source/locality pair, created from the shared child factory.
  Upstream::LoadBalancerPtr lb;
};

struct PerLocalityState {
  // Per-source child LB state: [Healthy=0, Degraded=1, AllHosts=2].
  std::array<PerSourceLocalityState, 3> by_source;
  // Identity of this worker locality, used to look up its advisory weight in the snapshot map.
  // Empty for a locality with no hosts (which carries no weight regardless).
  envoy::config::core::v3::Locality locality;

  PerSourceLocalityState& stateFor(PriorityRoutingWeights::SelectionSource s) {
    return by_source[static_cast<size_t>(s)];
  }
  const PerSourceLocalityState& stateFor(PriorityRoutingWeights::SelectionSource s) const {
    return by_source[static_cast<size_t>(s)];
  }
};

struct PerPriorityLocalityState {
  std::vector<PerLocalityState> localities;
};

/**
 * Worker-local load balancer. Derives LoadBalancerBase for live priority/health/panic selection,
 * then selects a locality by capacity-weighted random and delegates to the per-locality child LB
 * for endpoint selection.
 */
class WorkerLocalLb : public Upstream::LoadBalancerBase {
public:
  WorkerLocalLb(WorkerLocalLbFactory& factory, const Upstream::PrioritySet& priority_set);
  ~WorkerLocalLb() override;

  // Upstream::LoadBalancer
  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;
  OptRef<Envoy::Http::ConnectionPool::ConnectionLifetimeCallbacks> lifetimeCallbacks() override;
  absl::optional<Upstream::SelectedPoolAndConnection>
  selectExistingConnection(Upstream::LoadBalancerContext* context, const Upstream::Host& host,
                           std::vector<uint8_t>& hash_key) override;

private:
  // Build per-locality child LBs for all priorities.
  void buildPerPriorityLocalities();

  // Build or rebuild the per-locality child LBs for a single priority.
  void buildPerLocality(uint32_t priority, const Upstream::HostSet& host_set);

  // Re-sync a priority's per-locality child LBs after a priority update. allow_rebuild permits
  // tearing down and rebuilding on a topology change (priority/locality count mismatch); when false
  // (in-place attribute update with no membership delta) a mismatch is left for the next
  // membership-change callback to rebuild.
  void syncPriority(uint32_t priority, bool allow_rebuild);

  // Update a locality/source PrioritySet with a pre-selected host subset.
  void updateLocalityHosts(PerSourceLocalityState& state, const Upstream::HostVector& hosts,
                           bool is_local, const Upstream::HostVector& hosts_added,
                           const Upstream::HostVector& hosts_removed);

  // Update the per-source child LBs for one locality from the cluster's current host set.
  void syncLocalityState(PerLocalityState& state, const Upstream::HostSet& host_set,
                         size_t locality_index, bool recreate_child);

  // Live priority + selection source for a request, resolved from worker-local LoadBalancerBase
  // state. fail=true means the request must be failed (empty priority set, or panic +
  // fail_traffic_on_panic). in_panic lets callers count the panic stat themselves (peek does not).
  struct PrioritySourcePick {
    uint32_t priority;
    PriorityRoutingWeights::SelectionSource source;
    bool in_panic;
    bool fail;
  };
  PrioritySourcePick resolvePrioritySource(Upstream::LoadBalancerContext* context);

  // Choose a locality index within the selected priority/source by weighted-random over the routing
  // snapshot. Weights are looked up by identity (see LocalityWeightsMap; missing → 0.0). Returns 0
  // when there is no usable weight (single locality, no/stale snapshot, or zero effective total).
  // FUTURE: for very high locality counts, cache a per-worker index-aligned weight vector rebuilt
  // on snapshot publish + membership change to restore pure index reads (avoids per-pick hashing).
  size_t chooseLocality(const RoutingWeightsSnapshot* snapshot, uint32_t priority,
                        PriorityRoutingWeights::SelectionSource source) const;

  // Find a usable child LB at the preferred locality, falling back to any locality with a
  // non-null LB when the routing snapshot is stale (e.g. a locality's hosts were removed but
  // the routing weights haven't been recomputed yet). Returns nullptr if no locality has a
  // usable LB. Sets actual_idx to the index of the locality whose LB was returned.
  Upstream::LoadBalancer* pickLocalityLb(const std::vector<PerLocalityState>& per_locality,
                                         PriorityRoutingWeights::SelectionSource source,
                                         size_t preferred_idx, size_t& actual_idx) const;

  WorkerLocalLbFactory& factory_;
  std::vector<PerPriorityLocalityState> per_priority_locality_;
  // Destroyed explicitly in the destructor before other members so the callback doesn't fire
  // during destruction and access freed per-locality state.
  Envoy::Common::CallbackHandlePtr member_update_cb_;
};

/**
 * Main thread load balancer. Reads host-level utilization data and computes locality routing
 * weights on a periodic timer.
 */
class LoadAwareLocalityLoadBalancer : public Upstream::ThreadAwareLoadBalancer,
                                      protected Logger::Loggable<Logger::Id::upstream> {
public:
  LoadAwareLocalityLoadBalancer(OptRef<const Upstream::LoadBalancerConfig> lb_config,
                                const Upstream::ClusterInfo& cluster_info,
                                const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
                                Envoy::Random::RandomGenerator& random, TimeSource& time_source);
  ~LoadAwareLocalityLoadBalancer() override;

  // Upstream::ThreadAwareLoadBalancer
  Upstream::LoadBalancerFactorySharedPtr factory() override { return factory_; }
  absl::Status initialize() override;

private:
  // Compute per-locality routing weights from host-level utilization and publish to factory.
  void computeLocalityRoutingWeights();

  // Attach LocalityLbHostData to hosts that don't already have it.
  void addLbPolicyDataToHosts(const Upstream::HostVector& hosts);

  // Per-source outcome flags from computeSourceWeights, OR'd into the per-tick stats by the caller.
  // stale_localities is only consumed from the all-hosts pass (the canonical staleness source).
  struct SourceComputeResult {
    bool all_overloaded{false};
    bool local_preferred{false};
    bool probe_active{false};
    uint32_t stale_localities{0};
  };

  // Compute a source's per-locality weights into weights_map/all_local, advancing the caller-owned
  // EWMA state (smoothed/smoothed_valid). Returns this pass's per-tick stat signals.
  SourceComputeResult
  computeSourceWeights(const Upstream::HostsPerLocality& all_hosts_per_locality,
                       const std::vector<Upstream::HostVector>& eligible_hosts_per_locality,
                       int64_t now_ms, LocalityWeightsMap& weights_map, bool& all_local,
                       std::vector<double>& smoothed, std::vector<bool>& smoothed_valid);

  const Upstream::PrioritySet& priority_set_;
  Upstream::ClusterLbStats& stats_;
  LoadAwareLocalityStats lb_stats_;
  TimeSource& time_source_;
  double utilization_variance_threshold_;
  double ewma_alpha_;
  double remote_probe_fraction_;
  std::chrono::milliseconds weight_expiration_period_;
  std::vector<std::string> metric_names_;
  // Per-source, per-priority, per-locality EWMA-smoothed utilization state (main thread only).
  std::array<std::vector<std::vector<double>>, 3> smoothed_utilizations_;
  std::array<std::vector<std::vector<bool>>, 3> smoothed_utilizations_valid_;
  Event::TimerPtr weight_update_timer_;
  std::chrono::milliseconds weight_update_period_;
  std::shared_ptr<WorkerLocalLbFactory> factory_;
  Envoy::Common::CallbackHandlePtr priority_update_cb_;
};

} // namespace LoadAwareLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
