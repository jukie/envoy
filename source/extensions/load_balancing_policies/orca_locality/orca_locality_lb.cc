#include "source/extensions/load_balancing_policies/orca_locality/orca_locality_lb.h"

#include <algorithm>
#include <cstdint>
#include <memory>

#include "source/common/protobuf/utility.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace OrcaLocality {

// --- OrcaLocalityLbConfig ---

OrcaLocalityLbConfig::OrcaLocalityLbConfig(const OrcaLocalityLbProto& lb_proto,
                                           Event::Dispatcher& main_thread_dispatcher,
                                           ThreadLocal::SlotAllocator& tls_slot_allocator)
    : main_thread_dispatcher_(main_thread_dispatcher),
      tls_slot_allocator_(tls_slot_allocator) {

  // Locality manager config.
  locality_manager_config.metric_names_for_computing_utilization =
      std::vector<std::string>(lb_proto.metric_names_for_computing_utilization().begin(),
                               lb_proto.metric_names_for_computing_utilization().end());
  locality_manager_config.update_period =
      std::chrono::milliseconds(PROTOBUF_GET_MS_OR_DEFAULT(lb_proto, update_period, 1000));
  locality_manager_config.ema_alpha = PROTOBUF_GET_WRAPPED_OR_DEFAULT(lb_proto, ema_alpha, 0.3);
  locality_manager_config.probe_traffic_percent =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(lb_proto, probe_traffic_percent, 5);
  locality_manager_config.utilization_variance_threshold =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(lb_proto, utilization_variance_threshold, 0.05);
  locality_manager_config.minimum_local_percent =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(lb_proto, minimum_local_percent, 50);
  locality_manager_config.blackout_period =
      std::chrono::milliseconds(PROTOBUF_GET_MS_OR_DEFAULT(lb_proto, blackout_period, 10000));
  locality_manager_config.weight_expiration_period = std::chrono::milliseconds(
      PROTOBUF_GET_MS_OR_DEFAULT(lb_proto, weight_expiration_period, 180000));

  // OrcaWeightManager config - reuses same timing and metric params.
  orca_weight_config.metric_names_for_computing_utilization =
      locality_manager_config.metric_names_for_computing_utilization;
  orca_weight_config.error_utilization_penalty =
      PROTOBUF_GET_WRAPPED_OR_DEFAULT(lb_proto, error_utilization_penalty, 1.0);
  orca_weight_config.blackout_period = locality_manager_config.blackout_period;
  orca_weight_config.weight_expiration_period = locality_manager_config.weight_expiration_period;
  orca_weight_config.weight_update_period = locality_manager_config.update_period;
}

// --- SingleLocalityPrioritySet ---

SingleLocalityPrioritySet::SingleLocalityPrioritySet(uint32_t locality_index)
    : locality_index_(locality_index) {
  // Ensure P=0 HostSet exists.
  getOrCreateHostSet(0);
}

void SingleLocalityPrioritySet::updateFromOriginal(const Upstream::HostSet& original_host_set) {
  const auto& hosts_per_locality = original_host_set.hostsPerLocality();
  const auto& healthy_per_locality = original_host_set.healthyHostsPerLocality();
  const auto& degraded_per_locality = original_host_set.degradedHostsPerLocality();
  const auto& excluded_per_locality = original_host_set.excludedHostsPerLocality();

  // Helper to get a const ref to a single locality's hosts, or empty if out of bounds.
  static const Upstream::HostVector empty_hosts;
  auto safe_get = [this](const Upstream::HostsPerLocality& per_loc) -> const Upstream::HostVector& {
    if (locality_index_ < per_loc.get().size()) {
      return per_loc.get()[locality_index_];
    }
    return empty_hosts;
  };

  const auto& orig_hosts = safe_get(hosts_per_locality);
  const auto& orig_healthy = safe_get(healthy_per_locality);
  const auto& orig_degraded = safe_get(degraded_per_locality);
  const auto& orig_excluded = safe_get(excluded_per_locality);

  // Build HostsPerLocality wrappers (copy 1: into the per-locality wrapper).
  // Child LB sees one locality's hosts as a single flat set (no local flag).
  auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{orig_hosts}, false);
  auto healthy_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{orig_healthy}, false);
  auto degraded_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{orig_degraded}, false);
  auto excluded_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{orig_excluded}, false);

  // Build shared pointers for the flat host lists (copy 2: into UpdateHostsParams).
  auto hosts = std::make_shared<Upstream::HostVector>(orig_hosts);
  auto healthy_hosts = std::make_shared<Upstream::HealthyHostVector>(orig_healthy);
  auto degraded_hosts = std::make_shared<Upstream::DegradedHostVector>(orig_degraded);
  auto excluded_hosts = std::make_shared<Upstream::ExcludedHostVector>(orig_excluded);

  Upstream::PrioritySet::UpdateHostsParams params{
      std::move(hosts),
      std::move(healthy_hosts),
      std::move(degraded_hosts),
      std::move(excluded_hosts),
      std::move(hosts_per_loc),
      std::move(healthy_per_loc),
      std::move(degraded_per_loc),
      std::move(excluded_per_loc),
  };

  // Update P=0 host set. This fires member_update_cb to child LBs.
  updateHosts(0, std::move(params), nullptr, {}, {});
}

// --- WorkerLocalLb ---

WorkerLocalLb::WorkerLocalLb(const Upstream::PrioritySet& priority_set,
                             Random::RandomGenerator& random,
                             OptRef<ThreadLocalSplit> tls_split,
                             Upstream::LoadBalancerFactorySharedPtr child_worker_factory)
    : priority_set_(priority_set), random_(random), tls_split_(tls_split),
      child_worker_factory_(std::move(child_worker_factory)) {
  // Register for host membership changes on the worker-local PrioritySet.
  // PriorityUpdateCb marks dirty; MemberUpdateCb (the flush signal) does the rebuild.
  priority_update_cb_ = priority_set_.addPriorityUpdateCb(
      [this](uint32_t, const Upstream::HostVector&, const Upstream::HostVector&) {
        hosts_dirty_ = true;
      });
  member_update_cb_ = priority_set_.addMemberUpdateCb(
      [this](const Upstream::HostVector&, const Upstream::HostVector&) { rebuildChildLbs(); });
}

void WorkerLocalLb::rebuildChildLbs() {
  if (!child_worker_factory_ || priority_set_.hostSetsPerPriority().empty()) {
    hosts_dirty_ = false;
    return;
  }

  const auto& host_set = *priority_set_.hostSetsPerPriority()[0];
  const auto& hosts_per_locality = host_set.healthyHostsPerLocality();
  const size_t num_localities = hosts_per_locality.get().size();

  // Always rebuild the full-set child LB so it sees any host membership changes.
  // This is used when locality routing is inactive (e.g., during startup before
  // ORCA data arrives) to avoid confining traffic to locality 0.
  full_child_lb_ = child_worker_factory_->create({priority_set_, nullptr});

  if (num_localities == last_locality_count_ && !child_lbs_.empty()) {
    // Same locality count - sync hosts in existing per-locality views.
    // This fires updateHosts() on each SingleLocalityPrioritySet, which propagates
    // to child LBs that registered their own callbacks.
    for (size_t i = 0; i < num_localities; ++i) {
      locality_priority_sets_[i]->updateFromOriginal(host_set);
    }
    hosts_dirty_ = false;
    return;
  }

  // Locality count changed - rebuild per-locality child LBs.
  locality_priority_sets_.clear();
  child_lbs_.clear();
  locality_priority_sets_.reserve(num_localities);
  child_lbs_.reserve(num_localities);

  for (uint32_t i = 0; i < num_localities; ++i) {
    auto locality_ps = std::make_unique<SingleLocalityPrioritySet>(i);
    locality_ps->updateFromOriginal(host_set);

    Upstream::LoadBalancerParams params{*locality_ps, nullptr};
    auto child_lb = child_worker_factory_->create(params);

    locality_priority_sets_.push_back(std::move(locality_ps));
    child_lbs_.push_back(std::move(child_lb));
  }

  last_locality_count_ = num_localities;
  hosts_dirty_ = false;
}

Upstream::HostSelectionResponse WorkerLocalLb::chooseHost(Upstream::LoadBalancerContext* context) {
  if (priority_set_.hostSetsPerPriority().empty()) {
    return {nullptr};
  }

  // Use P=0 only for now.
  const auto& host_set = *priority_set_.hostSetsPerPriority()[0];
  const auto& hosts_per_locality = host_set.healthyHostsPerLocality();
  const size_t num_localities = hosts_per_locality.get().size();

  // Rebuild child LBs if host membership changed since the last callback.
  // This handles the initial build (hosts_dirty_ starts true) and any edge cases
  // where a host update arrives between callback registration and first chooseHost().
  if (child_worker_factory_ && hosts_dirty_) {
    rebuildChildLbs();
  }

  // Read the current split directly from thread-local storage (zero synchronization).
  static const LocalityRoutingSplit default_split;
  const auto& current_split = tls_split_.has_value() ? tls_split_->split : default_split;

  // If locality routing is not active, delegate to the full-set child LB or pick
  // from all healthy hosts. The full-set child LB sees all localities so traffic
  // is not confined to locality 0 during startup (before ORCA data arrives).
  if (!current_split.active || num_localities < 2) {
    if (child_worker_factory_ && full_child_lb_) {
      return full_child_lb_->chooseHost(context);
    }
    const auto& all_healthy = host_set.healthyHosts();
    if (all_healthy.empty()) {
      return {nullptr};
    }
    return {pickHostFromLocality(all_healthy)};
  }

  // Determine locality to route to.
  uint32_t locality_index = 0;
  const uint64_t rand_val = random_.random() % 10000;

  if (rand_val < current_split.local_percent_to_route) {
    // Route locally.
    locality_index = 0;
  } else {
    // Route to a remote locality using weighted residual capacity sampling.
    if (current_split.residual_capacity.size() > 1 &&
        current_split.residual_capacity.back() > 0) {
      const uint64_t residual_rand = random_.random() % current_split.residual_capacity.back();
      for (size_t i = 1; i < current_split.residual_capacity.size(); ++i) {
        if (residual_rand < current_split.residual_capacity[i]) {
          locality_index = static_cast<uint32_t>(i);
          break;
        }
      }
    } else {
      // Fallback: pick a random remote locality.
      locality_index = static_cast<uint32_t>(1 + (random_.random() % (num_localities - 1)));
    }
  }

  // Bounds check.
  if (locality_index >= num_localities) {
    locality_index = 0;
  }

  // Delegate to child LB if configured.
  if (child_worker_factory_) {
    if (locality_index < child_lbs_.size() && child_lbs_[locality_index]) {
      return child_lbs_[locality_index]->chooseHost(context);
    }
  }

  // Fallback: built-in weighted random.
  const auto& locality_hosts = hosts_per_locality.get()[locality_index];
  if (locality_hosts.empty()) {
    const auto& all_healthy = host_set.healthyHosts();
    if (all_healthy.empty()) {
      return {nullptr};
    }
    return {pickHostFromLocality(all_healthy)};
  }

  return {pickHostFromLocality(locality_hosts)};
}

Upstream::HostConstSharedPtr
WorkerLocalLb::peekAnotherHost(Upstream::LoadBalancerContext* context) {
  return chooseHost(context).host;
}

Upstream::HostConstSharedPtr
WorkerLocalLb::pickHostFromLocality(const Upstream::HostVector& hosts) {
  if (hosts.empty()) {
    return nullptr;
  }

  // Check if hosts have varying weights (ORCA-derived).
  bool all_equal = true;
  const uint32_t first_weight = hosts[0]->weight();
  for (size_t i = 1; i < hosts.size(); ++i) {
    if (hosts[i]->weight() != first_weight) {
      all_equal = false;
      break;
    }
  }

  if (all_equal) {
    // Unweighted: random selection.
    return hosts[random_.random() % hosts.size()];
  }

  // Weighted random selection (O(n), fine for typical locality sizes).
  uint64_t total_weight = 0;
  for (const auto& host : hosts) {
    total_weight += host->weight();
  }
  if (total_weight == 0) {
    return hosts[random_.random() % hosts.size()];
  }

  uint64_t target = random_.random() % total_weight;
  uint64_t cumulative = 0;
  for (const auto& host : hosts) {
    cumulative += host->weight();
    if (target < cumulative) {
      return host;
    }
  }

  return hosts.back();
}

// --- WorkerLocalLbFactory ---

WorkerLocalLbFactory::WorkerLocalLbFactory(
    Random::RandomGenerator& random, ThreadLocal::SlotAllocator& tls,
    Upstream::LoadBalancerFactorySharedPtr child_worker_factory)
    : random_(random), child_worker_factory_(std::move(child_worker_factory)) {
  tls_ = ThreadLocal::TypedSlot<ThreadLocalSplit>::makeUnique(tls);
  tls_->set([](Event::Dispatcher&) { return std::make_shared<ThreadLocalSplit>(); });
}

Upstream::LoadBalancerPtr WorkerLocalLbFactory::create(Upstream::LoadBalancerParams params) {
  return std::make_unique<WorkerLocalLb>(params.priority_set, random_, tls_->get(),
                                         child_worker_factory_);
}

void WorkerLocalLbFactory::pushSplit(const LocalityRoutingSplit& split) {
  tls_->runOnAllThreads([split](OptRef<ThreadLocalSplit> tls_split) {
    if (tls_split.has_value()) {
      tls_split->split = split;
    }
  });
}

// --- OrcaLocalityLoadBalancer ---

OrcaLocalityLoadBalancer::OrcaLocalityLoadBalancer(
    OptRef<const Upstream::LoadBalancerConfig> lb_config, const Upstream::ClusterInfo& cluster_info,
    const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
    Random::RandomGenerator& random, TimeSource& time_source)
    : cluster_info_(cluster_info), priority_set_(priority_set), runtime_(runtime), random_(random),
      time_source_(time_source) {

  typed_config_ = dynamic_cast<const OrcaLocalityLbConfig*>(lb_config.ptr());
  ASSERT(typed_config_ != nullptr);

  // Create child ThreadAwareLoadBalancer if endpoint_picking_policy is configured.
  if (typed_config_->child_factory_ != nullptr) {
    child_talb_ =
        typed_config_->child_factory_->create(*typed_config_->child_config_, cluster_info_,
                                              priority_set_, runtime_, random_, time_source_);
    child_manages_orca_weights_ = typed_config_->child_manages_orca_weights_;
  }
}

absl::Status OrcaLocalityLoadBalancer::initialize() {
  ASSERT(typed_config_ != nullptr);

  // Initialize the child load balancer first (CSWRR OrcaWeightManager must
  // attach ORCA load report callbacks before we start reading utilization).
  Upstream::LoadBalancerFactorySharedPtr child_worker_factory;
  if (child_talb_) {
    auto status = child_talb_->initialize();
    if (!status.ok()) {
      return status;
    }
    child_worker_factory = child_talb_->factory();
  }

  // Create factory with child worker factory (may be nullptr).
  factory_ = std::make_shared<WorkerLocalLbFactory>(random_, typed_config_->tls_slot_allocator_,
                                                    child_worker_factory);

  // Create OrcaWeightManager only when the child doesn't manage ORCA weights.
  // With CSWRR as child, the child's OrcaWeightManager handles host weights
  // and we just read lastUtilization() from OrcaHostLbPolicyData.
  if (!child_manages_orca_weights_) {
    orca_weight_manager_ = std::make_unique<Common::OrcaWeightManager>(
        typed_config_->orca_weight_config, priority_set_, time_source_,
        typed_config_->main_thread_dispatcher_, []() {
          // Host weights are read directly by workers via host.weight().
          // No explicit refresh needed.
        });

    auto status = orca_weight_manager_->initialize();
    if (!status.ok()) {
      return status;
    }
  }

  // Create OrcaLocalityManager for locality-level aggregation.
  // Reads utilization from OrcaHostLbPolicyData regardless of who set it.
  locality_manager_ = std::make_unique<OrcaLocalityManager>(
      typed_config_->locality_manager_config, priority_set_, time_source_,
      typed_config_->main_thread_dispatcher_,
      [factory = factory_](const LocalityRoutingSplit& split) { factory->pushSplit(split); });

  return locality_manager_->initialize();
}

} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
