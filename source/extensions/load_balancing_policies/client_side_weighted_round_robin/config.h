#pragma once

#include "envoy/event/dispatcher.h"
#include "envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3/client_side_weighted_round_robin.pb.h"
#include "envoy/server/factory_context.h"
#include "envoy/upstream/load_balancer.h"

#include "source/common/common/logger.h"
#include "source/extensions/load_balancing_policies/client_side_weighted_round_robin/client_side_weighted_round_robin_lb.h"
#include "source/extensions/load_balancing_policies/common/factory_base.h"

#include "absl/status/status.h"

/**
 * Create a thread-aware client-side weighted round robin load balancer for the given cluster.
 *
 * @param lb_config Optional load balancer configuration to initialize the balancer with; may be
 *        empty to use defaults.
 * @param cluster_info Cluster metadata and settings used by the balancer.
 * @param priority_set Priority set for the cluster used to obtain hosts.
 * @param runtime Runtime loader used for feature flags and runtime settings.
 * @param random Random generator used for weight selection and tie-breaking.
 * @param time_source Time source used for timing and metrics.
 * @returns A pointer to a newly created thread-aware ClientSideWeightedRoundRobinLoadBalancer.
 */

/**
 * Convert the protobuf configuration into a LoadBalancerConfigPtr suitable for this factory.
 *
 * @param context Server factory context whose main thread dispatcher and thread-local storage
 *        are used to construct the load balancer config.
 * @param config Protobuf message expected to be a
 *        ClientSideWeightedRoundRobin (envoy.extensions.load_balancing_policies.client_side_weighted_round_robin.v3).
 * @returns An absl::StatusOr containing the constructed LoadBalancerConfigPtr on success.
 */
namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace ClientSideWeightedRoundRobin {

using ClientSideWeightedRoundRobinLbProto = envoy::extensions::load_balancing_policies::
    client_side_weighted_round_robin::v3::ClientSideWeightedRoundRobin;

class Factory : public Upstream::TypedLoadBalancerFactoryBase<ClientSideWeightedRoundRobinLbProto> {
public:
  Factory()
      : Upstream::TypedLoadBalancerFactoryBase<ClientSideWeightedRoundRobinLbProto>(
            "envoy.load_balancing_policies.client_side_weighted_round_robin") {}

  Upstream::ThreadAwareLoadBalancerPtr create(OptRef<const Upstream::LoadBalancerConfig> lb_config,
                                              const Upstream::ClusterInfo& cluster_info,
                                              const Upstream::PrioritySet& priority_set,
                                              Runtime::Loader& runtime,
                                              Envoy::Random::RandomGenerator& random,
                                              TimeSource& time_source) override {
    return std::make_unique<Upstream::ClientSideWeightedRoundRobinLoadBalancer>(
        lb_config, cluster_info, priority_set, runtime, random, time_source);
  }

  absl::StatusOr<Upstream::LoadBalancerConfigPtr>
  loadConfig(Server::Configuration::ServerFactoryContext& context,
             const Protobuf::Message& config) override {
    const auto& lb_config = dynamic_cast<const ClientSideWeightedRoundRobinLbProto&>(config);
    return Upstream::LoadBalancerConfigPtr{new Upstream::ClientSideWeightedRoundRobinLbConfig(
        lb_config, context.mainThreadDispatcher(), context.threadLocal())};
  }
};

DECLARE_FACTORY(Factory);

} // namespace ClientSideWeightedRoundRobin
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
