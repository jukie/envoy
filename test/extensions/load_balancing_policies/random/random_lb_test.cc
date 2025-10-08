#include "source/extensions/load_balancing_policies/random/config.h"
#include "source/extensions/load_balancing_policies/random/random_lb.h"

#include "test/extensions/load_balancing_policies/common/load_balancer_impl_base_test.h"
#include "test/mocks/stream_info/mocks.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Upstream {
namespace {

using testing::Return;

using ZoneAwareHostLbPolicyData = ZoneAwareLoadBalancerBase::ZoneAwareHostLbPolicyData;

class RandomLoadBalancerTest : public LoadBalancerTestBase {
public:
  void init() {
    lb_ = std::make_shared<RandomLoadBalancer>(priority_set_, nullptr, stats_, runtime_, random_,
                                               50, config_);
  }

  envoy::extensions::load_balancing_policies::random::v3::Random config_;
  std::shared_ptr<LoadBalancer> lb_;
};

TEST_P(RandomLoadBalancerTest, NoHosts) {
  init();

  EXPECT_EQ(nullptr, lb_->peekAnotherHost(nullptr));
  EXPECT_EQ(nullptr, lb_->chooseHost(nullptr).host);
}

TEST_P(RandomLoadBalancerTest, Normal) {
  init();
  hostSet().healthy_hosts_ = {makeTestHost(info_, "tcp://127.0.0.1:80"),
                              makeTestHost(info_, "tcp://127.0.0.1:81")};
  hostSet().hosts_ = hostSet().healthy_hosts_;
  hostSet().runCallbacks({}, {}); // Trigger callbacks. The added/removed lists are not relevant.

  EXPECT_CALL(random_, random()).WillOnce(Return(2));
  EXPECT_EQ(hostSet().healthy_hosts_[0], lb_->peekAnotherHost(nullptr));

  EXPECT_CALL(random_, random()).WillOnce(Return(3));
  EXPECT_EQ(hostSet().healthy_hosts_[1], lb_->peekAnotherHost(nullptr));

  EXPECT_CALL(random_, random()).Times(0);
  EXPECT_EQ(hostSet().healthy_hosts_[0], lb_->chooseHost(nullptr).host);
  EXPECT_EQ(hostSet().healthy_hosts_[1], lb_->chooseHost(nullptr).host);
}

TEST_P(RandomLoadBalancerTest, FailClusterOnPanic) {
  config_.mutable_locality_lb_config()->mutable_zone_aware_lb_config()->set_fail_traffic_on_panic(
      true);
  init();

  hostSet().healthy_hosts_ = {};
  hostSet().hosts_ = {makeTestHost(info_, "tcp://127.0.0.1:80"),
                      makeTestHost(info_, "tcp://127.0.0.1:81")};
  hostSet().runCallbacks({}, {}); // Trigger callbacks. The added/removed lists are not relevant.
  EXPECT_EQ(nullptr, lb_->chooseHost(nullptr).host);
}

INSTANTIATE_TEST_SUITE_P(PrimaryOrFailoverAndLegacyOrNew, RandomLoadBalancerTest,
                         ::testing::Values(LoadBalancerTestParam{true},
                                           LoadBalancerTestParam{false}));

class RandomZoneAwareLoadBalancerTest : public LoadBalancerTestBase {
public:
  void init() {
    local_priority_set_ = std::make_shared<PrioritySetImpl>();
    local_priority_set_->getOrCreateHostSet(0);
    lb_ = std::make_shared<RandomLoadBalancer>(priority_set_, local_priority_set_.get(), stats_,
                                               runtime_, random_, 50, config_);
  }

  void updateHosts(HostVectorConstSharedPtr hosts,
                   HostsPerLocalityConstSharedPtr hosts_per_locality) {
    local_priority_set_->updateHosts(
        0,
        updateHostsParams(hosts, hosts_per_locality,
                          std::make_shared<const HealthyHostVector>(*hosts), hosts_per_locality),
        {}, empty_host_vector_, empty_host_vector_, random_.random(), absl::nullopt);
  }

  envoy::extensions::load_balancing_policies::random::v3::Random config_;
  std::shared_ptr<PrioritySetImpl> local_priority_set_;
  std::shared_ptr<RandomLoadBalancer> lb_;
  HostsPerLocalityConstSharedPtr empty_locality_;
  HostVector empty_host_vector_;
};

TEST_P(RandomZoneAwareLoadBalancerTest, OrcaLocalityPrefersLocalCapacity) {
  envoy::config::core::v3::Locality zone_a;
  zone_a.set_zone("zone_a");
  envoy::config::core::v3::Locality zone_b;
  zone_b.set_zone("zone_b");

  HostVectorSharedPtr upstream_hosts(new HostVector{
      makeTestHost(info_, "tcp://10.0.0.1:80", zone_a),
      makeTestHost(info_, "tcp://10.0.0.2:80", zone_b)});
  HostsPerLocalitySharedPtr upstream_per_locality =
      makeHostsPerLocality({{(*upstream_hosts)[0]}, {(*upstream_hosts)[1]}});
  hostSet().hosts_ = *upstream_hosts;
  hostSet().healthy_hosts_ = *upstream_hosts;
  hostSet().healthy_hosts_per_locality_ = upstream_per_locality;

  auto* orca_config = config_.mutable_locality_lb_config()
                          ->mutable_zone_aware_lb_config()
                          ->mutable_orca_locality_routing();
  orca_config->mutable_min_reported_endpoints()->set_value(1);

  init();

  HostVectorSharedPtr local_hosts(new HostVector{
      makeTestHost(info_, "tcp://127.0.0.1:1000", zone_a),
      makeTestHost(info_, "tcp://127.0.0.1:2000", zone_b)});
  HostsPerLocalitySharedPtr local_per_locality =
      makeHostsPerLocality({{(*local_hosts)[0]}, {(*local_hosts)[1]}});

  // Attach ORCA policy data to all hosts since they're created after load balancer initialization
  for (const auto& host : *upstream_hosts) {
    host->setLbPolicyData(std::make_unique<ZoneAwareHostLbPolicyData>(std::vector<std::string>()));
  }
  for (const auto& host : *local_hosts) {
    host->setLbPolicyData(std::make_unique<ZoneAwareHostLbPolicyData>(std::vector<std::string>()));
  }

  updateHosts(local_hosts, local_per_locality);

  StreamInfo::MockStreamInfo stream_info;
  xds::data::orca::v3::OrcaLoadReport low_utilization;
  low_utilization.set_application_utilization(0.1);
  xds::data::orca::v3::OrcaLoadReport high_utilization;
  high_utilization.set_application_utilization(0.9);

  auto apply_report = [&](const HostSharedPtr& host,
                          const xds::data::orca::v3::OrcaLoadReport& report) {
    auto policy_data = host->typedLbPolicyData<ZoneAwareHostLbPolicyData>();
    ASSERT_TRUE(policy_data.has_value());
    EXPECT_TRUE(policy_data->onOrcaLoadReport(report, stream_info).ok());
  };

  for (const auto& host : upstream_per_locality->get()[0]) {
    apply_report(host, low_utilization);
  }
  for (const auto& host : upstream_per_locality->get()[1]) {
    apply_report(host, high_utilization);
  }

  auto& local_host_set = *local_priority_set_->hostSetsPerPriority()[0];
  for (const auto& host : local_host_set.healthyHostsPerLocality().get()[0]) {
    apply_report(host, low_utilization);
  }
  for (const auto& host : local_host_set.healthyHostsPerLocality().get()[1]) {
    apply_report(host, high_utilization);
  }

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 6))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));

  EXPECT_CALL(random_, random()).WillRepeatedly(Return(0));
  EXPECT_EQ(upstream_per_locality->get()[0][0], lb_->chooseHost(nullptr).host);
}

TEST_P(RandomZoneAwareLoadBalancerTest, OrcaLocalityRoutesResidualWhenLocalSaturated) {
  envoy::config::core::v3::Locality zone_a;
  zone_a.set_zone("zone_a");
  envoy::config::core::v3::Locality zone_b;
  zone_b.set_zone("zone_b");

  HostVectorSharedPtr upstream_hosts(new HostVector{
      makeTestHost(info_, "tcp://10.0.0.1:80", zone_a),
      makeTestHost(info_, "tcp://10.0.0.2:80", zone_b)});
  HostsPerLocalitySharedPtr upstream_per_locality =
      makeHostsPerLocality({{(*upstream_hosts)[0]}, {(*upstream_hosts)[1]}});
  hostSet().hosts_ = *upstream_hosts;
  hostSet().healthy_hosts_ = *upstream_hosts;
  hostSet().healthy_hosts_per_locality_ = upstream_per_locality;

  auto* orca_config = config_.mutable_locality_lb_config()
                          ->mutable_zone_aware_lb_config()
                          ->mutable_orca_locality_routing();
  orca_config->mutable_min_reported_endpoints()->set_value(1);

  init();

  HostVectorSharedPtr local_hosts(new HostVector{
      makeTestHost(info_, "tcp://127.0.0.1:1000", zone_a),
      makeTestHost(info_, "tcp://127.0.0.1:2000", zone_b)});
  HostsPerLocalitySharedPtr local_per_locality =
      makeHostsPerLocality({{(*local_hosts)[0]}, {(*local_hosts)[1]}});

  // Attach ORCA policy data to all hosts since they're created after load balancer initialization
  for (const auto& host : *upstream_hosts) {
    host->setLbPolicyData(std::make_unique<ZoneAwareHostLbPolicyData>(std::vector<std::string>()));
  }
  for (const auto& host : *local_hosts) {
    host->setLbPolicyData(std::make_unique<ZoneAwareHostLbPolicyData>(std::vector<std::string>()));
  }

  updateHosts(local_hosts, local_per_locality);

  StreamInfo::MockStreamInfo stream_info;
  xds::data::orca::v3::OrcaLoadReport saturated_utilization;
  saturated_utilization.set_application_utilization(1.0);
  xds::data::orca::v3::OrcaLoadReport available_utilization;
  available_utilization.set_application_utilization(0.1);

  auto apply_report = [&](const HostSharedPtr& host,
                          const xds::data::orca::v3::OrcaLoadReport& report) {
    auto policy_data = host->typedLbPolicyData<ZoneAwareHostLbPolicyData>();
    ASSERT_TRUE(policy_data.has_value());
    EXPECT_TRUE(policy_data->onOrcaLoadReport(report, stream_info).ok());
  };

  for (const auto& host : upstream_per_locality->get()[0]) {
    apply_report(host, saturated_utilization);
  }
  for (const auto& host : upstream_per_locality->get()[1]) {
    apply_report(host, available_utilization);
  }

  auto& local_host_set = *local_priority_set_->hostSetsPerPriority()[0];
  for (const auto& host : local_host_set.healthyHostsPerLocality().get()[0]) {
    apply_report(host, saturated_utilization);
  }
  for (const auto& host : local_host_set.healthyHostsPerLocality().get()[1]) {
    apply_report(host, available_utilization);
  }

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 6))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));

  EXPECT_CALL(random_, random()).WillRepeatedly(Return(0));
  EXPECT_EQ(upstream_per_locality->get()[1][0], lb_->chooseHost(nullptr).host);
}

INSTANTIATE_TEST_SUITE_P(PrimaryOrFailoverAndLegacyOrNew, RandomZoneAwareLoadBalancerTest,
                         ::testing::Values(LoadBalancerTestParam{true},
                                           LoadBalancerTestParam{false}));

TEST(TypedRandomLbConfigTest, TypedRandomLbConfigTest) {
  {
    envoy::config::cluster::v3::Cluster::CommonLbConfig common;

    Extensions::LoadBalancingPolices::Random::TypedRandomLbConfig typed_config(common);

    EXPECT_FALSE(typed_config.lb_config_.has_locality_lb_config());
  }

  {
    envoy::config::cluster::v3::Cluster::CommonLbConfig common;

    common.mutable_locality_weighted_lb_config();

    Extensions::LoadBalancingPolices::Random::TypedRandomLbConfig typed_config(common);

    EXPECT_TRUE(typed_config.lb_config_.has_locality_lb_config());
    EXPECT_TRUE(typed_config.lb_config_.locality_lb_config().has_locality_weighted_lb_config());
    EXPECT_FALSE(typed_config.lb_config_.locality_lb_config().has_zone_aware_lb_config());
  }

  {
    envoy::config::cluster::v3::Cluster::CommonLbConfig common;

    common.mutable_zone_aware_lb_config()->mutable_min_cluster_size()->set_value(3);
    common.mutable_zone_aware_lb_config()->mutable_routing_enabled()->set_value(23.0);
    common.mutable_zone_aware_lb_config()->set_fail_traffic_on_panic(true);

    Extensions::LoadBalancingPolices::Random::TypedRandomLbConfig typed_config(common);

    EXPECT_TRUE(typed_config.lb_config_.has_locality_lb_config());
    EXPECT_FALSE(typed_config.lb_config_.locality_lb_config().has_locality_weighted_lb_config());
    EXPECT_TRUE(typed_config.lb_config_.locality_lb_config().has_zone_aware_lb_config());

    const auto& zone_aware_lb_config =
        typed_config.lb_config_.locality_lb_config().zone_aware_lb_config();
    EXPECT_EQ(zone_aware_lb_config.min_cluster_size().value(), 3);
    EXPECT_DOUBLE_EQ(zone_aware_lb_config.routing_enabled().value(), 23.0);
    EXPECT_TRUE(zone_aware_lb_config.fail_traffic_on_panic());
  }
}

} // namespace
} // namespace Upstream
} // namespace Envoy
