#include "envoy/config/core/v3/extension.pb.h"
#include "envoy/extensions/load_balancing_policies/client_side_weighted_round_robin/v3/client_side_weighted_round_robin.pb.h"
#include "envoy/extensions/load_balancing_policies/round_robin/v3/round_robin.pb.h"

#include "source/extensions/load_balancing_policies/orca_locality/config.h"

#include "test/mocks/server/factory_context.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/priority_set.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace OrcaLocality {
namespace {

// Helper to build an endpoint_picking_policy with a single child.
void addChildPolicy(OrcaLocalityLbProto& config_msg, const std::string& name,
                    const Protobuf::Message& child_msg) {
  envoy::config::core::v3::TypedExtensionConfig child_config;
  child_config.set_name(name);
  child_config.mutable_typed_config()->PackFrom(child_msg);
  *config_msg.mutable_endpoint_picking_policy()->add_policies()->mutable_typed_extension_config() =
      child_config;
}

TEST(OrcaLocalityConfigTest, FactoryCreatesLb) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Upstream::MockClusterInfo> cluster_info;
  NiceMock<Upstream::MockPrioritySet> main_thread_priority_set;
  NiceMock<Upstream::MockPrioritySet> thread_local_priority_set;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  envoy::config::core::v3::TypedExtensionConfig config;
  config.set_name("envoy.load_balancing_policies.orca_locality");
  OrcaLocalityLbProto config_msg;
  config_msg.add_metric_names_for_computing_utilization("named_metrics.cpu");
  config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory = Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(config);
  EXPECT_EQ("envoy.load_balancing_policies.orca_locality", factory.name());

  auto lb_config = factory.loadConfig(context, config_msg).value();

  // Two timers are created: one for OrcaWeightManager, one for OrcaLocalityManager.
  new NiceMock<Event::MockTimer>(&mock_thread_dispatcher);
  new NiceMock<Event::MockTimer>(&mock_thread_dispatcher);

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

TEST(OrcaLocalityConfigTest, DefaultConfigValues) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  envoy::config::core::v3::TypedExtensionConfig ext_config;
  ext_config.set_name("envoy.load_balancing_policies.orca_locality");
  OrcaLocalityLbProto config_msg;
  ext_config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(ext_config);
  auto lb_config = factory.loadConfig(context, config_msg).value();
  EXPECT_NE(lb_config, nullptr);

  auto* typed = dynamic_cast<const OrcaLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->locality_manager_config.update_period, std::chrono::milliseconds(1000));
  EXPECT_DOUBLE_EQ(typed->locality_manager_config.ema_alpha, 0.3);
  EXPECT_EQ(typed->locality_manager_config.probe_traffic_percent, 5);
  EXPECT_DOUBLE_EQ(typed->locality_manager_config.utilization_variance_threshold, 0.05);
  EXPECT_EQ(typed->locality_manager_config.minimum_local_percent, 50);
  EXPECT_EQ(typed->locality_manager_config.blackout_period, std::chrono::milliseconds(10000));
  EXPECT_EQ(typed->locality_manager_config.weight_expiration_period,
            std::chrono::milliseconds(180000));
  // No child policy configured.
  EXPECT_EQ(typed->child_factory_, nullptr);
  EXPECT_EQ(typed->child_config_, nullptr);
  EXPECT_FALSE(typed->child_manages_orca_weights_);
}

TEST(OrcaLocalityConfigTest, CustomConfigValues) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  envoy::config::core::v3::TypedExtensionConfig ext_config;
  ext_config.set_name("envoy.load_balancing_policies.orca_locality");
  OrcaLocalityLbProto config_msg;
  config_msg.add_metric_names_for_computing_utilization("named_metrics.gpu");
  config_msg.mutable_update_period()->set_seconds(5);
  config_msg.mutable_ema_alpha()->set_value(0.5);
  config_msg.mutable_probe_traffic_percent()->set_value(10);
  config_msg.mutable_utilization_variance_threshold()->set_value(0.1);
  config_msg.mutable_minimum_local_percent()->set_value(30);
  config_msg.mutable_blackout_period()->set_seconds(20);
  config_msg.mutable_weight_expiration_period()->set_seconds(300);
  ext_config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(ext_config);
  auto lb_config = factory.loadConfig(context, config_msg).value();
  auto* typed = dynamic_cast<const OrcaLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(typed, nullptr);

  EXPECT_EQ(typed->locality_manager_config.metric_names_for_computing_utilization.size(), 1);
  EXPECT_EQ(typed->locality_manager_config.metric_names_for_computing_utilization[0],
            "named_metrics.gpu");
  EXPECT_EQ(typed->locality_manager_config.update_period, std::chrono::milliseconds(5000));
  EXPECT_DOUBLE_EQ(typed->locality_manager_config.ema_alpha, 0.5);
  EXPECT_EQ(typed->locality_manager_config.probe_traffic_percent, 10);
  EXPECT_DOUBLE_EQ(typed->locality_manager_config.utilization_variance_threshold, 0.1);
  EXPECT_EQ(typed->locality_manager_config.minimum_local_percent, 30);
  EXPECT_EQ(typed->locality_manager_config.blackout_period, std::chrono::milliseconds(20000));
  EXPECT_EQ(typed->locality_manager_config.weight_expiration_period,
            std::chrono::milliseconds(300000));
}

TEST(OrcaLocalityConfigTest, ChildPolicyRoundRobin) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Upstream::MockClusterInfo> cluster_info;
  NiceMock<Upstream::MockPrioritySet> main_thread_priority_set;
  NiceMock<Upstream::MockPrioritySet> thread_local_priority_set;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  OrcaLocalityLbProto config_msg;
  envoy::extensions::load_balancing_policies::round_robin::v3::RoundRobin rr_msg;
  addChildPolicy(config_msg, "envoy.load_balancing_policies.round_robin", rr_msg);

  envoy::config::core::v3::TypedExtensionConfig ext_config;
  ext_config.set_name("envoy.load_balancing_policies.orca_locality");
  ext_config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(ext_config);
  auto lb_config = factory.loadConfig(context, config_msg).value();

  auto* typed = dynamic_cast<const OrcaLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(typed, nullptr);
  EXPECT_NE(typed->child_factory_, nullptr);
  EXPECT_NE(typed->child_config_, nullptr);
  EXPECT_FALSE(typed->child_manages_orca_weights_);

  // Timers: OrcaWeightManager + OrcaLocalityManager (no child timers for RR).
  new NiceMock<Event::MockTimer>(&mock_thread_dispatcher);
  new NiceMock<Event::MockTimer>(&mock_thread_dispatcher);

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

TEST(OrcaLocalityConfigTest, ChildPolicyCswrr) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Upstream::MockClusterInfo> cluster_info;
  NiceMock<Upstream::MockPrioritySet> main_thread_priority_set;
  NiceMock<Upstream::MockPrioritySet> thread_local_priority_set;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  OrcaLocalityLbProto config_msg;
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin cswrr_msg;
  addChildPolicy(config_msg, "envoy.load_balancing_policies.client_side_weighted_round_robin",
                 cswrr_msg);

  envoy::config::core::v3::TypedExtensionConfig ext_config;
  ext_config.set_name("envoy.load_balancing_policies.orca_locality");
  ext_config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(ext_config);
  auto lb_config = factory.loadConfig(context, config_msg).value();

  auto* typed = dynamic_cast<const OrcaLocalityLbConfig*>(lb_config.get());
  ASSERT_NE(typed, nullptr);
  EXPECT_NE(typed->child_factory_, nullptr);
  EXPECT_NE(typed->child_config_, nullptr);
  EXPECT_TRUE(typed->child_manages_orca_weights_);

  // Timers: CSWRR OrcaWeightManager timer + OrcaLocalityManager timer.
  // Parent does NOT create its own OrcaWeightManager timer.
  new NiceMock<Event::MockTimer>(&mock_thread_dispatcher);
  new NiceMock<Event::MockTimer>(&mock_thread_dispatcher);

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

TEST(OrcaLocalityConfigTest, NoChildPolicyFallback) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Upstream::MockClusterInfo> cluster_info;
  NiceMock<Upstream::MockPrioritySet> main_thread_priority_set;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  // Config with no endpoint_picking_policy.
  OrcaLocalityLbProto config_msg;

  envoy::config::core::v3::TypedExtensionConfig ext_config;
  ext_config.set_name("envoy.load_balancing_policies.orca_locality");
  ext_config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(ext_config);
  auto lb_config_or = factory.loadConfig(context, config_msg);
  ASSERT_TRUE(lb_config_or.ok());

  auto* typed = dynamic_cast<const OrcaLocalityLbConfig*>(lb_config_or.value().get());
  ASSERT_NE(typed, nullptr);
  EXPECT_EQ(typed->child_factory_, nullptr);
  EXPECT_FALSE(typed->child_manages_orca_weights_);
}

TEST(OrcaLocalityConfigTest, InvalidChildPolicyReturnsError) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  OrcaLocalityLbProto config_msg;
  // Add a child policy with a name but no valid typed_config. Since
  // getAndCheckFactory resolves by the Any type URL (not the name string),
  // an empty typed_config yields nullptr and the error path triggers.
  auto* policy = config_msg.mutable_endpoint_picking_policy()->add_policies();
  policy->mutable_typed_extension_config()->set_name(
      "envoy.load_balancing_policies.nonexistent_policy");

  envoy::config::core::v3::TypedExtensionConfig ext_config;
  ext_config.set_name("envoy.load_balancing_policies.orca_locality");
  ext_config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory =
      Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(ext_config);
  auto result = factory.loadConfig(context, config_msg);
  EXPECT_FALSE(result.ok());
}

} // namespace
} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
