#include "test/extensions/load_balancing_policies/common/load_balancer_impl_base_test.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Upstream {
namespace {

using testing::ElementsAre;
using testing::Return;
using testing::ReturnRef;

class TestLb : public LoadBalancerBase {
public:
  TestLb(const PrioritySet& priority_set, ClusterLbStats& lb_stats, Runtime::Loader& runtime,
         Random::RandomGenerator& random,
         const envoy::config::cluster::v3::Cluster::CommonLbConfig& common_config)
      : LoadBalancerBase(priority_set, lb_stats, runtime, random,
                         PROTOBUF_PERCENT_TO_ROUNDED_INTEGER_OR_DEFAULT(
                             common_config, healthy_panic_threshold, 100, 50)) {}
  using LoadBalancerBase::chooseHostSet;
  using LoadBalancerBase::isInPanic;
  using LoadBalancerBase::percentageDegradedLoad;
  using LoadBalancerBase::percentageLoad;

  HostSelectionResponse chooseHost(LoadBalancerContext*) override { PANIC("not implemented"); }

  HostConstSharedPtr peekAnotherHost(LoadBalancerContext*) override { PANIC("not implemented"); }
};

class LoadBalancerBaseTest : public LoadBalancerTestBase {
public:
  void updateHostSet(MockHostSet& host_set, uint32_t num_hosts, uint32_t num_healthy_hosts,
                     uint32_t num_degraded_hosts = 0, uint32_t num_excluded_hosts = 0) {
    ASSERT(num_healthy_hosts + num_degraded_hosts + num_excluded_hosts <= num_hosts);

    host_set.hosts_.clear();
    host_set.healthy_hosts_.clear();
    host_set.degraded_hosts_.clear();
    host_set.excluded_hosts_.clear();
    for (uint32_t i = 0; i < num_hosts; ++i) {
      host_set.hosts_.push_back(makeTestHost(info_, "tcp://127.0.0.1:80"));
    }
    uint32_t i = 0;
    for (; i < num_healthy_hosts; ++i) {
      host_set.healthy_hosts_.push_back(host_set.hosts_[i]);
    }
    for (; i < (num_healthy_hosts + num_degraded_hosts); ++i) {
      host_set.degraded_hosts_.push_back(host_set.hosts_[i]);
    }

    for (; i < (num_healthy_hosts + num_degraded_hosts + num_excluded_hosts); ++i) {
      host_set.excluded_hosts_.push_back(host_set.hosts_[i]);
    }
    host_set.runCallbacks({}, {});
  }

  template <typename T, typename FUNC>
  std::vector<T> aggregatePrioritySetsValues(TestLb& lb, FUNC func) {
    std::vector<T> ret;

    for (size_t i = 0; i < priority_set_.host_sets_.size(); ++i) {
      ret.push_back((lb.*func)(i));
    }

    return ret;
  }

  std::vector<uint32_t> getLoadPercentage() {
    return aggregatePrioritySetsValues<uint32_t>(lb_, &TestLb::percentageLoad);
  }

  std::vector<uint32_t> getDegradedLoadPercentage() {
    return aggregatePrioritySetsValues<uint32_t>(lb_, &TestLb::percentageDegradedLoad);
  }

  std::vector<bool> getPanic() {
    return aggregatePrioritySetsValues<bool>(lb_, &TestLb::isInPanic);
  }

  envoy::config::cluster::v3::Cluster::CommonLbConfig common_config_;
  TestLb lb_{priority_set_, stats_, runtime_, random_, common_config_};
};

INSTANTIATE_TEST_SUITE_P(PrimaryOrFailoverAndLegacyOrNew, LoadBalancerBaseTest,
                         ::testing::Values(LoadBalancerTestParam{true},
                                           LoadBalancerTestParam{false}));

// Basic test of host set selection.
TEST_P(LoadBalancerBaseTest, PrioritySelection) {
  NiceMock<Upstream::MockLoadBalancerContext> context;
  updateHostSet(host_set_, 1 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 1, 0);

  HealthyAndDegradedLoad priority_load{Upstream::HealthyLoad({100, 0, 0}),
                                       Upstream::DegradedLoad({0, 0, 0})};
  EXPECT_CALL(context, determinePriorityLoad(_, _, _)).WillRepeatedly(ReturnRef(priority_load));
  // Primary and failover are in panic mode. Load distribution is based
  // on the number of hosts regardless of their health.
  EXPECT_EQ(50, lb_.percentageLoad(0));
  EXPECT_EQ(50, lb_.percentageLoad(1));
  EXPECT_EQ(&host_set_, &lb_.chooseHostSet(&context, 0).first);

  // Modify number of hosts in failover, but leave them in the unhealthy state
  // primary and secondary are in panic mode, so load distribution is
  // based on number of host regardless of their health.
  updateHostSet(failover_host_set_, 2, 0);
  EXPECT_EQ(34, lb_.percentageLoad(0));
  EXPECT_EQ(66, lb_.percentageLoad(1));
  EXPECT_EQ(&host_set_, &lb_.chooseHostSet(&context, 0).first);

  // Update the priority set with a new priority level P=2 and ensure the host
  // is chosen
  MockHostSet& tertiary_host_set_ = *priority_set_.getMockHostSet(2);
  updateHostSet(tertiary_host_set_, 1 /* num_hosts */, 1 /* num_healthy_hosts */);
  EXPECT_EQ(0, lb_.percentageLoad(0));
  EXPECT_EQ(0, lb_.percentageLoad(1));
  EXPECT_EQ(100, lb_.percentageLoad(2));
  priority_load.healthy_priority_load_ = HealthyLoad({0u, 0u, 100});
  EXPECT_EQ(&tertiary_host_set_, &lb_.chooseHostSet(&context, 0).first);

  // Now add a healthy host in P=0 and make sure it is immediately selected.
  updateHostSet(host_set_, 1 /* num_hosts */, 1 /* num_healthy_hosts */);
  host_set_.healthy_hosts_ = host_set_.hosts_;
  host_set_.runCallbacks({}, {});
  EXPECT_EQ(100, lb_.percentageLoad(0));
  EXPECT_EQ(0, lb_.percentageLoad(2));
  priority_load.healthy_priority_load_ = HealthyLoad({100u, 0u, 0u});
  EXPECT_EQ(&host_set_, &lb_.chooseHostSet(&context, 0).first);

  // Remove the healthy host and ensure we fail back over to tertiary_host_set_
  updateHostSet(host_set_, 1 /* num_hosts */, 0 /* num_healthy_hosts */);
  EXPECT_EQ(0, lb_.percentageLoad(0));
  EXPECT_EQ(100, lb_.percentageLoad(2));
  priority_load.healthy_priority_load_ = HealthyLoad({0u, 0u, 100});
  EXPECT_EQ(&tertiary_host_set_, &lb_.chooseHostSet(&context, 0).first);
}

// Tests host selection with a randomized number of healthy, degraded and unhealthy hosts.
TEST_P(LoadBalancerBaseTest, PrioritySelectionFuzz) {
  TestRandomGenerator rand;

  // Determine total number of hosts.
  const auto total_hosts = 1 + (rand.random() % 10);

  NiceMock<Upstream::MockLoadBalancerContext> context;

  const auto host_set_hosts = rand.random() % total_hosts;

  if (host_set_hosts == 0) {
    updateHostSet(host_set_, 0, 0);
  } else {
    // We get on average 50% healthy hosts, 25% degraded hosts and 25% unhealthy hosts.
    const auto healthy_hosts = rand.random() % host_set_hosts;
    const auto degraded_hosts = rand.random() % (host_set_hosts - healthy_hosts);
    const auto unhealthy_hosts = host_set_hosts - healthy_hosts - degraded_hosts;

    updateHostSet(host_set_, host_set_hosts, unhealthy_hosts, degraded_hosts);
  }

  const auto failover_set_hosts = total_hosts - host_set_hosts;

  if (host_set_hosts == 0) {
    updateHostSet(failover_host_set_, 0, 0);
  } else {
    // We get on average 50% healthy hosts, 25% degraded hosts and 25% unhealthy hosts.
    const auto healthy_hosts = rand.random() % failover_set_hosts;
    const auto degraded_hosts = rand.random() % (failover_set_hosts - healthy_hosts);
    const auto unhealthy_hosts = failover_set_hosts - healthy_hosts - degraded_hosts;

    updateHostSet(failover_host_set_, failover_set_hosts, unhealthy_hosts, degraded_hosts);
  }

  EXPECT_CALL(context, determinePriorityLoad(_, _, _))
      .WillRepeatedly(
          Invoke([](const auto&, const auto& original_load,
                    const auto&) -> const HealthyAndDegradedLoad& { return original_load; }));

  for (uint64_t i = 0; i < total_hosts; ++i) {
    const auto hs = lb_.chooseHostSet(&context, 0);
    switch (hs.second) {
    case LoadBalancerBase::HostAvailability::Healthy:
      // Either we selected one of the healthy hosts or we failed to select anything and
      // defaulted to healthy.
      EXPECT_TRUE(!hs.first.healthyHosts().empty() ||
                  (hs.first.healthyHosts().empty() && hs.first.degradedHosts().empty()));
      break;
    case LoadBalancerBase::HostAvailability::Degraded:
      EXPECT_FALSE(hs.first.degradedHosts().empty());
      break;
    }
  }
}

// Test of host set selection with priority filter
TEST_P(LoadBalancerBaseTest, PrioritySelectionWithFilter) {
  NiceMock<Upstream::MockLoadBalancerContext> context;

  HealthyAndDegradedLoad priority_load{Upstream::HealthyLoad({0u, 100u}),
                                       Upstream::DegradedLoad({0, 0})};
  // return a filter that excludes priority 0
  EXPECT_CALL(context, determinePriorityLoad(_, _, _)).WillRepeatedly(ReturnRef(priority_load));

  updateHostSet(host_set_, 1 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 1, 1);

  // Since we've excluded P0, we should pick the failover host set
  EXPECT_EQ(failover_host_set_.priority(), lb_.chooseHostSet(&context, 0).first.priority());

  updateHostSet(host_set_, 1 /* num_hosts */, 0 /* num_healthy_hosts */,
                1 /* num_degraded_hosts */);
  updateHostSet(failover_host_set_, 1, 0, 1);

  // exclude priority 0 for degraded hosts
  priority_load.healthy_priority_load_ = Upstream::HealthyLoad({0, 0});
  priority_load.degraded_priority_load_ = Upstream::DegradedLoad({0, 100});

  // Since we've excluded P0, we should pick the failover host set
  EXPECT_EQ(failover_host_set_.priority(), lb_.chooseHostSet(&context, 0).first.priority());
}

TEST_P(LoadBalancerBaseTest, OverProvisioningFactor) {
  // Default overprovisioning factor 1.4 makes P0 receives 70% load.
  updateHostSet(host_set_, 4, 2);
  updateHostSet(failover_host_set_, 4, 2);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(70, 30));

  // Set overprovisioning factor to 1, now it should be proportioned to healthy ratio.
  host_set_.setOverprovisioningFactor(100);
  updateHostSet(host_set_, 4, 2);
  failover_host_set_.setOverprovisioningFactor(100);
  updateHostSet(failover_host_set_, 4, 2);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(50, 50));
}

TEST_P(LoadBalancerBaseTest, WeightedPriorityHealth) {
  host_set_.weighted_priority_health_ = true;
  failover_host_set_.weighted_priority_health_ = true;

  // Makes math easier to read.
  host_set_.setOverprovisioningFactor(100);
  failover_host_set_.setOverprovisioningFactor(100);

  // Basic healthy/unhealthy test.
  updateHostSet(host_set_, 4, 2, 0, 0);
  updateHostSet(failover_host_set_, 1, 1);

  // Total weight is 10, healthy weight is 6.
  host_set_.hosts_[0]->weight(3); // Healthy
  host_set_.hosts_[1]->weight(3); // Healthy
  host_set_.hosts_[2]->weight(2); // Unhealthy
  host_set_.hosts_[3]->weight(2); // Unhealthy
  host_set_.runCallbacks({}, {});
  ASSERT_THAT(getLoadPercentage(), ElementsAre(60, 40));
}

TEST_P(LoadBalancerBaseTest, WeightedPriorityHealthExcluded) {
  host_set_.weighted_priority_health_ = true;
  failover_host_set_.weighted_priority_health_ = true;

  // Makes math easier to read.
  host_set_.setOverprovisioningFactor(100);
  failover_host_set_.setOverprovisioningFactor(100);

  updateHostSet(failover_host_set_, 1, 1);
  updateHostSet(host_set_, 3, 1, 0, 1);
  host_set_.hosts_[0]->weight(4);  // Healthy
  host_set_.hosts_[1]->weight(10); // Excluded
  host_set_.hosts_[2]->weight(6);  // Unhealthy
  host_set_.runCallbacks({}, {});
  ASSERT_THAT(getLoadPercentage(), ElementsAre(40, 60));
}

TEST_P(LoadBalancerBaseTest, WeightedPriorityHealthDegraded) {
  host_set_.weighted_priority_health_ = true;
  failover_host_set_.weighted_priority_health_ = true;

  // Makes math easier to read.
  host_set_.setOverprovisioningFactor(100);
  failover_host_set_.setOverprovisioningFactor(100);

  updateHostSet(host_set_, 4, 1, 1, 0);
  host_set_.hosts_[0]->weight(4); // Healthy
  host_set_.hosts_[1]->weight(3); // Degraded
  host_set_.hosts_[2]->weight(2); // Unhealthy
  host_set_.hosts_[3]->weight(1); // Unhealthy
  host_set_.runCallbacks({}, {});

  updateHostSet(failover_host_set_, 2, 1, 1); // 1 healthy host, 1 degraded.
  failover_host_set_.hosts_[0]->weight(1);    // Healthy
  failover_host_set_.hosts_[1]->weight(9);    // Degraded
  failover_host_set_.runCallbacks({}, {});

  // 40% for healthy priority 0, 10% for healthy priority 1, 30% for degraded priority zero, and the
  // remaining 20% to degraded priority 1.
  ASSERT_THAT(getLoadPercentage(), ElementsAre(40, 10));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(30, 20));
}

TEST_P(LoadBalancerBaseTest, GentleFailover) {
  // With 100% of P=0 hosts healthy, P=0 gets all the load.
  // None of the levels is in Panic mode
  updateHostSet(host_set_, 1, 1);
  updateHostSet(failover_host_set_, 1, 1);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(100, 0));
  ASSERT_THAT(getPanic(), ElementsAre(false, false));

  // Health P=0 == 50*1.4 == 70
  // Total health = 70 + 70 >= 100%. None of the levels should be in panic mode.
  updateHostSet(host_set_, 2 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 2 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(70, 30));
  ASSERT_THAT(getPanic(), ElementsAre(false, false));

  // Health P=0 == 25*1.4 == 35   P=1 is healthy so takes all spillover.
  // Total health = 35+100 >= 100%. P=0 is below Panic level but it is ignored, because
  // Total health >= 100%.
  updateHostSet(host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 2 /* num_hosts */, 2 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(35, 65));
  ASSERT_THAT(getPanic(), ElementsAre(false, false));

  // Health P=0 == 25*1.4 == 35   P=1 == 35
  // Health is then scaled up by (100 / (35 + 35) == 50)
  // Total health = 35% + 35% is less than 100%. Panic levels per priority kick in.
  updateHostSet(host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(50, 50));
  ASSERT_THAT(getPanic(), ElementsAre(true, true));

  // Health P=0 == 100*1.4 == 35 P=1 == 35
  // Since 3 hosts are excluded, P=0 should be considered fully healthy.
  // Total health = 100% + 35% is greater than 100%. Panic should not trigger.
  updateHostSet(host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */, 0 /* num_degraded_hosts
                                                                            */
                ,
                3 /* num_excluded_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(100, 0));
  ASSERT_THAT(getPanic(), ElementsAre(false, false));

  // Health P=0 == 100*1.4 == 35 P=1 == 35
  // Total health = 35% is less than 100%.
  // All priorities are in panic mode (situation called TotalPanic)
  // Load is distributed based on number of hosts regardless of their health status.
  // P=0 and P=1 have 4 hosts each so each priority will receive 50% of the traffic.
  updateHostSet(host_set_, 4 /* num_hosts */, 0 /* num_healthy_hosts */, 0 /* num_degraded_hosts
                                                                            */
                ,
                4 /* num_excluded_hosts */);
  updateHostSet(failover_host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(50, 50));
  ASSERT_THAT(getPanic(), ElementsAre(true, true));

  // Make sure that in TotalPanic mode (all levels are in Panic),
  // load distribution depends only on number of hosts.
  // excluded_hosts should not be taken into account.
  // P=0 has 4 hosts with 1 excluded, P=1 has 6 hosts with 2 excluded.
  // P=0 should receive 4/(4+6)=40% of traffic
  // P=1 should receive 6/(4+6)=60% of traffic
  updateHostSet(host_set_, 4 /* num_hosts */, 0 /* num_healthy_hosts */, 0 /* num_degraded_hosts
                                                                            */
                ,
                1 /* num_excluded_hosts */);
  updateHostSet(failover_host_set_, 6 /* num_hosts */, 1 /* num_healthy_hosts */,
                0 /* num_degraded_hosts */, 2 /* num_excluded_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(40, 60));
  ASSERT_THAT(getPanic(), ElementsAre(true, true));
}

TEST_P(LoadBalancerBaseTest, GentleFailoverWithExtraLevels) {
  // Add a third host set. Again with P=0 healthy, all traffic goes there.
  MockHostSet& tertiary_host_set_ = *priority_set_.getMockHostSet(2);
  updateHostSet(host_set_, 1, 1);
  updateHostSet(failover_host_set_, 1, 1);
  updateHostSet(tertiary_host_set_, 1, 1);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(100, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(false, false, false));

  // Health P=0 == 50*1.4 == 70
  // Health P=0 == 50, so can take the 30% spillover.
  updateHostSet(host_set_, 2 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 2 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 2 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(70, 30, 0));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));

  // Health P=0 == 25*1.4 == 35   P=1 is healthy so takes all spillover.
  updateHostSet(host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 2 /* num_hosts */, 2 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 2 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(35, 65, 0));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));

  // This is the first test where health (P=0 + P=1 < 100)
  // Health P=0 == 25*1.4 == 35   P=1 == 35  P=2 == 35
  updateHostSet(host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(35, 35, 30));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));

  // This is the first test where (health P=0 + P=1 < 100)
  // Health P=0 == 25*1.4 == 35   P=1 == 35  P=2 == 35
  updateHostSet(host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 4 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(35, 35, 30));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));

  // Now all health is (20% * 1.5 == 28). 28 * 3 < 100 so we have to scale.
  // Each Priority level gets 33% of the load, with P=0 picking up the rounding error.
  updateHostSet(host_set_, 5 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 1 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(34, 33, 33));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(true, true, true));

  // Levels P=0 and P=1 are totally down. P=2 is totally healthy.
  // 100% of the traffic should go to P=2 and P=0 and P=1 should
  // not be in panic mode.
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 5 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(0, 0, 100));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(false, false, false));

  // Levels P=0 and P=1 are totally down. P=2 is 80*1.4 >= 100% healthy.
  // 100% of the traffic should go to P=2 and P=0 and P=1 should
  // not be in panic mode.
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 4 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(0, 0, 100));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(false, false, false));

  // Levels P=0 and P=1 are totally down. P=2 is 40*1.4=56%% healthy.
  // 100% of the traffic should go to P=2. All levels P=0, P=1 and P=2 should
  // be in panic mode.
  // Since all levels are in panic mode load distribution is based
  // on number of hosts in each level.
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 2 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(34, 33, 33));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(true, true, true));

  // Level P=0 is totally degraded. P=1 is 40*1.4=56% healthy and 40*1.4=56% degraded. P=2 is
  // 40*1.4=56%% healthy. 100% of the traffic should go to P=2. No priorities should be in panic
  // mode.
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */,
                5 /* num_degraded_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 2 /* num_healthy_hosts */,
                2 /* num_degraded_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 2 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(0, 56, 44));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(false, false, false));

  // All levels are completely down - situation called TotalPanic.
  // Load is distributed based on the number
  // of hosts in the priority in relation to the total number of hosts.
  // Here the total number of hosts is 10.
  // priority 0 will receive 5/10: 50% of the traffic
  // priority 1 will receive 3/10: 30% of the traffic
  // priority 2 will receive 2/10: 20% of the traffic
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 2 /* num_hosts */, 0 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(50, 30, 20));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(true, true, true));

  // Rounding errors should be picked up by the first priority.
  // All priorities are in panic mode - situation called TotalPanic.
  // Load is distributed based on the number
  // of hosts in the priority in relation to the total number of hosts.
  // Total number of hosts is 5+6+3=14.
  // priority 0 should receive 5/14=37% of traffic
  // priority 1 should receive 6/14=42% of traffic
  // priority 2 should receive 3/14=21% of traffic
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 6 /* num_hosts */, 2 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 3 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(37, 42, 21));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getPanic(), ElementsAre(true, true, true));

  // Load should spill over into degraded.
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */,
                1 /* num_degraded_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */,
                5 /* num_degraded_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 1 /* num_healthy_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(0, 0, 28));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(28, 44, 0));

  // Rounding errors should be picked up by the first priority with degraded hosts when
  // there are no healthy priorities.
  // Disable panic threshold to prevent total panic from kicking in.
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(0));
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */,
                2 /* num_degraded_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */,
                1 /* num_degraded_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(0, 0, 0));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 67, 33));

  // Simulate Total Panic mode. There is no healthy hosts, but there are
  // degraded hosts. Because there is Total Panic, load is distributed
  // based just on number of hosts in priorities regardless of its health.
  // Rounding errors should be picked up by the first priority.
  // Enable back panic threshold.
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  updateHostSet(host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */,
                2 /* num_degraded_hosts */);
  updateHostSet(tertiary_host_set_, 5 /* num_hosts */, 0 /* num_healthy_hosts */,
                1 /* num_degraded_hosts */);
  ASSERT_THAT(getLoadPercentage(), ElementsAre(34, 33, 33));
  ASSERT_THAT(getDegradedLoadPercentage(), ElementsAre(0, 0, 0));

  // Rounding error should be allocated to the first non-empty priority
  // In this test P=0 is not empty.
  updateHostSet(host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  ASSERT_THAT(getPanic(), ElementsAre(true, true, true));
  ASSERT_THAT(getLoadPercentage(), ElementsAre(34, 33, 33));

  // Rounding error should be allocated to the first non-empty priority
  // In this test P=0 is empty and P=1 is not empty.
  updateHostSet(host_set_, 0 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 6 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  ASSERT_THAT(getPanic(), ElementsAre(true, true, true));
  ASSERT_THAT(getLoadPercentage(), ElementsAre(0, 67, 33));
  // In this test P=1 is not empty.
  updateHostSet(host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(failover_host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  updateHostSet(tertiary_host_set_, 3 /* num_hosts */, 0 /* num_healthy_hosts */);
  ASSERT_THAT(getPanic(), ElementsAre(true, true, true));
  ASSERT_THAT(getLoadPercentage(), ElementsAre(34, 33, 33));
}

TEST_P(LoadBalancerBaseTest, BoundaryConditions) {
  TestRandomGenerator rand;
  uint32_t num_priorities = rand.random() % 10;

  for (uint32_t i = 0; i < num_priorities; ++i) {
    uint32_t num_hosts = rand.random() % 100;
    uint32_t healthy_hosts = std::min<uint32_t>(num_hosts, rand.random() % 100);
    // Make sure random health situations don't trigger the assert in recalculatePerPriorityState
    updateHostSet(*priority_set_.getMockHostSet(i), num_hosts, healthy_hosts);
  }
}

class TestZoneAwareLb : public ZoneAwareLoadBalancerBase {
public:
  TestZoneAwareLb(const PrioritySet& priority_set, ClusterLbStats& lb_stats,
                  Runtime::Loader& runtime, Random::RandomGenerator& random,
                  uint32_t healthy_panic_threshold,
                  absl::optional<LocalityLbConfig> locality_config)
      : ZoneAwareLoadBalancerBase(priority_set, nullptr, lb_stats, runtime, random,
                                  healthy_panic_threshold, locality_config) {}

  HostConstSharedPtr chooseHostOnce(LoadBalancerContext*) override {
    return choose_host_once_host_;
  }
  HostConstSharedPtr peekAnotherHost(LoadBalancerContext*) override { PANIC("not implemented"); }

  HostConstSharedPtr choose_host_once_host_{std::make_shared<NiceMock<MockHost>>()};
};

// Used to test common functions of ZoneAwareLoadBalancerBase.
class ZoneAwareLoadBalancerBaseTest : public LoadBalancerTestBase {
public:
  envoy::config::cluster::v3::Cluster::CommonLbConfig common_config_;
  TestZoneAwareLb lb_{priority_set_, stats_, runtime_, random_, 50, {}};
  TestZoneAwareLoadBalancer lbx_{priority_set_, stats_, runtime_, random_, 50, {}};
};

// Tests the source type static methods in zone aware load balancer.
TEST_F(ZoneAwareLoadBalancerBaseTest, SourceTypeMethods) {
  { EXPECT_ENVOY_BUG(lbx_.runInvalidLocalitySourceType(), "unexpected locality source type enum"); }

  { EXPECT_ENVOY_BUG(lbx_.runInvalidSourceType(), "unexpected source type enum"); }
}

TEST_F(ZoneAwareLoadBalancerBaseTest, BaseMethods) {
  EXPECT_FALSE(lb_.lifetimeCallbacks().has_value());
  std::vector<uint8_t> hash_key;
  auto mock_host = std::make_shared<NiceMock<MockHost>>();
  EXPECT_FALSE(lb_.selectExistingConnection(nullptr, *mock_host, hash_key).has_value());
}

using LocalityLbConfig =
    envoy::extensions::load_balancing_policies::common::v3::LocalityLbConfig;

// Zone-aware load balancer with local priority set support for LRS_REPORTED_RATE testing.
// Unlike TestZoneAwareLb, this LB's chooseHostOnce goes through hostSourceToUse()
// to exercise the full zone-aware routing path including LRS fraction handling.
class TestZoneAwareLbWithLocal : public ZoneAwareLoadBalancerBase {
public:
  TestZoneAwareLbWithLocal(const PrioritySet& priority_set,
                           const PrioritySet* local_priority_set, ClusterLbStats& lb_stats,
                           Runtime::Loader& runtime, Random::RandomGenerator& random,
                           uint32_t healthy_panic_threshold,
                           absl::optional<LocalityLbConfig> locality_config)
      : ZoneAwareLoadBalancerBase(priority_set, local_priority_set, lb_stats, runtime, random,
                                  healthy_panic_threshold, locality_config) {}

  HostConstSharedPtr chooseHostOnce(LoadBalancerContext* context) override {
    const absl::optional<HostsSource> hosts_source = hostSourceToUse(context, random(false));
    if (!hosts_source) {
      return nullptr;
    }
    const auto& hosts = hostSourceToHosts(*hosts_source);
    if (hosts.empty()) {
      return nullptr;
    }
    return hosts[random_.random() % hosts.size()];
  }
  HostConstSharedPtr peekAnotherHost(LoadBalancerContext*) override { PANIC("not implemented"); }

  // Expose protected members for testing.
  void setLrsReportedFraction(const envoy::config::core::v3::Locality& locality,
                              uint64_t fraction) {
    lrs_reported_fractions_[locality] = fraction;
  }
  void clearLrsReportedFractions() { lrs_reported_fractions_.clear(); }
  void setLrsFractionsStale(bool stale) { lrs_fractions_stale_ = stale; }
  bool getLrsFractionsStale() const { return lrs_fractions_stale_; }
  void regenerateRouting() { regenerateLocalityRoutingStructures(); }
};

// Test fixture for LRS_REPORTED_RATE zone-aware routing tests.
class LrsReportedRateTest : public Event::TestUsingSimulatedTime, public testing::Test {
protected:
  LrsReportedRateTest()
      : stat_names_(stats_store_.symbolTable()), stats_(stat_names_, *stats_store_.rootScope()) {
    zone_a_.set_zone("A");
    zone_b_.set_zone("B");
    zone_c_.set_zone("C");
  }

  // Create a locality config with LRS_REPORTED_RATE mode.
  LocalityLbConfig makeLrsLocalityConfig(uint64_t min_cluster_size = 1) {
    LocalityLbConfig config;
    auto* za = config.mutable_zone_aware_lb_config();
    za->mutable_routing_enabled()->set_value(100);
    za->mutable_min_cluster_size()->set_value(min_cluster_size);
    za->set_locality_basis(static_cast<LocalityLbConfig::ZoneAwareLbConfig::LocalityBasis>(2));
    return config;
  }

  // Initialize the LB with given upstream and local host configurations.
  void init(HostVectorSharedPtr upstream_hosts,
            HostsPerLocalitySharedPtr upstream_hosts_per_locality,
            HostVectorSharedPtr local_hosts,
            HostsPerLocalitySharedPtr local_hosts_per_locality,
            uint64_t min_cluster_size = 1) {
    local_priority_set_ = std::make_shared<PrioritySetImpl>();
    local_priority_set_->getOrCreateHostSet(0);

    host_set_.healthy_hosts_ = *upstream_hosts;
    host_set_.hosts_ = *upstream_hosts;
    host_set_.healthy_hosts_per_locality_ = upstream_hosts_per_locality;

    LocalityLbConfig config = makeLrsLocalityConfig(min_cluster_size);
    lb_ = std::make_unique<TestZoneAwareLbWithLocal>(priority_set_, local_priority_set_.get(),
                                                     stats_, runtime_, random_, 50, config);

    // Update local priority set to trigger regenerateLocalityRoutingStructures.
    local_priority_set_->updateHosts(
        0,
        updateHostsParams(local_hosts, local_hosts_per_locality,
                          std::make_shared<const HealthyHostVector>(*local_hosts),
                          local_hosts_per_locality),
        {}, empty_host_vector_, empty_host_vector_, random_.random(), absl::nullopt);
  }

  using ZoneAwareLbConfig = LocalityLbConfig::ZoneAwareLbConfig;

  Stats::IsolatedStoreImpl stats_store_;
  ClusterLbStatNames stat_names_;
  ClusterLbStats stats_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<MockPrioritySet> priority_set_;
  MockHostSet& host_set_ = *priority_set_.getMockHostSet(0);
  std::shared_ptr<MockClusterInfo> info_{new NiceMock<MockClusterInfo>()};
  std::shared_ptr<PrioritySetImpl> local_priority_set_;
  std::unique_ptr<TestZoneAwareLbWithLocal> lb_;
  HostVector empty_host_vector_;

  envoy::config::core::v3::Locality zone_a_;
  envoy::config::core::v3::Locality zone_b_;
  envoy::config::core::v3::Locality zone_c_;
};

// When LRS_REPORTED_RATE is configured but no fractions have been set,
// the LB should fall back to host count behavior (same as HEALTHY_HOSTS_NUM).
TEST_F(LrsReportedRateTest, FallsBackToHostCountWhenNoFractions) {
  // 3 zones: A(3 local, 3 upstream), B(5 local, 5 upstream), C(2 local, 2 upstream)
  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:86", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:87", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:88", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.1:89", zone_c_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:86", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:87", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.1:88", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.1:89", zone_c_)}});

  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:86", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:87", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:88", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.2:89", zone_c_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:86", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:87", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.2:88", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.2:89", zone_c_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  // No fractions set, lrs_fractions_stale_ should be true (default).
  EXPECT_TRUE(lb_->getLrsFractionsStale());

  // With equal host counts (3/5/2 = 30%/50%/20%) in both local and upstream,
  // local_pct == upstream_pct for zone A, so we should get LocalityDirect.
  // All traffic should route directly to zone A (index 0).
  // Random calls: #1 priority hash, #2 host selection within locality.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)).WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_all_directly_.value());
}

// When fractions are set, they should be used for local percentage computation
// while upstream still uses host counts.
TEST_F(LrsReportedRateTest, UsesFractionsForLocalPercentage) {
  // Upstream: zone_a=3, zone_b=5, zone_c=2 (30%/50%/20% by host count)
  // Local with LRS fractions: zone_a=5000 (50%), zone_b=3500 (35%), zone_c=1500 (15%)
  // This simulates BGP skew where zone_a gets more traffic than its host count suggests.

  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:86", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:87", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:88", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.1:89", zone_c_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:86", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:87", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.1:88", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.1:89", zone_c_)}});

  // Local hosts: 3 in zone_a, 5 in zone_b, 2 in zone_c (host counts).
  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:86", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:87", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:88", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.2:89", zone_c_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:86", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:87", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.2:88", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.2:89", zone_c_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  // Set LRS fractions: zone_a=50%, zone_b=35%, zone_c=15%.
  lb_->setLrsReportedFraction(zone_a_, 5000);
  lb_->setLrsReportedFraction(zone_b_, 3500);
  lb_->setLrsReportedFraction(zone_c_, 1500);
  lb_->setLrsFractionsStale(false);

  // Regenerate routing structures directly (bypasses the callback that clears fractions).
  lb_->regenerateRouting();

  // With LRS fractions:
  //   local_percentage: zone_a=50%, zone_b=35%, zone_c=15% (from fractions)
  //   upstream_percentage: zone_a=30%, zone_b=50%, zone_c=20% (from host counts)
  //
  // For zone A: upstream_pct (3000) < local_pct (5000) -> LocalityResidual.
  // local_percent_to_route = upstream_pct * 10000 / local_pct = 3000 * 10000 / 5000 = 6000
  // So 60% of traffic should go local, 40% should spill over.

  // When random() returns a value < 6000 (out of 10000), we route locally (zone A = index 0).
  // Random calls: #1 priority hash, #2 zone routing decision, #3 host selection.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)).WillOnce(Return(5999)).WillOnce(Return(0));
  auto response = lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_sampled_.value());

  // When random() returns a value >= 6000, we route cross-zone.
  // Random calls: #1 priority hash, #2 zone routing, #3 residual pick, #4 host selection.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(6000))
      .WillOnce(Return(0))
      .WillOnce(Return(0));
  response = lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_cross_zone_.value());
}

// When fractions show skew (zone_a gets 50% traffic but only 30% of hosts),
// routing transitions from LocalityDirect to LocalityResidual.
TEST_F(LrsReportedRateTest, SkewedTrafficCausesResidualRouting) {
  // Equal setup: zone_a=3, zone_b=3 (both local and upstream).
  // With equal host counts, this would be LocalityDirect.
  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_)}});

  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  // Without fractions (stale), should be LocalityDirect since local=upstream percentages.
  // Random calls: #1 priority hash, #2 host selection.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)).WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_all_directly_.value());

  // Now set skewed fractions: zone_a=70%, zone_b=30%.
  lb_->setLrsReportedFraction(zone_a_, 7000);
  lb_->setLrsReportedFraction(zone_b_, 3000);
  lb_->setLrsFractionsStale(false);

  // Regenerate routing structures directly with the new fractions.
  lb_->regenerateRouting();

  // Now local_pct: zone_a=70%, zone_b=30%. upstream_pct: zone_a=50%, zone_b=50%.
  // zone_a: upstream_pct (5000) < local_pct (7000) -> LocalityResidual.
  // local_percent_to_route = 5000 * 10000 / 7000 = 7142

  // Route locally when random < 7142.
  // Random calls: #1 priority hash, #2 zone routing, #3 host selection.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)).WillOnce(Return(7141)).WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_sampled_.value());

  // Route cross-zone when random >= 7142.
  // Random calls: #1 priority hash, #2 zone routing, #3 residual pick, #4 host selection.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(7142))
      .WillOnce(Return(0))
      .WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_cross_zone_.value());
}

// When fractions become stale, the LB should fall back to host count behavior.
TEST_F(LrsReportedRateTest, StaleFractionsFallBack) {
  // 2 zones: A and B, equal hosts.
  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_)}});

  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:82", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  // Set fractions with skew: zone_a=80%, zone_b=20%.
  lb_->setLrsReportedFraction(zone_a_, 8000);
  lb_->setLrsReportedFraction(zone_b_, 2000);
  lb_->setLrsFractionsStale(false);

  lb_->regenerateRouting();

  // With fractions, should be LocalityResidual (upstream 50% < local 80% for zone A).
  // local_percent_to_route = 5000 * 10000 / 8000 = 6250
  // Verify cross-zone routing is triggered.
  // Random calls: #1 priority hash, #2 zone routing, #3 residual pick, #4 host selection.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(9999))
      .WillOnce(Return(0))
      .WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_cross_zone_.value());

  // Now mark fractions as stale.
  lb_->setLrsFractionsStale(true);

  lb_->regenerateRouting();

  // With stale fractions, falls back to host count (equal: 2/2 = 50%/50%).
  // local_pct == upstream_pct -> LocalityDirect. All traffic goes to zone A.
  // Random calls: #1 priority hash, #2 host selection.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)).WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_GE(stats_.lb_zone_routing_all_directly_.value(), 1U);
}

// Upstream always uses host counts regardless of LRS_REPORTED_RATE mode.
TEST_F(LrsReportedRateTest, UpstreamUsesHostCountsRegardless) {
  // zone_a: 2 upstream, 2 local. zone_b: 2 upstream, 2 local.
  // LRS fractions: zone_a=80%, zone_b=20%.
  // Upstream should still compute 50%/50% from host counts.
  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_)}});

  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:82", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  // Set skewed fractions for local side.
  lb_->setLrsReportedFraction(zone_a_, 8000);
  lb_->setLrsReportedFraction(zone_b_, 2000);
  lb_->setLrsFractionsStale(false);

  lb_->regenerateRouting();

  // local_pct: zone_a=80%, zone_b=20%. upstream_pct: zone_a=50%, zone_b=50% (host counts).
  // zone_a: upstream (50%) < local (80%) -> LocalityResidual.
  // local_percent_to_route = 5000 * 10000 / 8000 = 6250.
  // This proves upstream uses host counts (50%) not fractions (80%).
  // Random calls: #1 priority hash, #2 zone routing, #3 host selection.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)).WillOnce(Return(6249)).WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_sampled_.value());

  // Random calls: #1 priority hash, #2 zone routing, #3 residual pick, #4 host selection.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(6250))
      .WillOnce(Return(0))
      .WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_cross_zone_.value());
}

// When fractions are set for only some localities, localities without fractions
// should fall back to host count.
TEST_F(LrsReportedRateTest, PartialFractionsUseMixedSources) {
  // 3 zones. Fractions set for zone_a and zone_b, but not zone_c.
  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:84", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.1:85", zone_c_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.1:84", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.1:85", zone_c_)}});

  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:84", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.2:85", zone_c_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:82", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.2:84", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.2:85", zone_c_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  // Set fractions for zone_a and zone_b only. zone_c gets host count (2).
  // zone_a = 6000 (60%), zone_b = 1000 (10%). zone_c = host count 2.
  // Total local weight = 6000 + 1000 + 2 = 7002.
  // local_pct: zone_a = 6000/7002 * 10000 ≈ 8568, zone_b ≈ 1428, zone_c ≈ 2.
  lb_->setLrsReportedFraction(zone_a_, 6000);
  lb_->setLrsReportedFraction(zone_b_, 1000);
  lb_->setLrsFractionsStale(false);

  lb_->regenerateRouting();

  // zone_a: upstream_pct ≈ 33% (3333), local_pct ≈ 85.68% (8568).
  // upstream < local -> LocalityResidual. This means fractions were used for zone_a.
  // Cross-zone routing should occur for some random values.
  // Random calls: #1 priority hash, #2 zone routing, #3 residual pick, #4 host selection.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(9999))
      .WillOnce(Return(0))
      .WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_cross_zone_.value());
}

// Fractions update on EDS update via metadata.
TEST_F(LrsReportedRateTest, FractionsUpdateOnEds) {
  // 2 zones, equal hosts.
  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:82", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_)}});

  // Create local hosts with metadata containing observed_traffic_fraction.
  envoy::config::core::v3::Metadata metadata_a;
  auto* lb_fields_a =
      (*metadata_a.mutable_filter_metadata())["envoy.lb"].mutable_fields();
  (*lb_fields_a)["observed_traffic_fraction"].set_number_value(7000); // 70%

  envoy::config::core::v3::Metadata metadata_b;
  auto* lb_fields_b =
      (*metadata_b.mutable_filter_metadata())["envoy.lb"].mutable_fields();
  (*lb_fields_b)["observed_traffic_fraction"].set_number_value(3000); // 30%

  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", metadata_a, zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", metadata_a, zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", metadata_b, zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", metadata_b, zone_b_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", metadata_a, zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", metadata_a, zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:82", metadata_b, zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:83", metadata_b, zone_b_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  // The metadata-based fractions should be extracted automatically via the local priority
  // set callback which calls updateLrsReportedFractions().
  // zone_a: local_pct = 70%, upstream_pct = 50%. upstream < local -> LocalityResidual.
  EXPECT_FALSE(lb_->getLrsFractionsStale());

  // Verify cross-zone routing occurs (proves fractions were extracted from metadata).
  // Random calls: #1 priority hash, #2 zone routing, #3 residual pick, #4 host selection.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(9999))
      .WillOnce(Return(0))
      .WillOnce(Return(0));
  lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_cross_zone_.value());
}

// Spillover distributes correctly to zones with residual capacity.
TEST_F(LrsReportedRateTest, SpilloverDistribution) {
  // 3 zones with different capacities.
  // Upstream: zone_a=3 (30%), zone_b=5 (50%), zone_c=2 (20%).
  // LRS fractions: zone_a=50%, zone_b=35%, zone_c=15%.
  HostVectorSharedPtr upstream_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:86", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:87", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.1:88", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.1:89", zone_c_)}));
  HostsPerLocalitySharedPtr upstream_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.1:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.1:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.1:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:85", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:86", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.1:87", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.1:88", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.1:89", zone_c_)}});

  HostVectorSharedPtr local_hosts(
      new HostVector({makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_),
                      makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:86", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:87", zone_b_),
                      makeTestHost(info_, "tcp://127.0.0.2:88", zone_c_),
                      makeTestHost(info_, "tcp://127.0.0.2:89", zone_c_)}));
  HostsPerLocalitySharedPtr local_hosts_per_locality = makeHostsPerLocality(
      {{makeTestHost(info_, "tcp://127.0.0.2:80", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:81", zone_a_),
        makeTestHost(info_, "tcp://127.0.0.2:82", zone_a_)},
       {makeTestHost(info_, "tcp://127.0.0.2:83", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:84", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:85", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:86", zone_b_),
        makeTestHost(info_, "tcp://127.0.0.2:87", zone_b_)},
       {makeTestHost(info_, "tcp://127.0.0.2:88", zone_c_),
        makeTestHost(info_, "tcp://127.0.0.2:89", zone_c_)}});

  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.healthy_panic_threshold", 50))
      .WillRepeatedly(Return(50));
  EXPECT_CALL(runtime_.snapshot_, featureEnabled("upstream.zone_routing.enabled", 100))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.min_cluster_size", 1))
      .WillRepeatedly(Return(1));
  EXPECT_CALL(runtime_.snapshot_, getInteger("upstream.zone_routing.force_local_zone.min_size", 0))
      .WillRepeatedly(Return(0));

  init(upstream_hosts, upstream_hosts_per_locality, local_hosts, local_hosts_per_locality);

  lb_->setLrsReportedFraction(zone_a_, 5000);
  lb_->setLrsReportedFraction(zone_b_, 3500);
  lb_->setLrsReportedFraction(zone_c_, 1500);
  lb_->setLrsFractionsStale(false);

  lb_->regenerateRouting();

  // local_pct: zone_a=50%, zone_b=35%, zone_c=15%.
  // upstream_pct: zone_a=30%, zone_b=50%, zone_c=20%.
  // zone_a: upstream(30%) < local(50%) -> Residual.
  // local_percent_to_route = 3000 * 10000 / 5000 = 6000.
  //
  // Residual capacity:
  //   zone_a (local, index 0): no residual (already routed what we can)
  //   zone_b (index 1): upstream(5000) - local(3500) = 1500
  //   zone_c (index 2): upstream(2000) - local(1500) = 500
  // Total residual: 2000. zone_b gets 1500/2000=75%, zone_c gets 500/2000=25%.

  // Cross-zone traffic with threshold 0 -> should land in zone_b (index 1, residual [0,1500))
  // Random calls: #1 priority hash, #2 zone routing, #3 residual pick, #4 host selection.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(6001))
      .WillOnce(Return(0))
      .WillOnce(Return(0));
  auto host = lb_->chooseHost(nullptr);
  EXPECT_EQ(1U, stats_.lb_zone_routing_cross_zone_.value());

  // Cross-zone traffic with threshold 1499 -> still zone_b
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(6001))
      .WillOnce(Return(1499))
      .WillOnce(Return(0));
  host = lb_->chooseHost(nullptr);
  EXPECT_EQ(2U, stats_.lb_zone_routing_cross_zone_.value());

  // Cross-zone traffic with threshold 1500 -> zone_c (index 2, residual [1500,2000))
  EXPECT_CALL(random_, random())
      .WillOnce(Return(0))
      .WillOnce(Return(6001))
      .WillOnce(Return(1500))
      .WillOnce(Return(0));
  host = lb_->chooseHost(nullptr);
  EXPECT_EQ(3U, stats_.lb_zone_routing_cross_zone_.value());
}

} // namespace
} // namespace Upstream
} // namespace Envoy
