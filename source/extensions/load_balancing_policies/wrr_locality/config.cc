// Factory implementation for WrrLocality load balancer.
// Handles configuration validation and load balancer instantiation.

#include "source/extensions/load_balancing_policies/wrr_locality/config.h"

#include "envoy/extensions/load_balancing_policies/wrr_locality/v3/wrr_locality.pb.h"
#include "envoy/extensions/load_balancing_policies/wrr_locality/v3/wrr_locality.pb.validate.h"
#include "envoy/registry/registry.h"

#include "source/common/protobuf/utility.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {

absl::StatusOr<Upstream::LoadBalancerConfigPtr>
Factory::loadConfig(Server::Configuration::ServerFactoryContext& context,
                    const Protobuf::Message& message) {
  const auto& typed_config = MessageUtil::downcastAndValidate<
      const envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality&>(
      message, context.messageValidationVisitor());

  // Validate that endpoint_picking_policy is provided with at least one policy
  if (!typed_config.has_endpoint_picking_policy() ||
      typed_config.endpoint_picking_policy().policies().empty()) {
    return absl::InvalidArgumentError(
        "WrrLocality requires endpoint_picking_policy to be configured with at least one policy");
  }

  // Validate that ClientSideWeightedRoundRobin is present in the endpoint picking policy.
  // This is required to extract ORCA weight calculation parameters.
  bool has_client_side_wrr = false;
  for (const auto& policy : typed_config.endpoint_picking_policy().policies()) {
    if (policy.typed_extension_config().name() == kClientSideWrrPolicyName) {
      has_client_side_wrr = true;
      break;
    }
  }
  if (!has_client_side_wrr) {
    return absl::InvalidArgumentError(
        "WrrLocality requires ClientSideWeightedRoundRobin in endpoint_picking_policy to extract "
        "ORCA weight calculation parameters (blackout_period, weight_expiration_period, etc.)");
  }

  return std::make_unique<WrrLocalityLbConfig>(typed_config, context);
}

REGISTER_FACTORY(Factory, Upstream::TypedLoadBalancerFactory);

} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy

