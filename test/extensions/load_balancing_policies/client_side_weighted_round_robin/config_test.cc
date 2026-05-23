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

using CswrrProto = envoy::extensions::load_balancing_policies::
    client_side_weighted_round_robin::v3::ClientSideWeightedRoundRobin;

class CswrrOobConfigResolution : public testing::Test {
protected:
  Upstream::ClientSideWeightedRoundRobinLbConfig makeConfig(const CswrrProto& proto) {
    return Upstream::ClientSideWeightedRoundRobinLbConfig(proto, dispatcher_, tls_);
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<ThreadLocal::MockInstance> tls_;
};

TEST_F(CswrrOobConfigResolution, EnableOobLoadReportEnablesWithPeriod) {
  CswrrProto proto;
  proto.mutable_enable_oob_load_report()->set_value(true);
  proto.mutable_oob_reporting_period()->set_seconds(42);

  auto config = makeConfig(proto);

  EXPECT_TRUE(config.oob_enabled);
  EXPECT_EQ(config.oob_manager_config.reporting_period, std::chrono::milliseconds(42000));
}

TEST_F(CswrrOobConfigResolution, OobReportingConfigOverridesParsed) {
  CswrrProto proto;
  proto.mutable_enable_oob_load_report()->set_value(true);
  proto.mutable_oob_reporting_period()->set_seconds(5);
  auto* overrides = proto.mutable_oob_reporting_config();
  overrides->set_port_value(9001);
  overrides->set_authority("orca.example.com");

  auto config = makeConfig(proto);

  EXPECT_TRUE(config.oob_enabled);
  EXPECT_EQ(config.oob_manager_config.reporting_period, std::chrono::milliseconds(5000));
  EXPECT_EQ(config.oob_manager_config.port_value, 9001u);
  EXPECT_EQ(config.oob_manager_config.authority, "orca.example.com");
}

// oob_reporting_config without enable_oob_load_report=true: accepted, overrides unused.
TEST_F(CswrrOobConfigResolution, OobReportingConfigIgnoredWhenNotEnabled) {
  CswrrProto proto;
  proto.mutable_oob_reporting_config()->set_port_value(9001);

  auto config = makeConfig(proto);

  EXPECT_FALSE(config.oob_enabled);
}

} // namespace
} // namespace ClientSideWeightedRoundRobin
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
