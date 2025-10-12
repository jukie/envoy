#pragma once

// Factory for creating WrrLocality load balancers.
// Implements the TypedLoadBalancerFactory interface for integration with Envoy's
// load balancer extension system.

#include "envoy/server/factory_context.h"
#include "envoy/upstream/load_balancer.h"

#include "source/extensions/load_balancing_policies/wrr_locality/wrr_locality_lb.h"

#include "absl/status/statusor.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {

class Factory : public Upstream::TypedLoadBalancerFactory {
public:
  Factory() = default;

  std::string name() const override { return "envoy.load_balancing_policies.wrr_locality"; }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<
        envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality>();
  }

  Upstream::ThreadAwareLoadBalancerPtr create(
      OptRef<const Upstream::LoadBalancerConfig> lb_config, const Upstream::ClusterInfo& cluster_info,
      const Upstream::PrioritySet& priority_set, Runtime::Loader& runtime,
      Random::RandomGenerator& random, TimeSource& time_source) override {
    return std::make_unique<WrrLocalityLoadBalancer>(lb_config, cluster_info, priority_set, runtime,
                                                     random, time_source);
  }

  absl::StatusOr<Upstream::LoadBalancerConfigPtr>
  loadConfig(Server::Configuration::ServerFactoryContext& context,
             const Protobuf::Message& config) override;
};

} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
