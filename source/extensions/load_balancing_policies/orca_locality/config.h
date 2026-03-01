#pragma once

#include "envoy/extensions/load_balancing_policies/orca_locality/v3/orca_locality.pb.h"
#include "envoy/extensions/load_balancing_policies/orca_locality/v3/orca_locality.pb.validate.h"
#include "envoy/server/factory_context.h"
#include "envoy/upstream/load_balancer.h"

#include "source/common/config/utility.h"
#include "source/common/upstream/load_balancer_factory_base.h"
#include "source/extensions/load_balancing_policies/orca_locality/orca_locality_lb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace OrcaLocality {

class Factory : public Upstream::TypedLoadBalancerFactoryBase<OrcaLocalityLbProto> {
public:
  Factory()
      : TypedLoadBalancerFactoryBase<OrcaLocalityLbProto>(
            "envoy.load_balancing_policies.orca_locality") {}

  Upstream::ThreadAwareLoadBalancerPtr create(OptRef<const Upstream::LoadBalancerConfig> lb_config,
                                              const Upstream::ClusterInfo& cluster_info,
                                              const Upstream::PrioritySet& priority_set,
                                              Runtime::Loader& runtime,
                                              Envoy::Random::RandomGenerator& random,
                                              TimeSource& time_source) override {
    return std::make_unique<OrcaLocalityLoadBalancer>(lb_config, cluster_info, priority_set,
                                                      runtime, random, time_source);
  }

  absl::StatusOr<Upstream::LoadBalancerConfigPtr>
  loadConfig(Server::Configuration::ServerFactoryContext& context,
             const Protobuf::Message& config) override {
    const auto& lb_config = dynamic_cast<const OrcaLocalityLbProto&>(config);
    auto orca_config = std::make_unique<OrcaLocalityLbConfig>(
        lb_config, context.mainThreadDispatcher(), context.threadLocal());

    // Cross-field validation: minimum_local_percent + probe_traffic_percent must not exceed 100,
    // otherwise probe enforcement silently overrides the minimum local guarantee.
    if (orca_config->locality_manager_config.minimum_local_percent +
            orca_config->locality_manager_config.probe_traffic_percent >
        100) {
      return absl::InvalidArgumentError(
          "minimum_local_percent + probe_traffic_percent must not exceed 100.");
    }

    // Resolve child endpoint-picking policy if configured.
    for (const auto& policy : lb_config.endpoint_picking_policy().policies()) {
      auto* child_factory = Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(
          policy.typed_extension_config(), /*is_optional=*/true);
      if (child_factory == nullptr) {
        continue;
      }

      // Load and validate child config.
      auto sub_lb_proto = child_factory->createEmptyConfigProto();
      RETURN_IF_NOT_OK(Config::Utility::translateOpaqueConfig(
          policy.typed_extension_config().typed_config(), context.messageValidationVisitor(),
          *sub_lb_proto));

      auto child_config_or = child_factory->loadConfig(context, *sub_lb_proto);
      RETURN_IF_NOT_OK(child_config_or.status());

      orca_config->child_factory_ = child_factory;
      orca_config->child_config_ = std::move(child_config_or.value());

      // Check if the child policy manages its own ORCA weights (e.g., CSWRR).
      orca_config->child_manages_orca_weights_ = child_factory->managesOrcaWeights();

      return Upstream::LoadBalancerConfigPtr{orca_config.release()};
    }

    // No child policy configured or none recognized - use built-in
    // weighted random. This is valid when endpoint_picking_policy is
    // absent.
    if (lb_config.has_endpoint_picking_policy() &&
        lb_config.endpoint_picking_policy().policies_size() > 0) {
      return absl::InvalidArgumentError("No supported endpoint picking policy found in "
                                        "endpoint_picking_policy.");
    }

    return Upstream::LoadBalancerConfigPtr{orca_config.release()};
  }
};

DECLARE_FACTORY(Factory);

} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
