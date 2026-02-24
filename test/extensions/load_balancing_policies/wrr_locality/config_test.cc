#include "envoy/config/core/v3/extension.pb.h"

#include "source/extensions/load_balancing_policies/wrr_locality/config.h"

#include "test/mocks/server/factory_context.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/priority_set.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace WrrLocality {
namespace {

// Helper to build a WrrLocality config message with ClientSideWeightedRoundRobin as the endpoint
// picking policy.
envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality buildWrrLocalityConfig() {
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin cswrr_config_msg;
  envoy::config::core::v3::TypedExtensionConfig cswrr_config;
  cswrr_config.set_name("envoy.load_balancing_policies.client_side_weighted_round_robin");
  cswrr_config.mutable_typed_config()->PackFrom(cswrr_config_msg);

  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality wrr_locality_config_msg;
  *(wrr_locality_config_msg.mutable_endpoint_picking_policy()
        ->add_policies()
        ->mutable_typed_extension_config()) = cswrr_config;

  return wrr_locality_config_msg;
}

TEST(WrrLocalityConfigTest, ValidateSuccess) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Upstream::MockClusterInfo> cluster_info;
  NiceMock<Upstream::MockPrioritySet> main_thread_priority_set;
  NiceMock<Upstream::MockPrioritySet> thread_local_priority_set;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  auto wrr_locality_config_msg = buildWrrLocalityConfig();

  envoy::config::core::v3::TypedExtensionConfig wrr_locality_config;
  wrr_locality_config.set_name("envoy.load_balancing_policies.wrr_locality");
  wrr_locality_config.mutable_typed_config()->PackFrom(wrr_locality_config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(wrr_locality_config);
  EXPECT_EQ("envoy.load_balancing_policies.wrr_locality", factory.name());

  auto lb_config = factory.loadConfig(context, wrr_locality_config_msg).value();

  // Verify the config is a WrrLocalityLbConfig with expected ORCA defaults.
  auto* typed_config = dynamic_cast<const WrrLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(nullptr, typed_config);
  EXPECT_EQ(std::chrono::milliseconds(10000), typed_config->blackout_period);
  EXPECT_EQ(std::chrono::milliseconds(180000), typed_config->weight_expiration_period);
  EXPECT_EQ(std::chrono::milliseconds(1000), typed_config->weight_update_period);
  EXPECT_DOUBLE_EQ(0.0, typed_config->error_utilization_penalty);
  EXPECT_TRUE(typed_config->metric_names_for_computing_utilization.empty());
  // Default: locality_weighted_lb_config should be set.
  EXPECT_TRUE(typed_config->round_robin_config.has_locality_lb_config());
  EXPECT_TRUE(
      typed_config->round_robin_config.locality_lb_config().has_locality_weighted_lb_config());

  auto thread_aware_lb =
      factory.create(*lb_config, cluster_info, main_thread_priority_set, context.runtime_loader_,
                     context.api_.random_, context.time_system_);
  EXPECT_NE(nullptr, thread_aware_lb);

  ASSERT_TRUE(thread_aware_lb->initialize().ok());

  auto thread_local_lb_factory = thread_aware_lb->factory();
  EXPECT_NE(nullptr, thread_local_lb_factory);

  auto thread_local_lb = thread_local_lb_factory->create({thread_local_priority_set, nullptr});
  EXPECT_NE(nullptr, thread_local_lb);
}

TEST(WrrLocalityConfigTest, ValidateSuccessWithOrcaParameters) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  // Build ClientSideWeightedRoundRobin config with custom ORCA parameters.
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin cswrr_config_msg;
  cswrr_config_msg.mutable_blackout_period()->set_seconds(20);
  cswrr_config_msg.mutable_weight_expiration_period()->set_seconds(300);
  cswrr_config_msg.mutable_weight_update_period()->set_seconds(5);
  cswrr_config_msg.mutable_error_utilization_penalty()->set_value(0.5);
  cswrr_config_msg.mutable_metric_names_for_computing_utilization()->Add("metric1");
  cswrr_config_msg.mutable_metric_names_for_computing_utilization()->Add("metric2");

  envoy::config::core::v3::TypedExtensionConfig cswrr_config;
  cswrr_config.set_name("envoy.load_balancing_policies.client_side_weighted_round_robin");
  cswrr_config.mutable_typed_config()->PackFrom(cswrr_config_msg);

  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality wrr_locality_config_msg;
  *(wrr_locality_config_msg.mutable_endpoint_picking_policy()
        ->add_policies()
        ->mutable_typed_extension_config()) = cswrr_config;

  envoy::config::core::v3::TypedExtensionConfig wrr_locality_config;
  wrr_locality_config.set_name("envoy.load_balancing_policies.wrr_locality");
  wrr_locality_config.mutable_typed_config()->PackFrom(wrr_locality_config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(wrr_locality_config);

  auto lb_config = factory.loadConfig(context, wrr_locality_config_msg).value();

  auto* typed_config = dynamic_cast<const WrrLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(nullptr, typed_config);
  EXPECT_EQ(std::chrono::milliseconds(20000), typed_config->blackout_period);
  EXPECT_EQ(std::chrono::milliseconds(300000), typed_config->weight_expiration_period);
  EXPECT_EQ(std::chrono::milliseconds(5000), typed_config->weight_update_period);
  EXPECT_DOUBLE_EQ(0.5, typed_config->error_utilization_penalty);
  ASSERT_EQ(2, typed_config->metric_names_for_computing_utilization.size());
  EXPECT_EQ("metric1", typed_config->metric_names_for_computing_utilization[0]);
  EXPECT_EQ("metric2", typed_config->metric_names_for_computing_utilization[1]);
}

TEST(WrrLocalityConfigTest, ValidateSuccessWithLocalityLbConfig) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  auto wrr_locality_config_msg = buildWrrLocalityConfig();

  // Set zone_aware_lb_config on the WrrLocality proto.
  auto* zone_aware_config =
      wrr_locality_config_msg.mutable_locality_lb_config()->mutable_zone_aware_lb_config();
  zone_aware_config->set_fail_traffic_on_panic(true);
  zone_aware_config->mutable_min_cluster_size()->set_value(3);

  envoy::config::core::v3::TypedExtensionConfig wrr_locality_config;
  wrr_locality_config.set_name("envoy.load_balancing_policies.wrr_locality");
  wrr_locality_config.mutable_typed_config()->PackFrom(wrr_locality_config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(wrr_locality_config);

  auto lb_config = factory.loadConfig(context, wrr_locality_config_msg).value();

  auto* typed_config = dynamic_cast<const WrrLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(nullptr, typed_config);
  // Verify zone_aware_lb_config is propagated.
  EXPECT_TRUE(typed_config->round_robin_config.has_locality_lb_config());
  EXPECT_TRUE(typed_config->round_robin_config.locality_lb_config().has_zone_aware_lb_config());
  EXPECT_TRUE(typed_config->round_robin_config.locality_lb_config()
                  .zone_aware_lb_config()
                  .fail_traffic_on_panic());
  EXPECT_EQ(3, typed_config->round_robin_config.locality_lb_config()
                   .zone_aware_lb_config()
                   .min_cluster_size()
                   .value());
}

TEST(WrrLocalityConfigTest, ValidateSuccessWithoutLocalityLbConfig) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  auto wrr_locality_config_msg = buildWrrLocalityConfig();
  // No locality_lb_config set — should default to locality_weighted_lb_config.

  envoy::config::core::v3::TypedExtensionConfig wrr_locality_config;
  wrr_locality_config.set_name("envoy.load_balancing_policies.wrr_locality");
  wrr_locality_config.mutable_typed_config()->PackFrom(wrr_locality_config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(wrr_locality_config);

  auto lb_config = factory.loadConfig(context, wrr_locality_config_msg).value();

  auto* typed_config = dynamic_cast<const WrrLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(nullptr, typed_config);
  EXPECT_TRUE(typed_config->round_robin_config.has_locality_lb_config());
  EXPECT_TRUE(
      typed_config->round_robin_config.locality_lb_config().has_locality_weighted_lb_config());
}

TEST(WrrLocalityConfigTest, ValidateFailureWithoutEndpointPickingPolicy) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;

  // WrrLocality policy without endpoint picking policy.
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality wrr_locality_config_msg;
  envoy::config::core::v3::TypedExtensionConfig wrr_locality_config;
  wrr_locality_config.set_name("envoy.load_balancing_policies.wrr_locality");
  wrr_locality_config.mutable_typed_config()->PackFrom(wrr_locality_config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(wrr_locality_config);
  EXPECT_EQ("envoy.load_balancing_policies.wrr_locality", factory.name());

  EXPECT_EQ(factory.loadConfig(context, wrr_locality_config_msg).status(),
            absl::InvalidArgumentError("No supported endpoint picking policy."));
}

TEST(WrrLocalityConfigTest, ValidateFailureUnsupportedEndpointPickingPolicy) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;

  // WrrLocality policy WITHOUT ClientSideWeightedRoundRobin policy for endpoint
  // picking is currently not supported.
  // Random lb policy for endpoint picking.
  envoy::extensions::load_balancing_policies::random::v3::Random epp_config_msg;
  envoy::config::core::v3::TypedExtensionConfig epp_config;

  epp_config.set_name("envoy.load_balancing_policies.random");
  epp_config.mutable_typed_config()->PackFrom(epp_config_msg);

  // WrrLocality policy with Random policy for endpoint picking.
  envoy::extensions::load_balancing_policies::wrr_locality::v3::WrrLocality wrr_locality_config_msg;
  *(wrr_locality_config_msg.mutable_endpoint_picking_policy()
        ->add_policies()
        ->mutable_typed_extension_config()) = epp_config;

  envoy::config::core::v3::TypedExtensionConfig wrr_locality_config;
  wrr_locality_config.set_name("envoy.load_balancing_policies.wrr_locality");
  wrr_locality_config.mutable_typed_config()->PackFrom(wrr_locality_config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(wrr_locality_config);
  EXPECT_EQ("envoy.load_balancing_policies.wrr_locality", factory.name());

  EXPECT_EQ(factory.loadConfig(context, wrr_locality_config_msg).status(),
            absl::InvalidArgumentError("Currently WrrLocalityLoadBalancer only supports "
                                       "ClientSideWeightedRoundRobinLoadBalancer as its endpoint "
                                       "picking policy."));
}

} // namespace
} // namespace WrrLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
