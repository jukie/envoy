#include "source/common/network/transport_socket_options_impl.h"
#include "source/common/network/utility.h"
#include "source/common/router/string_accessor_impl.h"
#include "source/common/stream_info/filter_state_impl.h"
#include "source/extensions/clusters/common/logical_host.h"

#include "test/mocks/event/mocks.h"
#include "test/mocks/network/connection.h"
#include "test/mocks/network/transport_socket.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/transport_socket_match.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;
using testing::StrictMock;

namespace Envoy {
namespace Extensions {
namespace Clusters {

class RealHostDescriptionTest : public testing::Test {
public:
  Network::Address::InstanceConstSharedPtr address_ = nullptr;
  Upstream::MockHost* mock_host_{new NiceMock<Upstream::MockHost>()};
  Upstream::HostConstSharedPtr host_{mock_host_};
  Upstream::RealHostDescription description_{address_, host_};
};

TEST_F(RealHostDescriptionTest, UnitTest) {
  // No-op unit tests.
  description_.canary();
  description_.metadata();
  description_.priority();
  EXPECT_EQ(nullptr, description_.healthCheckAddress());

  // Pass through functions.
  EXPECT_CALL(*mock_host_, transportSocketFactory());
  description_.transportSocketFactory();

  EXPECT_CALL(*mock_host_, canCreateConnection(_));
  description_.canCreateConnection(Upstream::ResourcePriority::Default);

  EXPECT_CALL(*mock_host_, loadMetricStats());
  description_.loadMetricStats();

  EXPECT_CALL(*mock_host_, addressListOrNull())
      .WillOnce(Return(std::make_shared<Upstream::HostDescription::AddressVector>()));
  description_.addressListOrNull();

  const envoy::config::core::v3::Metadata metadata;
  const envoy::config::cluster::v3::Cluster cluster;
  Network::MockTransportSocketFactory socket_factory;
  EXPECT_CALL(*mock_host_, resolveTransportSocketFactory(_, _, _))
      .WillOnce(ReturnRef(socket_factory));
  description_.resolveTransportSocketFactory(address_, &metadata, nullptr);

  EXPECT_CALL(*mock_host_, lbPolicyDataCount());
  description_.lbPolicyDataCount();

  EXPECT_CALL(*mock_host_, lbPolicyDataAt(0));
  description_.lbPolicyDataAt(0);

  description_.canary(false);
  description_.priority(0);
  description_.metadata(nullptr);
  description_.setLastHcPassTime(MonotonicTime());
  description_.addLbPolicyData(nullptr);

  Upstream::HealthCheckHostMonitorPtr heath_check_monitor;
  description_.setHealthChecker(std::move(heath_check_monitor));

  Upstream::Outlier::DetectorHostMonitorPtr detector_host;
  description_.setOutlierDetector(std::move(detector_host));
}

// Test fixture for LogicalHost per-connection transport socket resolution.
class LogicalHostTransportSocketResolutionTest : public testing::Test {
public:
  void SetUp() override {
    cluster_info_ = std::make_shared<NiceMock<Upstream::MockClusterInfo>>();
    transport_socket_matcher_ = dynamic_cast<Upstream::MockTransportSocketMatcher*>(
        cluster_info_->transport_socket_matcher_.get());
    ASSERT_NE(transport_socket_matcher_, nullptr);
  }

  Network::TransportSocketOptionsConstSharedPtr
  createTransportSocketOptionsWithFilterState(const std::string& key, const std::string& value) {
    auto filter_state = std::make_shared<StreamInfo::FilterStateImpl>(
        StreamInfo::FilterState::LifeSpan::Connection);
    auto string_accessor = std::make_shared<Router::StringAccessorImpl>(value);
    filter_state->setData(key, string_accessor, StreamInfo::FilterState::StateType::ReadOnly,
                          StreamInfo::FilterState::LifeSpan::Connection,
                          StreamInfo::StreamSharingMayImpactPooling::SharedWithUpstreamConnection);
    auto shared_objects = filter_state->objectsSharedWithUpstreamConnection();
    return std::make_shared<Network::TransportSocketOptionsImpl>(
        "", std::vector<std::string>{}, std::vector<std::string>{}, std::vector<std::string>{},
        absl::nullopt, std::move(shared_objects));
  }

  // Helper to compute the per-connection resolution condition as used in LogicalHost.
  bool needsPerConnectionResolution(Network::TransportSocketOptionsConstSharedPtr options) {
    return cluster_info_->transportSocketMatcher().usesFilterState() && options &&
           !options->downstreamSharedFilterStateObjects().empty();
  }

  std::shared_ptr<NiceMock<Upstream::MockClusterInfo>> cluster_info_;
  Upstream::MockTransportSocketMatcher* transport_socket_matcher_;
};

// Test that per-connection resolution is triggered when all conditions are met.
TEST_F(LogicalHostTransportSocketResolutionTest, PerConnectionResolutionWhenAllConditionsMet) {
  ON_CALL(*transport_socket_matcher_, usesFilterState()).WillByDefault(Return(true));
  auto options =
      createTransportSocketOptionsWithFilterState("envoy.network.namespace", "/run/netns/ns1");

  EXPECT_TRUE(needsPerConnectionResolution(options));
}

// Test that per-connection resolution is not triggered when usesFilterState returns false.
TEST_F(LogicalHostTransportSocketResolutionTest,
       NoPerConnectionResolutionWhenUsesFilterStateFalse) {
  ON_CALL(*transport_socket_matcher_, usesFilterState()).WillByDefault(Return(false));
  auto options =
      createTransportSocketOptionsWithFilterState("envoy.network.namespace", "/run/netns/ns1");

  EXPECT_FALSE(needsPerConnectionResolution(options));
}

// Test that per-connection resolution is not triggered when transport socket options are null.
TEST_F(LogicalHostTransportSocketResolutionTest, NoPerConnectionResolutionWhenOptionsNull) {
  ON_CALL(*transport_socket_matcher_, usesFilterState()).WillByDefault(Return(true));
  Network::TransportSocketOptionsConstSharedPtr options = nullptr;

  EXPECT_FALSE(needsPerConnectionResolution(options));
}

// Test that per-connection resolution is not triggered when filter state objects are empty.
TEST_F(LogicalHostTransportSocketResolutionTest,
       NoPerConnectionResolutionWhenFilterStateObjectsEmpty) {
  ON_CALL(*transport_socket_matcher_, usesFilterState()).WillByDefault(Return(true));
  auto options = std::make_shared<Network::TransportSocketOptionsImpl>(
      "", std::vector<std::string>{}, std::vector<std::string>{}, std::vector<std::string>{});

  EXPECT_FALSE(needsPerConnectionResolution(options));
}

// Test that override transport socket options takes precedence over passed options.
TEST_F(LogicalHostTransportSocketResolutionTest, OverrideTransportSocketOptionsTakesPrecedence) {
  ON_CALL(*transport_socket_matcher_, usesFilterState()).WillByDefault(Return(true));

  // Create override options with filter state.
  auto override_options =
      createTransportSocketOptionsWithFilterState("envoy.network.namespace", "/run/netns/ns1");

  // Create passed options without filter state.
  auto passed_options = std::make_shared<Network::TransportSocketOptionsImpl>(
      "", std::vector<std::string>{}, std::vector<std::string>{}, std::vector<std::string>{});

  // Simulate LogicalHost's effective_options logic: use override if not null.
  const auto& effective_options = override_options != nullptr ? override_options : passed_options;

  EXPECT_TRUE(needsPerConnectionResolution(effective_options));
}

// Test fixture for end-to-end exercise of LogicalHost::createOrcaReportingConnection. Constructs a
// real LogicalHost backed by a NiceMock<MockClusterInfo> so we can drive the
// transport-socket-matcher resolve() expectations and observe the dispatcher's
// createClientConnection_ calls.
class LogicalHostOrcaReportingConnectionTest : public testing::Test {
public:
  void SetUp() override {
    cluster_info_ = std::make_shared<NiceMock<Upstream::MockClusterInfo>>();
    transport_socket_matcher_ = dynamic_cast<Upstream::MockTransportSocketMatcher*>(
        cluster_info_->transport_socket_matcher_.get());
    ASSERT_NE(transport_socket_matcher_, nullptr);
    address_ = *Network::Utility::resolveUrl("tcp://10.0.0.1:1234");
  }

  Upstream::LogicalHostSharedPtr makeLogicalHost(
      const Network::TransportSocketOptionsConstSharedPtr& override_transport_socket_options) {
    envoy::config::endpoint::v3::LocalityLbEndpoints locality_lb_endpoints;
    envoy::config::endpoint::v3::LbEndpoint lb_endpoint;
    auto host_or_error = Upstream::LogicalHost::create(
        cluster_info_, "lyft.com", address_, /*address_list=*/{}, locality_lb_endpoints,
        lb_endpoint, override_transport_socket_options);
    EXPECT_TRUE(host_or_error.ok());
    return std::shared_ptr<Upstream::LogicalHost>(std::move(host_or_error.value()));
  }

  std::shared_ptr<NiceMock<Upstream::MockClusterInfo>> cluster_info_;
  Upstream::MockTransportSocketMatcher* transport_socket_matcher_;
  Network::Address::InstanceConstSharedPtr address_;
};

// Verifies that LogicalHost::createOrcaReportingConnection forwards the override transport socket
// options (set at LogicalHost construction time) into the resolve() call rather than the
// caller-supplied options. This mirrors LogicalHost::createConnection's existing override
// precedence behavior.
TEST_F(LogicalHostOrcaReportingConnectionTest, OverrideTransportSocketOptionsArePreferred) {
  // Construct override options with filter state so that the (no-metadata) per-connection
  // resolution branch fires and we can observe which options the matcher receives.
  ON_CALL(*transport_socket_matcher_, usesFilterState()).WillByDefault(Return(true));
  auto filter_state = std::make_shared<StreamInfo::FilterStateImpl>(
      StreamInfo::FilterState::LifeSpan::Connection);
  filter_state->setData("envoy.network.namespace",
                        std::make_shared<Router::StringAccessorImpl>("/run/netns/override"),
                        StreamInfo::FilterState::StateType::ReadOnly,
                        StreamInfo::FilterState::LifeSpan::Connection,
                        StreamInfo::StreamSharingMayImpactPooling::SharedWithUpstreamConnection);
  Network::TransportSocketOptionsConstSharedPtr override_options =
      std::make_shared<Network::TransportSocketOptionsImpl>(
          "", std::vector<std::string>{}, std::vector<std::string>{}, std::vector<std::string>{},
          absl::nullopt, filter_state->objectsSharedWithUpstreamConnection());

  // The caller-supplied options must NOT be the ones forwarded; LogicalHost should prefer the
  // override.
  Network::TransportSocketOptionsConstSharedPtr caller_options =
      std::make_shared<Network::TransportSocketOptionsImpl>(
          "caller-name", std::vector<std::string>{}, std::vector<std::string>{},
          std::vector<std::string>{});

  auto host = makeLogicalHost(override_options);

  // Expect resolve() to be called with the override options pointer (not caller_options) since
  // metadata is null but per-connection resolution kicks in via filter-state-bearing
  // effective_options.
  EXPECT_CALL(*transport_socket_matcher_, resolve(_, _, override_options))
      .WillOnce(Return(Upstream::TransportSocketMatcher::MatchData(
          *transport_socket_matcher_->socket_factory_, transport_socket_matcher_->stats_,
          "override-options")));

  NiceMock<Event::MockDispatcher> dispatcher;
  auto* connection = new NiceMock<Network::MockClientConnection>();
  // Critical: ORCA OOB connection must target the host's data address (10.0.0.1:1234) -- not a
  // health-check or override address.
  EXPECT_CALL(dispatcher, createClientConnection_(address_, _, _, _)).WillOnce(Return(connection));

  auto data = host->createOrcaReportingConnection(dispatcher, caller_options, /*metadata=*/nullptr);
  EXPECT_EQ(connection, data.connection_.get());
  // The host_description_ should be a RealHostDescription wrapping the logical host.
  ASSERT_NE(data.host_description_, nullptr);
  EXPECT_EQ(address_->asString(), data.host_description_->address()->asString());
}

// Verifies that when metadata is supplied, LogicalHost::createOrcaReportingConnection routes
// transport socket factory selection through the matcher's resolve() with that metadata,
// regardless of the per-connection-resolution heuristics.
TEST_F(LogicalHostOrcaReportingConnectionTest, MetadataDrivesTransportSocketResolution) {
  auto host = makeLogicalHost(/*override_transport_socket_options=*/nullptr);

  envoy::config::core::v3::Metadata orca_metadata;
  auto& fields = (*orca_metadata.mutable_filter_metadata())["envoy.test.orca"];
  (*fields.mutable_fields())["key"].set_string_value("value");

  Network::TransportSocketOptionsConstSharedPtr caller_options =
      std::make_shared<Network::TransportSocketOptionsImpl>(
          "caller-name", std::vector<std::string>{}, std::vector<std::string>{},
          std::vector<std::string>{});

  // Expect the matcher to see the supplied metadata pointer and the caller's options.
  EXPECT_CALL(*transport_socket_matcher_, resolve(&orca_metadata, _, caller_options))
      .WillOnce(Return(Upstream::TransportSocketMatcher::MatchData(
          *transport_socket_matcher_->socket_factory_, transport_socket_matcher_->stats_,
          "metadata-resolved")));

  NiceMock<Event::MockDispatcher> dispatcher;
  auto* connection = new NiceMock<Network::MockClientConnection>();
  EXPECT_CALL(dispatcher, createClientConnection_(address_, _, _, _)).WillOnce(Return(connection));

  auto data = host->createOrcaReportingConnection(dispatcher, caller_options, &orca_metadata);
  EXPECT_EQ(connection, data.connection_.get());
}

} // namespace Clusters
} // namespace Extensions
} // namespace Envoy
