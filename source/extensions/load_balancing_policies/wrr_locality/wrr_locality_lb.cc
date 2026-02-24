#include "source/extensions/load_balancing_policies/wrr_locality/wrr_locality_lb.h"

#include <cstdint>
#include <memory>

#include "source/common/protobuf/utility.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {

// WorkerLocalLb

WrrLocalityLoadBalancer::WorkerLocalLb::WorkerLocalLb(
    const Upstream::PrioritySet& priority_set, const Upstream::PrioritySet* local_priority_set,
    Upstream::ClusterLbStats& stats, Runtime::Loader& runtime, Random::RandomGenerator& random,
    uint32_t healthy_panic_threshold, const RoundRobinConfig& round_robin_config,
    TimeSource& time_source, OptRef<ThreadLocalShim> tls_shim)
    : RoundRobinLoadBalancer(priority_set, local_priority_set, stats, runtime, random,
                             healthy_panic_threshold, round_robin_config, time_source) {
  if (tls_shim.has_value()) {
    apply_weights_cb_handle_ = tls_shim->apply_weights_cb_helper_.add([this, &priority_set]() {
      for (const Upstream::HostSetPtr& host_set : priority_set.hostSetsPerPriority()) {
        if (host_set != nullptr) {
          refresh(host_set->priority());
        }
      }
    });
  }
}

// WorkerLocalLbFactory

Upstream::LoadBalancerPtr
WrrLocalityLoadBalancer::WorkerLocalLbFactory::create(Upstream::LoadBalancerParams params) {
  return std::make_unique<WorkerLocalLb>(
      params.priority_set, params.local_priority_set, cluster_info_.lbStats(), runtime_, random_,
      PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(cluster_info_.lbConfig(),
                                                     healthy_panic_threshold, 100, 50),
      round_robin_config_, time_source_, tls_->get());
}

void WrrLocalityLoadBalancer::WorkerLocalLbFactory::applyWeightsToAllWorkers() {
  tls_->runOnAllThreads([](OptRef<ThreadLocalShim> tls_shim) -> void {
    if (tls_shim.has_value()) {
      tls_shim->apply_weights_cb_helper_.runCallbacks();
    }
  });
}

// WrrLocalityLoadBalancer

WrrLocalityLoadBalancer::WrrLocalityLoadBalancer(
    OptRef<const Upstream::LoadBalancerConfig> lb_config, const Upstream::ClusterInfo& cluster_info,
    const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
    Envoy::Random::RandomGenerator& random, TimeSource& time_source) {

  const auto* typed_lb_config = dynamic_cast<const WrrLocalityLbConfig*>(lb_config.ptr());
  ASSERT(typed_lb_config != nullptr);

  factory_ = std::make_shared<WorkerLocalLbFactory>(
      cluster_info, priority_set, runtime, random, time_source,
      typed_lb_config->tls_slot_allocator_, typed_lb_config->round_robin_config);

  Common::OrcaWeightManagerConfig orca_config{
      typed_lb_config->metric_names_for_computing_utilization,
      typed_lb_config->error_utilization_penalty,
      typed_lb_config->blackout_period,
      typed_lb_config->weight_expiration_period,
      typed_lb_config->weight_update_period,
  };
  weight_manager_ = std::make_unique<Common::OrcaWeightManager>(
      orca_config, priority_set, time_source, typed_lb_config->main_thread_dispatcher_,
      [factory = factory_]() { factory->applyWeightsToAllWorkers(); });
}

absl::Status WrrLocalityLoadBalancer::initialize() { return weight_manager_->initialize(); }

} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
