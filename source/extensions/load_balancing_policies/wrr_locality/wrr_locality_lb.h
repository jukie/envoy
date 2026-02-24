#pragma once

#include <memory>
#include <utility>
#include <vector>

#include "envoy/extensions/load_balancing_policies/round_robin/v3/round_robin.pb.h"
#include "envoy/extensions/load_balancing_policies/wrr_locality/v3/wrr_locality.pb.h"
#include "envoy/thread_local/thread_local.h"
#include "envoy/thread_local/thread_local_object.h"
#include "envoy/upstream/upstream.h"

#include "source/common/common/callback_impl.h"
#include "source/common/upstream/load_balancer_factory_base.h"
#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"
#include "source/extensions/load_balancing_policies/round_robin/round_robin_lb.h"

#include "absl/status/status.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {

using ::Envoy::Upstream::ThreadAwareLoadBalancer;
using ::Envoy::Upstream::TypedLoadBalancerFactoryBase;

using WrrLocalityLbProto =
    envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality;
using RoundRobinConfig = envoy::extensions::load_balancing_policies::round_robin::v3::RoundRobin;

/**
 * Load balancer config that holds ORCA parameters extracted from the ClientSideWeightedRoundRobin
 * child config, plus a RoundRobinConfig with locality_lb_config set natively.
 */
class WrrLocalityLbConfig : public Upstream::LoadBalancerConfig {
public:
  WrrLocalityLbConfig(std::vector<std::string> metric_names_for_computing_utilization,
                      double error_utilization_penalty, std::chrono::milliseconds blackout_period,
                      std::chrono::milliseconds weight_expiration_period,
                      std::chrono::milliseconds weight_update_period,
                      RoundRobinConfig round_robin_config,
                      Event::Dispatcher& main_thread_dispatcher,
                      ThreadLocal::SlotAllocator& tls_slot_allocator)
      : metric_names_for_computing_utilization(std::move(metric_names_for_computing_utilization)),
        error_utilization_penalty(error_utilization_penalty), blackout_period(blackout_period),
        weight_expiration_period(weight_expiration_period),
        weight_update_period(weight_update_period),
        round_robin_config(std::move(round_robin_config)),
        main_thread_dispatcher_(main_thread_dispatcher), tls_slot_allocator_(tls_slot_allocator) {}

  // Parameters for weight calculation from ORCA load report.
  std::vector<std::string> metric_names_for_computing_utilization;
  double error_utilization_penalty;
  // Timing parameters for the weight update.
  std::chrono::milliseconds blackout_period;
  std::chrono::milliseconds weight_expiration_period;
  std::chrono::milliseconds weight_update_period;

  // RoundRobin proto with locality_lb_config and slow_start_config set.
  RoundRobinConfig round_robin_config;

  Event::Dispatcher& main_thread_dispatcher_;
  ThreadLocal::SlotAllocator& tls_slot_allocator_;
};

/**
 * Weighted Round Robin Locality policy. Manages ORCA-based host weights and
 * delegates per-locality endpoint picking to RoundRobinLoadBalancer instances
 * with native locality_lb_config support.
 */
class WrrLocalityLoadBalancer : public ThreadAwareLoadBalancer,
                                protected Logger::Loggable<Logger::Id::upstream> {
public:
  // Thread-local shim to store callbacks for weight updates of worker-local LBs.
  class ThreadLocalShim : public Envoy::ThreadLocal::ThreadLocalObject {
  public:
    ::Envoy::Common::CallbackManager<void> apply_weights_cb_helper_;
  };

  // Worker-local LB extending RoundRobinLoadBalancer.
  class WorkerLocalLb : public Upstream::RoundRobinLoadBalancer {
  public:
    WorkerLocalLb(const Upstream::PrioritySet& priority_set,
                  const Upstream::PrioritySet* local_priority_set, Upstream::ClusterLbStats& stats,
                  Runtime::Loader& runtime, Random::RandomGenerator& random,
                  uint32_t healthy_panic_threshold, const RoundRobinConfig& round_robin_config,
                  TimeSource& time_source, OptRef<ThreadLocalShim> tls_shim);

  private:
    ::Envoy::Common::CallbackHandlePtr apply_weights_cb_handle_;
  };

  // Factory that creates worker-local LB instances on each worker thread.
  class WorkerLocalLbFactory : public Upstream::LoadBalancerFactory {
  public:
    WorkerLocalLbFactory(const Upstream::ClusterInfo& cluster_info,
                         const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
                         Envoy::Random::RandomGenerator& random, TimeSource& time_source,
                         ThreadLocal::SlotAllocator& tls,
                         const RoundRobinConfig& round_robin_config)
        : cluster_info_(cluster_info), priority_set_(priority_set), runtime_(runtime),
          random_(random), time_source_(time_source), round_robin_config_(round_robin_config) {
      tls_ = ThreadLocal::TypedSlot<ThreadLocalShim>::makeUnique(tls);
      tls_->set([](Envoy::Event::Dispatcher&) { return std::make_shared<ThreadLocalShim>(); });
    }

    Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override;

    bool recreateOnHostChange() const override { return false; }

    void applyWeightsToAllWorkers();

    std::unique_ptr<Envoy::ThreadLocal::TypedSlot<ThreadLocalShim>> tls_;

    const Upstream::ClusterInfo& cluster_info_;
    const Upstream::PrioritySet& priority_set_;
    Runtime::Loader& runtime_;
    Envoy::Random::RandomGenerator& random_;
    TimeSource& time_source_;
    const RoundRobinConfig round_robin_config_;
  };

public:
  WrrLocalityLoadBalancer(OptRef<const Upstream::LoadBalancerConfig> lb_config,
                          const Upstream::ClusterInfo& cluster_info,
                          const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
                          Envoy::Random::RandomGenerator& random, TimeSource& time_source);

private:
  // {Upstream::ThreadAwareLoadBalancer} Interface implementation.
  Upstream::LoadBalancerFactorySharedPtr factory() override { return factory_; }
  absl::Status initialize() override;

  std::shared_ptr<WorkerLocalLbFactory> factory_;
  std::unique_ptr<Common::OrcaWeightManager> weight_manager_;
};

} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
