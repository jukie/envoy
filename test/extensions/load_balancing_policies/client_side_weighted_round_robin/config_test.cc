#include "envoy/config/core/v3/extension.pb.h"

#include "source/extensions/load_balancing_policies/client_side_weighted_round_robin/client_side_weighted_round_robin_lb.h"
#include "source/extensions/load_balancing_policies/client_side_weighted_round_robin/config.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/server/server_factory_context.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/priority_set.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace ClientSideWeightedRoundRobin {
namespace {

TEST(ClientSideWeightedRoundRobinConfigTest, ValidateFail) {
  NiceMock<Server::Configuration::MockServerFactoryContext> context;
  NiceMock<Upstream::MockClusterInfo> cluster_info;
  NiceMock<Upstream::MockPrioritySet> main_thread_priority_set;
  NiceMock<Upstream::MockPrioritySet> thread_local_priority_set;
  NiceMock<Event::MockDispatcher> mock_thread_dispatcher;
  ON_CALL(context, mainThreadDispatcher()).WillByDefault(ReturnRef(mock_thread_dispatcher));

  envoy::config::core::v3::TypedExtensionConfig config;
  config.set_name("envoy.load_balancing_policies.client_side_weighted_round_robin");
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin config_msg;
  config.mutable_typed_config()->PackFrom(config_msg);

  auto& factory = Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(config);
  EXPECT_EQ("envoy.load_balancing_policies.client_side_weighted_round_robin", factory.name());

  auto lb_config = factory.loadConfig(context, *factory.createEmptyConfigProto()).value();

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

TEST(CswrrOobConfigResolution, NewFieldEnablesAndIsParsed) {
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin proto;
  auto* oob = proto.mutable_oob_reporting_config();
  oob->mutable_reporting_period()->set_seconds(5);
  oob->set_port_value(9001);
  oob->set_authority("orca.example.com");

  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<ThreadLocal::MockInstance> tls;
  Upstream::ClientSideWeightedRoundRobinLbConfig config(proto, dispatcher, tls);

  EXPECT_TRUE(config.enable_oob_load_report);
  EXPECT_EQ(config.oob_manager_config.reporting_period, std::chrono::milliseconds(5000));
  EXPECT_EQ(config.oob_manager_config.port_value, 9001u);
  EXPECT_EQ(config.oob_manager_config.authority, "orca.example.com");
}

TEST(CswrrOobConfigResolution, DisabledTurnsOffEvenWhenPresent) {
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin proto;
  proto.mutable_oob_reporting_config()->set_disabled(true);

  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<ThreadLocal::MockInstance> tls;
  Upstream::ClientSideWeightedRoundRobinLbConfig config(proto, dispatcher, tls);

  EXPECT_FALSE(config.enable_oob_load_report);
}

TEST(CswrrOobConfigResolution, DeprecatedFieldsUsedWhenNewFieldAbsent) {
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin proto;
  proto.mutable_enable_oob_load_report()->set_value(true);
  proto.mutable_oob_reporting_period()->set_seconds(42);

  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<ThreadLocal::MockInstance> tls;
  Upstream::ClientSideWeightedRoundRobinLbConfig config(proto, dispatcher, tls);

  EXPECT_TRUE(config.enable_oob_load_report);
  EXPECT_EQ(config.oob_manager_config.reporting_period, std::chrono::milliseconds(42000));
}

TEST(CswrrOobConfigResolution, NewFieldWinsOverDeprecatedFields) {
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin proto;
  // Deprecated fields set...
  proto.mutable_enable_oob_load_report()->set_value(true);
  proto.mutable_oob_reporting_period()->set_seconds(99);
  // ...but the new field is also set and must win.
  proto.mutable_oob_reporting_config()->mutable_reporting_period()->set_seconds(5);

  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<ThreadLocal::MockInstance> tls;
  Upstream::ClientSideWeightedRoundRobinLbConfig config(proto, dispatcher, tls);

  EXPECT_TRUE(config.enable_oob_load_report);
  EXPECT_EQ(config.oob_manager_config.reporting_period, std::chrono::milliseconds(5000));
}

TEST(CswrrOobConfigResolution, EmptyConfigEnablesWithDefaults) {
  envoy::extensions::load_balancing_policies::client_side_weighted_round_robin::v3::
      ClientSideWeightedRoundRobin proto;
  proto.mutable_oob_reporting_config(); // present but empty

  NiceMock<Event::MockDispatcher> dispatcher;
  NiceMock<ThreadLocal::MockInstance> tls;
  Upstream::ClientSideWeightedRoundRobinLbConfig config(proto, dispatcher, tls);

  EXPECT_TRUE(config.enable_oob_load_report);
  EXPECT_EQ(config.oob_manager_config.reporting_period, std::chrono::milliseconds(10000));
  EXPECT_EQ(config.oob_manager_config.port_value, 0u);
}

} // namespace
} // namespace ClientSideWeightedRoundRobin
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
