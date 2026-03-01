#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "envoy/extensions/load_balancing_policies/orca_locality/v3/orca_locality.pb.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/upstream/load_balancer.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/logger.h"
#include "source/common/upstream/upstream_impl.h"
#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"
#include "source/extensions/load_balancing_policies/orca_locality/orca_locality_manager.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace OrcaLocality {

using OrcaLocalityLbProto =
    envoy::extensions::load_balancing_policies::orca_locality::v3::OrcaLocalityLbConfig;

/**
 * Load balancer config that wraps the proto and provides resolved child policy.
 */
class OrcaLocalityLbConfig : public Upstream::LoadBalancerConfig {
public:
  OrcaLocalityLbConfig(const OrcaLocalityLbProto& lb_proto,
                       Event::Dispatcher& main_thread_dispatcher,
                       ThreadLocal::SlotAllocator& tls_slot_allocator);

  OrcaLocalityManagerConfig locality_manager_config;
  Common::OrcaWeightManagerConfig orca_weight_config;

  Event::Dispatcher& main_thread_dispatcher_;
  ThreadLocal::SlotAllocator& tls_slot_allocator_;

  // Resolved child endpoint-picking policy (nullptr = built-in weighted random).
  Upstream::TypedLoadBalancerFactory* child_factory_{nullptr};
  Upstream::LoadBalancerConfigPtr child_config_;
  // Whether child policy manages its own ORCA weights (e.g., CSWRR).
  bool child_manages_orca_weights_{false};
};

/**
 * Thread-local storage for the current locality routing split.
 * Updated via runOnAllThreads() (~1/s), read by workers with zero synchronization.
 * Follows the CSWRR ThreadLocalShim pattern.
 */
struct ThreadLocalSplit : public ThreadLocal::ThreadLocalObject {
  LocalityRoutingSplit split;
};

/**
 * A PrioritySet view that presents a single locality's hosts as the full host set.
 * Used to give per-locality child LBs a filtered view of the upstream hosts.
 */
class SingleLocalityPrioritySet : public Upstream::PrioritySetImpl {
public:
  explicit SingleLocalityPrioritySet(uint32_t locality_index);

  // Sync hosts from the original host set for our locality.
  void updateFromOriginal(const Upstream::HostSet& original_host_set);

private:
  const uint32_t locality_index_;
};

/**
 * Per-worker load balancer that handles locality selection using ORCA-based splits,
 * and host selection within a locality using weighted selection.
 */
class WorkerLocalLb : public Upstream::LoadBalancer,
                      protected Logger::Loggable<Logger::Id::upstream> {
public:
  WorkerLocalLb(const Upstream::PrioritySet& priority_set, Random::RandomGenerator& random,
                OptRef<ThreadLocalSplit> tls_split,
                Upstream::LoadBalancerFactorySharedPtr child_worker_factory);

  // Upstream::LoadBalancer
  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext* context) override;
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* context) override;
  OptRef<Envoy::Http::ConnectionPool::ConnectionLifetimeCallbacks> lifetimeCallbacks() override {
    return {};
  }
  absl::optional<Upstream::SelectedPoolAndConnection>
  selectExistingConnection(Upstream::LoadBalancerContext*, const Upstream::Host&,
                           std::vector<uint8_t>&) override {
    return absl::nullopt;
  }

private:
  // Pick a host from a specific locality's healthy hosts using weighted selection.
  // Fallback when no child policy is configured.
  Upstream::HostConstSharedPtr pickHostFromLocality(const Upstream::HostVector& hosts);

  // Rebuild per-locality child LBs in response to host membership changes.
  void rebuildChildLbs();

  const Upstream::PrioritySet& priority_set_;
  Random::RandomGenerator& random_;
  OptRef<ThreadLocalSplit> tls_split_;

  // Child policy support.
  Upstream::LoadBalancerFactorySharedPtr child_worker_factory_;
  std::vector<std::unique_ptr<SingleLocalityPrioritySet>> locality_priority_sets_;
  std::vector<Upstream::LoadBalancerPtr> child_lbs_;
  // Full-set child LB used when locality routing is inactive (e.g., during startup
  // before ORCA data arrives). Sees all hosts across all localities so traffic is
  // not confined to locality 0 while waiting for the first split computation.
  Upstream::LoadBalancerPtr full_child_lb_;
  size_t last_locality_count_{0};

  // Host change tracking via callbacks on the worker-local PrioritySet.
  // Both fire on the same worker thread, so hosts_dirty_ needs no synchronization.
  Envoy::Common::CallbackHandlePtr priority_update_cb_;
  Envoy::Common::CallbackHandlePtr member_update_cb_;
  bool hosts_dirty_{true}; // Start dirty to force initial build.
};

/**
 * Factory that creates WorkerLocalLb instances on worker threads.
 */
class WorkerLocalLbFactory : public Upstream::LoadBalancerFactory {
public:
  WorkerLocalLbFactory(Random::RandomGenerator& random,
                       ThreadLocal::SlotAllocator& tls,
                       Upstream::LoadBalancerFactorySharedPtr child_worker_factory);

  Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override;
  bool recreateOnHostChange() const override { return false; }

  // Push a new split to all worker threads via TLS.
  void pushSplit(const LocalityRoutingSplit& split);

  Random::RandomGenerator& random_;
  std::unique_ptr<ThreadLocal::TypedSlot<ThreadLocalSplit>> tls_;
  Upstream::LoadBalancerFactorySharedPtr child_worker_factory_;
};

/**
 * ORCA locality-picking load balancer. Thread-aware main-thread component.
 *
 * Uses ORCA utilization reports to compute per-locality load and dynamically
 * adjusts the local vs remote traffic split. Within a locality, selects hosts
 * using weighted round-robin based on ORCA-derived weights.
 */
class OrcaLocalityLoadBalancer : public Upstream::ThreadAwareLoadBalancer,
                                 protected Logger::Loggable<Logger::Id::upstream> {
public:
  OrcaLocalityLoadBalancer(OptRef<const Upstream::LoadBalancerConfig> lb_config,
                           const Upstream::ClusterInfo& cluster_info,
                           const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
                           Random::RandomGenerator& random, TimeSource& time_source);

  // Upstream::ThreadAwareLoadBalancer
  Upstream::LoadBalancerFactorySharedPtr factory() override { return factory_; }
  absl::Status initialize() override;

private:
  const Upstream::ClusterInfo& cluster_info_;
  const Upstream::PrioritySet& priority_set_;
  Runtime::Loader& runtime_;
  Random::RandomGenerator& random_;
  TimeSource& time_source_;

  std::shared_ptr<WorkerLocalLbFactory> factory_;

  // Child endpoint-picking policy ThreadAwareLoadBalancer (nullptr = built-in weighted random).
  Upstream::ThreadAwareLoadBalancerPtr child_talb_;
  bool child_manages_orca_weights_{false};

  // ORCA weight manager: attaches per-host ORCA data and computes host weights.
  // Only created when child does not manage ORCA weights.
  std::unique_ptr<Common::OrcaWeightManager> orca_weight_manager_;

  // Locality manager: aggregates per-locality utilization and computes splits.
  std::unique_ptr<OrcaLocalityManager> locality_manager_;

  // Config needed for deferred initialization.
  const OrcaLocalityLbConfig* typed_config_;
};

} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
