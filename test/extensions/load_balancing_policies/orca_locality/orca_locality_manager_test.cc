#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"
#include "source/extensions/load_balancing_policies/orca_locality/orca_locality_manager.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/host_set.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/simulated_time_system.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace OrcaLocality {
namespace {

using ::testing::NiceMock;
using ::testing::Return;
using ::testing::ReturnRef;

// Helper to create a MockHost that tracks weight and lb policy data state.
std::shared_ptr<NiceMock<Upstream::MockHost>>
makeWeightTrackingMockHost(uint32_t initial_weight = 1) {
  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  auto weight = std::make_shared<uint32_t>(initial_weight);
  ON_CALL(*host, weight()).WillByDefault([weight]() -> uint32_t { return *weight; });
  ON_CALL(*host, weight(::testing::_)).WillByDefault([weight](uint32_t new_weight) {
    *weight = new_weight;
  });
  auto* raw_host = host.get();
  ON_CALL(*host, setLbPolicyData(::testing::_))
      .WillByDefault(::testing::Invoke([raw_host](Upstream::HostLbPolicyDataPtr data) {
        raw_host->lb_policy_data_ = std::move(data);
      }));
  return host;
}

// Set ORCA utilization data on a host for locality manager tests.
void setHostOrcaUtilization(Upstream::MockHost& host, double utilization,
                            MonotonicTime non_empty_since, MonotonicTime last_update_time) {
  auto data = std::make_unique<Common::OrcaHostLbPolicyData>(nullptr, /*weight=*/100,
                                                             non_empty_since, last_update_time);
  data->updateUtilization(utilization);
  host.lb_policy_data_ = std::move(data);
}

class OrcaLocalityManagerTest : public testing::Test {
protected:
  void SetUp() override {
    config_.metric_names_for_computing_utilization = {"named_metrics.foo"};
    config_.update_period = std::chrono::milliseconds(1000);
    config_.ema_alpha = 0.3;
    config_.probe_traffic_percent = 5;
    config_.utilization_variance_threshold = 0.05;
    config_.minimum_local_percent = 50;
    config_.blackout_period = std::chrono::milliseconds(10000);
    config_.weight_expiration_period = std::chrono::milliseconds(180000);
  }

  // Create a locality manager with the test config.
  // Must be called after setting up host sets on the priority set.
  std::unique_ptr<OrcaLocalityManager> createManager() {
    auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
    timer_ = timer;
    return std::make_unique<OrcaLocalityManager>(config_, priority_set_, time_system_, dispatcher_,
                                                 [this](const LocalityRoutingSplit& split) {
                                                   last_split_ = split;
                                                   split_updated_count_++;
                                                 });
  }

  // Set up the mock host set with hosts grouped by locality.
  // locality_hosts[0] is the local locality when has_local_locality is true.
  void setupHostsPerLocality(std::vector<Upstream::HostVector> locality_hosts,
                             bool has_local_locality = true) {
    auto* host_set = priority_set_.getMockHostSet(0);

    auto hosts_per_locality = std::make_shared<Upstream::HostsPerLocalityImpl>(
        std::move(locality_hosts), has_local_locality);
    host_set->healthy_hosts_per_locality_ = hosts_per_locality;
    ON_CALL(*host_set, healthyHostsPerLocality())
        .WillByDefault(ReturnRef(*host_set->healthy_hosts_per_locality_));
  }

  // Set utilization on all hosts in a locality.
  void setLocalityUtilization(Upstream::HostVector& hosts, double utilization) {
    MonotonicTime non_empty = MonotonicTime(std::chrono::seconds(1));
    MonotonicTime last_update = MonotonicTime(std::chrono::seconds(50));
    for (auto& host : hosts) {
      auto* mock_host = dynamic_cast<Upstream::MockHost*>(host.get());
      ASSERT(mock_host != nullptr);
      setHostOrcaUtilization(*mock_host, utilization, non_empty, last_update);
    }
  }

  OrcaLocalityManagerConfig config_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  Event::SimulatedTimeSystem time_system_;
  Event::MockTimer* timer_ = nullptr;

  LocalityRoutingSplit last_split_;
  int split_updated_count_ = 0;
};

// Test 1: Balanced utilization - all localities within variance threshold -> local-only (minus
// probe).
TEST_F(OrcaLocalityManagerTest, BalancedUtilization_LocalMinusProbe) {
  // Two localities, each with 2 hosts, all at 0.5 utilization.
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};

  setLocalityUtilization(local_hosts, 0.5);
  setLocalityUtilization(remote_hosts, 0.5);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  EXPECT_TRUE(last_split_.active);
  // Balanced: local gets 95% (100 - probe_traffic_percent=5).
  EXPECT_EQ(last_split_.local_percent_to_route, 9500); // 95 * 100.
  EXPECT_EQ(last_split_.num_localities, 2);
}

// Test 2: Local overloaded - local util > global avg + threshold -> correct spill.
TEST_F(OrcaLocalityManagerTest, LocalOverloaded_SpillsToRemote) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};

  // Local at 0.8, remote at 0.3. Global avg = (0.8*2 + 0.3*2)/4 = 0.55.
  // Local deviation = 0.8 - 0.55 = 0.25. > threshold (0.05) -> spill.
  setLocalityUtilization(local_hosts, 0.8);
  setLocalityUtilization(remote_hosts, 0.3);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  EXPECT_TRUE(last_split_.active);
  // Local should be reduced, remote gets spill.
  // spill = deviation * 100 = 25, but at least probe_traffic_percent (5).
  // local_percent = 100 - 25 = 75. Clamped by minimum_local_percent (50) -> 75 still valid.
  // Also clamped by (100 - probe_traffic_percent) = 95. So local = 75.
  EXPECT_EQ(last_split_.local_percent_to_route, 7500);
}

// Test 3: Local underloaded - local util < global avg -> local-only (minus probe).
TEST_F(OrcaLocalityManagerTest, LocalUnderloaded_LocalMinusProbe) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};

  // Local at 0.3, remote at 0.8.
  setLocalityUtilization(local_hosts, 0.3);
  setLocalityUtilization(remote_hosts, 0.8);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  EXPECT_TRUE(last_split_.active);
  // Underloaded: local gets 95% (100 - probe).
  EXPECT_EQ(last_split_.local_percent_to_route, 9500);
}

// Test 4: Exponential smoothing - step change dampened over multiple cycles.
TEST_F(OrcaLocalityManagerTest, ExponentialSmoothingDampensStepChange) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};

  // Start with balanced utilization.
  setLocalityUtilization(local_hosts, 0.5);
  setLocalityUtilization(remote_hosts, 0.5);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  // First update: smoothed value initialized to raw values (0.5, 0.5).
  manager->updateLocalitySplit();
  EXPECT_TRUE(last_split_.active);
  EXPECT_EQ(last_split_.local_percent_to_route, 9500); // Balanced.

  // Step change: local now at 0.9.
  setLocalityUtilization(local_hosts, 0.9);

  // Second update: smoothed = 0.3 * 0.9 + 0.7 * 0.5 = 0.62.
  // Global avg for 1 host each: (0.62*1 + 0.5*1) / 2 = 0.56.
  // Local deviation = 0.62 - 0.56 = 0.06 > threshold (0.05) -> slight spill.
  manager->updateLocalitySplit();
  auto states = manager->localityStates();
  EXPECT_NEAR(states[0].ema_utilization, 0.62, 0.01);
  EXPECT_NEAR(states[1].ema_utilization, 0.5, 0.01);

  // Third update: smoothed = 0.3 * 0.9 + 0.7 * 0.62 = 0.704.
  manager->updateLocalitySplit();
  states = manager->localityStates();
  EXPECT_NEAR(states[0].ema_utilization, 0.704, 0.01);
}

// Test 5: Minimum local enforced - even with extreme overload.
TEST_F(OrcaLocalityManagerTest, MinimumLocalPercentEnforced) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};

  // Extreme overload: local at 1.0, remote at 0.1.
  setLocalityUtilization(local_hosts, 1.0);
  setLocalityUtilization(remote_hosts, 0.1);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  EXPECT_TRUE(last_split_.active);
  // Even with extreme overload, local should not go below minimum_local_percent (50).
  EXPECT_GE(last_split_.local_percent_to_route, 5000);
}

// Test 6: Probe traffic enforced - even in balanced state, remote gets probe_traffic_percent.
TEST_F(OrcaLocalityManagerTest, ProbeTrafficAlwaysPresent) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};

  setLocalityUtilization(local_hosts, 0.5);
  setLocalityUtilization(remote_hosts, 0.5);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  EXPECT_TRUE(last_split_.active);
  // Probe traffic percent = 5, so local can be at most 95%.
  EXPECT_LE(last_split_.local_percent_to_route, 9500);
  // Residual capacity should be > 0 (remote gets at least probe traffic).
  EXPECT_GT(last_split_.residual_capacity.back(), 0u);
}

// Test 7: Stale data handling - hosts with expired ORCA data are excluded.
TEST_F(OrcaLocalityManagerTest, StaleDataExcludedFromAggregation) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};

  // One local host with valid data, one with expired data.
  auto* host0 = dynamic_cast<Upstream::MockHost*>(local_hosts[0].get());
  setHostOrcaUtilization(*host0, 0.5,
                         /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
                         /*last_update_time=*/MonotonicTime(std::chrono::seconds(50)));

  // Expired: last_update_time is very old.
  auto* host1 = dynamic_cast<Upstream::MockHost*>(local_hosts[1].get());
  setHostOrcaUtilization(*host1, 0.9,
                         /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
                         /*last_update_time=*/MonotonicTime(std::chrono::seconds(1)));

  setLocalityUtilization(remote_hosts, 0.5);
  setupHostsPerLocality({local_hosts, remote_hosts});

  // Set time so that host1's data is expired (beyond weight_expiration_period of 180s).
  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(200)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  // Only host0 (0.5) should contribute to local utilization, host1 excluded.
  auto states = manager->localityStates();
  EXPECT_EQ(states[0].valid_host_count, 1);
  EXPECT_NEAR(states[0].ema_utilization, 0.5, 0.01);
}

// Test 8: Single locality - no remote zones exist -> 100% local, no spill.
TEST_F(OrcaLocalityManagerTest, SingleLocality_NoSplit) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};

  setLocalityUtilization(local_hosts, 0.5);
  setupHostsPerLocality({local_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  // With only 1 locality, split should not be active.
  EXPECT_FALSE(last_split_.active);
  EXPECT_EQ(split_updated_count_, 0);
}

// Test 9: All remote zones at capacity - headroom is 0 -> even distribution, no divide by zero.
TEST_F(OrcaLocalityManagerTest, AllRemoteAtCapacity_EvenDistribution) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote1_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote2_hosts = {makeWeightTrackingMockHost()};

  // Local overloaded, remotes at max capacity (1.0).
  setLocalityUtilization(local_hosts, 0.9);
  setLocalityUtilization(remote1_hosts, 1.0);
  setLocalityUtilization(remote2_hosts, 1.0);

  setupHostsPerLocality({local_hosts, remote1_hosts, remote2_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  EXPECT_TRUE(last_split_.active);
  // Should not crash (no division by zero).
  // Residual capacity should distribute evenly between remote1 and remote2.
  ASSERT_EQ(last_split_.residual_capacity.size(), 3);
  EXPECT_EQ(last_split_.residual_capacity[0], 0); // Local.
  // Even distribution: each remote gets 10000/2 = 5000.
  EXPECT_EQ(last_split_.residual_capacity[1], 5000);
  EXPECT_EQ(last_split_.residual_capacity[2], 10000);
}

// Test 10: Host count weighting - localities with more hosts have proportionally more influence.
TEST_F(OrcaLocalityManagerTest, HostCountWeightingOnGlobalAvg) {
  // Local has 3 hosts at 0.6, remote has 1 host at 0.2.
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost(),
                                      makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};

  setLocalityUtilization(local_hosts, 0.6);
  setLocalityUtilization(remote_hosts, 0.2);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  // Global avg = (0.6*3 + 0.2*1) / 4 = 2.0/4 = 0.5.
  // Local deviation = 0.6 - 0.5 = 0.1 > threshold (0.05) -> overloaded.
  EXPECT_TRUE(last_split_.active);
  // Spill = deviation * 100 = 10%. local_percent = 90.
  EXPECT_EQ(last_split_.local_percent_to_route, 9000);
}

// Test: No ORCA data at all - split should not change.
TEST_F(OrcaLocalityManagerTest, NoOrcaData_SplitUnchanged) {
  // Hosts without any ORCA data.
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  // No ORCA data means no split computation -> callback not invoked.
  EXPECT_EQ(split_updated_count_, 0);
}

// Test: Multiple remote localities with headroom-proportional distribution.
TEST_F(OrcaLocalityManagerTest, HeadroomProportionalDistribution) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote1_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote2_hosts = {makeWeightTrackingMockHost()};

  // Local overloaded at 0.8, remote1 at 0.3 (headroom 0.7), remote2 at 0.6 (headroom 0.4).
  setLocalityUtilization(local_hosts, 0.8);
  setLocalityUtilization(remote1_hosts, 0.3);
  setLocalityUtilization(remote2_hosts, 0.6);

  setupHostsPerLocality({local_hosts, remote1_hosts, remote2_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  manager->updateLocalitySplit();

  EXPECT_TRUE(last_split_.active);
  ASSERT_EQ(last_split_.residual_capacity.size(), 3);
  EXPECT_EQ(last_split_.residual_capacity[0], 0); // Local.

  // Total headroom = 0.7 + 0.4 = 1.1.
  // remote1 share = 0.7/1.1 * 10000 = 6363.
  // remote2 share = 0.4/1.1 * 10000 = 3636.
  // Cumulative: remote1 = 6363, remote2 = 6363 + 3636 = 9999~10000.
  EXPECT_GT(last_split_.residual_capacity[1], last_split_.residual_capacity[0]);
  // Remote1 gets more than remote2 (more headroom).
  uint64_t remote1_share = last_split_.residual_capacity[1];
  uint64_t remote2_share = last_split_.residual_capacity[2] - last_split_.residual_capacity[1];
  EXPECT_GT(remote1_share, remote2_share);
}

// Test: Transition from active to inactive when localities drop below 2.
TEST_F(OrcaLocalityManagerTest, TransitionToInactive) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};

  setLocalityUtilization(local_hosts, 0.5);
  setLocalityUtilization(remote_hosts, 0.5);

  setupHostsPerLocality({local_hosts, remote_hosts});

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));
  auto manager = createManager();

  // First: active split.
  manager->updateLocalitySplit();
  EXPECT_TRUE(last_split_.active);

  // Now reduce to single locality.
  setupHostsPerLocality({local_hosts});

  manager->updateLocalitySplit();
  EXPECT_FALSE(last_split_.active);
  EXPECT_EQ(last_split_.local_percent_to_route, 10000);
}

// Calling updateLocalitySplit() twice with identical data should only fire the callback once.
TEST_F(OrcaLocalityManagerTest, SplitUnchanged_CallbackNotRepeated) {
  Upstream::HostVector local_hosts = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote_hosts = {makeWeightTrackingMockHost()};
  setLocalityUtilization(local_hosts, 0.5);
  setLocalityUtilization(remote_hosts, 0.5);
  setupHostsPerLocality({local_hosts, remote_hosts});

  auto manager = createManager();
  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));

  manager->updateLocalitySplit();
  EXPECT_EQ(split_updated_count_, 1);

  // Second call with identical data - split should not change, callback should not fire again.
  manager->updateLocalitySplit();
  EXPECT_EQ(split_updated_count_, 1);
}

// has_local_locality=false should keep split inactive even with 2+ localities.
TEST_F(OrcaLocalityManagerTest, NoLocalLocality_SplitInactive) {
  Upstream::HostVector hosts_a = {makeWeightTrackingMockHost()};
  Upstream::HostVector hosts_b = {makeWeightTrackingMockHost()};
  setLocalityUtilization(hosts_a, 0.5);
  setLocalityUtilization(hosts_b, 0.5);
  setupHostsPerLocality({hosts_a, hosts_b}, /*has_local_locality=*/false);

  auto manager = createManager();
  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));

  manager->updateLocalitySplit();
  EXPECT_EQ(split_updated_count_, 0);
  EXPECT_FALSE(manager->currentSplit().active);
}

} // namespace
} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
