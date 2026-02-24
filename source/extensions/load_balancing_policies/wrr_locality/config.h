#pragma once

#include <memory>

#include "envoy/event/dispatcher.h"
#include "envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3/client_side_weighted_round_robin.pb.h"
#include "envoy/server/factory_context.h"
#include "envoy/upstream/load_balancer.h"

#include "source/common/config/utility.h"
#include "source/common/protobuf/utility.h"
#include "source/extensions/load_balancing_policies/client_side_weighted_round_robin/config.h"
#include "source/extensions/load_balancing_policies/common/factory_base.h"
#include "source/extensions/load_balancing_policies/wrr_locality/wrr_locality_lb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {

class Factory : public TypedLoadBalancerFactoryBase<WrrLocalityLbProto> {
public:
  Factory()
      : TypedLoadBalancerFactoryBase<WrrLocalityLbProto>(
            "envoy.load_balancing_policies.wrr_locality") {}

  Upstream::ThreadAwareLoadBalancerPtr create(OptRef<const Upstream::LoadBalancerConfig> lb_config,
                                              const Upstream::ClusterInfo& cluster_info,
                                              const Upstream::PrioritySet& priority_set,
                                              Runtime::Loader& runtime,
                                              Envoy::Random::RandomGenerator& random,
                                              TimeSource& time_source) override {
    return std::make_unique<WrrLocalityLoadBalancer>(lb_config, cluster_info, priority_set, runtime,
                                                     random, time_source);
  }

  absl::StatusOr<Upstream::LoadBalancerConfigPtr>
  loadConfig(Server::Configuration::ServerFactoryContext& context,
             const Protobuf::Message& config) override {
    using CswrrProto = envoy::extensions::load_balancing_policies::
        client_side_weighted_round_robin::v3::ClientSideWeightedRoundRobin;

    const auto& lb_config = dynamic_cast<const WrrLocalityLbProto&>(config);

    // Iterate through the endpoint picking policies to find CSWRR.
    for (const auto& endpoint_picking_policy : lb_config.endpoint_picking_policy().policies()) {
      auto* endpoint_picking_policy_factory =
          Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(
              endpoint_picking_policy.typed_extension_config(),
              /*is_optional=*/true);

      if (endpoint_picking_policy_factory != nullptr) {
        // Ensure that the endpoint picking policy is a ClientSideWeightedRoundRobin.
        auto* client_side_weighted_round_robin_factory = dynamic_cast<
            ::Envoy::Extensions::LoadBalancingPolicies::ClientSideWeightedRoundRobin::Factory*>(
            endpoint_picking_policy_factory);
        if (client_side_weighted_round_robin_factory == nullptr) {
          return absl::InvalidArgumentError(
              "Currently WrrLocalityLoadBalancer only supports "
              "ClientSideWeightedRoundRobinLoadBalancer as its endpoint "
              "picking policy.");
        }

        // Parse the CSWRR child proto to extract ORCA parameters.
        CswrrProto cswrr_proto;
        RETURN_IF_NOT_OK(Config::Utility::translateOpaqueConfig(
            endpoint_picking_policy.typed_extension_config().typed_config(),
            context.messageValidationVisitor(), cswrr_proto));

        std::vector<std::string> metric_names(
            cswrr_proto.metric_names_for_computing_utilization().begin(),
            cswrr_proto.metric_names_for_computing_utilization().end());
        double error_utilization_penalty = cswrr_proto.error_utilization_penalty().value();
        auto blackout_period = std::chrono::milliseconds(
            PROTOBUF_GET_MS_OR_DEFAULT(cswrr_proto, blackout_period, 10000));
        auto weight_expiration_period = std::chrono::milliseconds(
            PROTOBUF_GET_MS_OR_DEFAULT(cswrr_proto, weight_expiration_period, 180000));
        auto weight_update_period = std::chrono::milliseconds(
            PROTOBUF_GET_MS_OR_DEFAULT(cswrr_proto, weight_update_period, 1000));

        // Build the RoundRobinConfig proto with locality_lb_config and slow_start_config.
        RoundRobinConfig round_robin_config;
        if (cswrr_proto.has_slow_start_config()) {
          *round_robin_config.mutable_slow_start_config() = cswrr_proto.slow_start_config();
        }

        // Extract locality_lb_config from the WrrLocality proto, or synthesize a
        // default locality_weighted_lb_config for backwards compatibility.
        if (lb_config.has_locality_lb_config()) {
          *round_robin_config.mutable_locality_lb_config() = lb_config.locality_lb_config();
        } else {
          round_robin_config.mutable_locality_lb_config()->mutable_locality_weighted_lb_config();
        }

        return Upstream::LoadBalancerConfigPtr{new WrrLocalityLbConfig(
            std::move(metric_names), error_utilization_penalty, blackout_period,
            weight_expiration_period, weight_update_period, std::move(round_robin_config),
            context.mainThreadDispatcher(), context.threadLocal())};
      }
    }

    return absl::InvalidArgumentError("No supported endpoint picking policy.");
  }
};

DECLARE_FACTORY(Factory);

} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
