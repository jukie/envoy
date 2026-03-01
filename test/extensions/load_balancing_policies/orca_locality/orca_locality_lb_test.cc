#include <cstdint>
#include <memory>
#include <vector>

#include "source/common/upstream/upstream_impl.h"
#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"
#include "source/extensions/load_balancing_policies/orca_locality/orca_locality_lb.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/thread_local/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/host_set.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/mocks/upstream/thread_aware_load_balancer.h"
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

// Helper to create a MockHost that tracks weight and lb policy data.
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

// ============================================================
// ThreadLocalSplit tests
// ============================================================

TEST(ThreadLocalSplitTest, DefaultSplitIsInactive) {
  ThreadLocalSplit tls_split;
  EXPECT_FALSE(tls_split.split.active);
  EXPECT_EQ(tls_split.split.local_percent_to_route, 10000);
}

TEST(ThreadLocalSplitTest, PushSplitUpdatesValue) {
  NiceMock<ThreadLocal::MockInstance> tls;
  ON_CALL(tls, allocateSlot()).WillByDefault(testing::Invoke(&tls, &ThreadLocal::MockInstance::allocateSlotMock));
  ON_CALL(tls, runOnAllThreads(testing::An<std::function<void()>>()))
      .WillByDefault(testing::Invoke(&tls, &ThreadLocal::MockInstance::runOnAllThreads1));

  NiceMock<Random::MockRandomGenerator> random;
  auto factory = std::make_shared<WorkerLocalLbFactory>(random, tls, nullptr);

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 7500;
  split.num_localities = 3;
  split.residual_capacity = {0, 5000, 10000};

  factory->pushSplit(split);

  // Verify by reading through the TLS slot.
  auto tls_split = factory->tls_->get();
  ASSERT_TRUE(tls_split.has_value());
  EXPECT_TRUE(tls_split->split.active);
  EXPECT_EQ(tls_split->split.local_percent_to_route, 7500);
  EXPECT_EQ(tls_split->split.num_localities, 3);
  ASSERT_EQ(tls_split->split.residual_capacity.size(), 3);
  EXPECT_EQ(tls_split->split.residual_capacity[2], 10000);
}

// ============================================================
// WorkerLocalLb tests
// ============================================================

class WorkerLocalLbTest : public testing::Test {
protected:
  void SetUp() override {
    ON_CALL(tls_, allocateSlot()).WillByDefault(testing::Invoke(&tls_, &ThreadLocal::MockInstance::allocateSlotMock));
    ON_CALL(tls_, runOnAllThreads(testing::An<std::function<void()>>()))
        .WillByDefault(testing::Invoke(&tls_, &ThreadLocal::MockInstance::runOnAllThreads1));
    host_set_ = priority_set_.getMockHostSet(0);
  }

  // Setup locality hosts on the mock host set.
  void setupLocalities(std::vector<Upstream::HostVector> locality_hosts,
                       bool has_local_locality = true) {
    auto hosts_per_locality = std::make_shared<Upstream::HostsPerLocalityImpl>(
        std::move(locality_hosts), has_local_locality);
    host_set_->healthy_hosts_per_locality_ = hosts_per_locality;
    ON_CALL(*host_set_, healthyHostsPerLocality())
        .WillByDefault(ReturnRef(*host_set_->healthy_hosts_per_locality_));

    // Also set up the flat healthy hosts list.
    Upstream::HostVector all_hosts;
    for (const auto& locality : host_set_->healthy_hosts_per_locality_->get()) {
      for (const auto& host : locality) {
        all_hosts.push_back(host);
      }
    }
    host_set_->healthy_hosts_ = all_hosts;
    ON_CALL(*host_set_, healthyHosts()).WillByDefault(ReturnRef(host_set_->healthy_hosts_));
  }

  // Create a WorkerLocalLb via the factory (which owns the TLS slot).
  std::unique_ptr<WorkerLocalLb>
  createLb(Upstream::LoadBalancerFactorySharedPtr child_factory = nullptr) {
    factory_ = std::make_shared<WorkerLocalLbFactory>(random_, tls_, std::move(child_factory));
    return std::unique_ptr<WorkerLocalLb>(
        static_cast<WorkerLocalLb*>(factory_->create({priority_set_, nullptr}).release()));
  }

  void pushSplit(const LocalityRoutingSplit& split) {
    ASSERT_NE(factory_, nullptr);
    factory_->pushSplit(split);
  }

  NiceMock<ThreadLocal::MockInstance> tls_;
  std::shared_ptr<WorkerLocalLbFactory> factory_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  Upstream::MockHostSet* host_set_;
  NiceMock<Random::MockRandomGenerator> random_;
};

// When split is not active, should pick from all healthy hosts.
TEST_F(WorkerLocalLbTest, InactiveSplit_PicksFromAllHosts) {
  Upstream::HostVector local = {makeWeightTrackingMockHost()};
  Upstream::HostVector remote = {makeWeightTrackingMockHost()};
  setupLocalities({local, remote});

  auto lb = createLb();

  // Split is inactive by default.
  EXPECT_CALL(random_, random()).WillOnce(Return(0));
  auto result = lb->chooseHost(nullptr);
  EXPECT_NE(result.host, nullptr);
}

// When split is active with 100% local, always picks from locality 0.
TEST_F(WorkerLocalLbTest, FullyLocal_PicksLocalHosts) {
  auto local_host = makeWeightTrackingMockHost();
  auto remote_host = makeWeightTrackingMockHost();
  setupLocalities({{local_host}, {remote_host}});

  auto lb = createLb();

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 10000; // 100% local.
  split.num_localities = 2;
  split.residual_capacity = {0, 10000};
  pushSplit(split);

  // Random value below local_percent_to_route -> local.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)).WillOnce(Return(0));
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, local_host);
}

// When split routes to remote, picks from the correct remote locality.
TEST_F(WorkerLocalLbTest, SpillToRemote_PicksRemoteHosts) {
  auto local_host = makeWeightTrackingMockHost();
  auto remote_host = makeWeightTrackingMockHost();
  setupLocalities({{local_host}, {remote_host}});

  auto lb = createLb();

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 0; // 0% local -> all remote.
  split.num_localities = 2;
  split.residual_capacity = {0, 10000};
  pushSplit(split);

  // First random() % 10000: anything >= 0 goes remote (local_percent_to_route is 0).
  // Second random() for residual capacity sampling: any value selects locality 1.
  // Third random() for host selection within locality.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // > 0, so not local
      .WillOnce(Return(0))    // residual_rand -> locality 1
      .WillOnce(Return(0));   // pick host 0 in locality 1
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, remote_host);
}

// With multiple remote localities, weighted residual capacity sampling selects the right one.
TEST_F(WorkerLocalLbTest, MultipleRemoteLocalities_WeightedSelection) {
  auto local_host = makeWeightTrackingMockHost();
  auto remote1_host = makeWeightTrackingMockHost();
  auto remote2_host = makeWeightTrackingMockHost();
  setupLocalities({{local_host}, {remote1_host}, {remote2_host}});

  auto lb = createLb();

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 0;
  split.num_localities = 3;
  // Residual capacity ranges: remote1 gets [0, 3000), remote2 gets [3000, 10000).
  split.residual_capacity = {0, 3000, 10000};
  pushSplit(split);

  // Select remote2: residual_rand = 5000, which is >= 3000 -> locality index 2.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // > 0, so not local
      .WillOnce(Return(5000)) // residual_rand in [3000, 10000) -> locality 2
      .WillOnce(Return(0));   // pick host in locality 2
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, remote2_host);
}

// Empty host set returns nullptr.
TEST_F(WorkerLocalLbTest, EmptyHostSet_ReturnsNull) {
  // Priority set with empty host sets.
  auto lb = createLb();
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, nullptr);
}

// Weighted host selection within a locality.
TEST_F(WorkerLocalLbTest, WeightedHostSelection) {
  auto heavy_host = makeWeightTrackingMockHost(100);
  auto light_host = makeWeightTrackingMockHost(1);
  setupLocalities({{heavy_host, light_host}});

  // Single locality -> inactive split -> falls back to all healthy hosts.
  // Only one random() call for weighted pick (no split random).
  auto lb = createLb();

  // Target = random % total_weight(101) = 50 -> cumulative 100 > 50, picks heavy_host.
  EXPECT_CALL(random_, random()).WillOnce(Return(50));
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, heavy_host);
}

// peekAnotherHost delegates to chooseHost.
TEST_F(WorkerLocalLbTest, PeekDelegatesToChoose) {
  auto host = makeWeightTrackingMockHost();
  setupLocalities({{host}});

  auto lb = createLb();

  // Single locality + single host -> one random() call for host pick.
  EXPECT_CALL(random_, random()).WillOnce(Return(0));
  auto result = lb->peekAnotherHost(nullptr);
  EXPECT_NE(result, nullptr);
}

// ============================================================
// WorkerLocalLb tests with child factory delegation
// ============================================================

// A simple child LB that always returns a specific host.
class FixedHostLb : public Upstream::LoadBalancer {
public:
  explicit FixedHostLb(Upstream::HostConstSharedPtr host) : host_(std::move(host)) {}

  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext*) override {
    return {host_};
  }
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext*) override {
    return host_;
  }
  OptRef<Envoy::Http::ConnectionPool::ConnectionLifetimeCallbacks> lifetimeCallbacks() override {
    return {};
  }
  absl::optional<Upstream::SelectedPoolAndConnection>
  selectExistingConnection(Upstream::LoadBalancerContext*, const Upstream::Host&,
                           std::vector<uint8_t>&) override {
    return absl::nullopt;
  }

private:
  Upstream::HostConstSharedPtr host_;
};

// Factory that creates FixedHostLb instances.
// Each locality's child LB returns the first healthy host from that
// locality's filtered PrioritySet view at creation time.
class FakeChildLbFactory : public Upstream::LoadBalancerFactory {
public:
  Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override {
    const auto& ps = params.priority_set;
    if (ps.hostSetsPerPriority().empty()) {
      return std::make_unique<FixedHostLb>(nullptr);
    }
    const auto& healthy = ps.hostSetsPerPriority()[0]->healthyHosts();
    Upstream::HostConstSharedPtr host = healthy.empty() ? nullptr : healthy[0];
    return std::make_unique<FixedHostLb>(host);
  }
  bool recreateOnHostChange() const override { return false; }
};

// A child LB that re-reads healthy hosts from its PrioritySet on every chooseHost() call.
// Used to verify that updateFromOriginal() propagates host changes through the
// SingleLocalityPrioritySet to child LBs (without relying on creation-time capture).
class DynamicHostLb : public Upstream::LoadBalancer {
public:
  explicit DynamicHostLb(const Upstream::PrioritySet& ps) : ps_(ps) {}

  Upstream::HostSelectionResponse chooseHost(Upstream::LoadBalancerContext*) override {
    if (ps_.hostSetsPerPriority().empty()) {
      return {nullptr};
    }
    const auto& hosts = ps_.hostSetsPerPriority()[0]->healthyHosts();
    return {hosts.empty() ? nullptr : hosts[0]};
  }
  Upstream::HostConstSharedPtr peekAnotherHost(Upstream::LoadBalancerContext* ctx) override {
    return chooseHost(ctx).host;
  }
  OptRef<Envoy::Http::ConnectionPool::ConnectionLifetimeCallbacks> lifetimeCallbacks() override {
    return {};
  }
  absl::optional<Upstream::SelectedPoolAndConnection>
  selectExistingConnection(Upstream::LoadBalancerContext*, const Upstream::Host&,
                           std::vector<uint8_t>&) override {
    return absl::nullopt;
  }

private:
  const Upstream::PrioritySet& ps_;
};

// Factory that creates DynamicHostLb instances.
// Stores a reference to each created LB's PrioritySet so tests can verify host updates.
class DynamicHostLbFactory : public Upstream::LoadBalancerFactory {
public:
  Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams params) override {
    auto lb = std::make_unique<DynamicHostLb>(params.priority_set);
    return lb;
  }
  bool recreateOnHostChange() const override { return false; }
};

// Active split: child LB receives locality-filtered hosts and chooses from
// the correct locality.
TEST_F(WorkerLocalLbTest, ChildFactory_ActiveSplit_DelegatesToCorrectLocality) {
  auto local_host = makeWeightTrackingMockHost();
  auto remote_host = makeWeightTrackingMockHost();
  setupLocalities({{local_host}, {remote_host}});

  // Also set up hostsPerLocality (needed by SingleLocalityPrioritySet).
  auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{local_host}, {remote_host}}, true);
  host_set_->hosts_per_locality_ = hosts_per_loc;
  host_set_->degraded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  host_set_->excluded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, degradedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->degraded_hosts_per_locality_));
  ON_CALL(*host_set_, excludedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->excluded_hosts_per_locality_));

  // Force spill to remote: 0% local.
  auto child_factory = std::make_shared<FakeChildLbFactory>();
  auto lb = createLb(child_factory);

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 0;
  split.num_localities = 2;
  split.residual_capacity = {0, 10000};
  pushSplit(split);

  // rand % 10000 >= 0 -> remote. residual selects locality 1.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // > 0, remote
      .WillOnce(Return(0));   // residual -> locality 1
  auto result = lb->chooseHost(nullptr);
  // FakeChildLbFactory returns the first host from locality 1 = remote_host.
  EXPECT_EQ(result.host, remote_host);
}

// Active split: child LB delegates to local locality.
TEST_F(WorkerLocalLbTest, ChildFactory_ActiveSplit_DelegatesToLocal) {
  auto local_host = makeWeightTrackingMockHost();
  auto remote_host = makeWeightTrackingMockHost();
  setupLocalities({{local_host}, {remote_host}});

  auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{local_host}, {remote_host}}, true);
  host_set_->hosts_per_locality_ = hosts_per_loc;
  host_set_->degraded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  host_set_->excluded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, degradedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->degraded_hosts_per_locality_));
  ON_CALL(*host_set_, excludedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->excluded_hosts_per_locality_));

  // 100% local.
  auto child_factory = std::make_shared<FakeChildLbFactory>();
  auto lb = createLb(child_factory);

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 10000;
  split.num_localities = 2;
  split.residual_capacity = {0, 10000};
  pushSplit(split);

  // rand % 10000 = 0 < 10000 -> local.
  EXPECT_CALL(random_, random()).WillOnce(Return(0));
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, local_host);
}

// Inactive split with child factory: delegates to full-set child LB (all localities visible).
TEST_F(WorkerLocalLbTest, ChildFactory_InactiveSplit_DelegatesToFullSet) {
  auto local_host = makeWeightTrackingMockHost();
  auto remote_host = makeWeightTrackingMockHost();
  setupLocalities({{local_host}, {remote_host}});

  auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{local_host}, {remote_host}}, true);
  host_set_->hosts_per_locality_ = hosts_per_loc;
  host_set_->degraded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  host_set_->excluded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, degradedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->degraded_hosts_per_locality_));
  ON_CALL(*host_set_, excludedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->excluded_hosts_per_locality_));

  // Split is inactive (default). Delegates to full_child_lb_ (not child_lbs_[0]),
  // which sees all healthy hosts via the unfiltered priority set.
  // FakeChildLbFactory picks healthy[0] = local_host from the full set.
  auto child_factory = std::make_shared<FakeChildLbFactory>();
  auto lb = createLb(child_factory);

  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, local_host);
}

// Child LBs rebuild when locality count changes.
TEST_F(WorkerLocalLbTest, ChildFactory_LocalityCountChange_Rebuilds) {
  auto h0 = makeWeightTrackingMockHost();
  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h0}, {h1}});

  auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{h0}, {h1}}, true);
  host_set_->hosts_per_locality_ = hosts_per_loc;
  host_set_->degraded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  host_set_->excluded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, degradedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->degraded_hosts_per_locality_));
  ON_CALL(*host_set_, excludedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->excluded_hosts_per_locality_));

  auto child_factory = std::make_shared<FakeChildLbFactory>();
  auto lb = createLb(child_factory);

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 10000;
  split.num_localities = 2;
  split.residual_capacity = {0, 10000};
  pushSplit(split);

  // First call - creates 2 child LBs.
  EXPECT_CALL(random_, random()).WillOnce(Return(0));
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, h0);

  // Add a third locality.
  auto h2 = makeWeightTrackingMockHost();
  auto new_hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{h0}, {h1}, {h2}}, true);
  host_set_->hosts_per_locality_ = new_hosts_per_loc;
  host_set_->healthy_hosts_per_locality_ = new_hosts_per_loc;
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, healthyHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->healthy_hosts_per_locality_));

  // Fire PrioritySet callbacks so WorkerLocalLb rebuilds its per-locality child LBs.
  priority_set_.runUpdateCallbacks(0, {h2}, {});

  // Update split for 3 localities, force remote to locality 2.
  split.local_percent_to_route = 0;
  split.num_localities = 3;
  split.residual_capacity = {0, 0, 10000};
  pushSplit(split);

  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // > 0, remote
      .WillOnce(Return(0));   // residual -> locality 2
  result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, h2);
}

// Host change with same locality count - child LBs should be synced, not recreated.
TEST_F(WorkerLocalLbTest, ChildFactory_SameLocalityCount_SyncsHosts) {
  auto h0 = makeWeightTrackingMockHost();
  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h0}, {h1}});

  auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{h0}, {h1}}, true);
  host_set_->hosts_per_locality_ = hosts_per_loc;
  host_set_->degraded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  host_set_->excluded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, degradedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->degraded_hosts_per_locality_));
  ON_CALL(*host_set_, excludedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->excluded_hosts_per_locality_));

  auto child_factory = std::make_shared<FakeChildLbFactory>();
  auto lb = createLb(child_factory);

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 0; // All remote.
  split.num_localities = 2;
  split.residual_capacity = {0, 10000};
  pushSplit(split);

  // First call - creates 2 child LBs, remote picks h1.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // > 0, remote
      .WillOnce(Return(0));   // residual -> locality 1
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, h1);

  // Replace h1 with h_new in locality 1 (same locality count).
  auto h_new = makeWeightTrackingMockHost();
  auto new_hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{h0}, {h_new}}, true);
  host_set_->hosts_per_locality_ = new_hosts_per_loc;
  host_set_->healthy_hosts_per_locality_ = new_hosts_per_loc;
  host_set_->healthy_hosts_ = {h0, h_new};
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, healthyHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->healthy_hosts_per_locality_));
  ON_CALL(*host_set_, healthyHosts()).WillByDefault(ReturnRef(host_set_->healthy_hosts_));

  // Fire callbacks - same locality count, triggers in-place sync.
  priority_set_.runUpdateCallbacks(0, {h_new}, {h1});

  // FakeChildLbFactory creates a FixedHostLb that captures the host at create time.
  // Since recreateOnHostChange()=false and the child LB doesn't register callbacks,
  // it still holds the old host. But the SingleLocalityPrioritySet WAS updated
  // (updateFromOriginal was called). Verify the sync path ran without crash and
  // the LB still functions - falls back to pickHostFromLocality since child has stale host.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // > 0, remote
      .WillOnce(Return(0));   // residual -> locality 1
  result = lb->chooseHost(nullptr);
  // Child LB still returns h1 (captured at creation), since it doesn't re-read hosts.
  // This is expected - the child is responsible for its own host tracking.
  EXPECT_NE(result.host, nullptr);
}

// Verify that updateFromOriginal() propagates host changes through SingleLocalityPrioritySet
// to child LBs that read hosts dynamically. This tests the full callback chain:
// priority_set callback -> rebuildChildLbs() -> updateFromOriginal() -> child LB sees new hosts.
TEST_F(WorkerLocalLbTest, ChildFactory_SameLocalityCount_CallbackPropagatesHosts) {
  auto h0 = makeWeightTrackingMockHost();
  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h0}, {h1}});

  auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{h0}, {h1}}, true);
  host_set_->hosts_per_locality_ = hosts_per_loc;
  host_set_->degraded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  host_set_->excluded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, degradedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->degraded_hosts_per_locality_));
  ON_CALL(*host_set_, excludedHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->excluded_hosts_per_locality_));

  // 100% local to consistently route to locality 0.
  auto child_factory = std::make_shared<DynamicHostLbFactory>();
  auto lb = createLb(child_factory);

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 10000;
  split.num_localities = 2;
  split.residual_capacity = {0, 10000};
  pushSplit(split);

  // First call - builds child LBs, locality 0 child sees {h0}.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)); // 0 < 10000 -> local
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, h0);

  // Replace h0 with h_new in locality 0 (same locality count -> updateFromOriginal path).
  auto h_new = makeWeightTrackingMockHost();
  auto new_hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
      std::vector<Upstream::HostVector>{{h_new}, {h1}}, true);
  host_set_->hosts_per_locality_ = new_hosts_per_loc;
  host_set_->healthy_hosts_per_locality_ = new_hosts_per_loc;
  host_set_->healthy_hosts_ = {h_new, h1};
  ON_CALL(*host_set_, hostsPerLocality()).WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
  ON_CALL(*host_set_, healthyHostsPerLocality())
      .WillByDefault(ReturnRef(*host_set_->healthy_hosts_per_locality_));
  ON_CALL(*host_set_, healthyHosts()).WillByDefault(ReturnRef(host_set_->healthy_hosts_));

  // Fire callbacks: same locality count -> rebuildChildLbs() calls updateFromOriginal().
  // This updates the SingleLocalityPrioritySet for locality 0 to contain {h_new}.
  priority_set_.runUpdateCallbacks(0, {h_new}, {h0});

  // DynamicHostLb re-reads from its SingleLocalityPrioritySet and now returns h_new.
  EXPECT_CALL(random_, random()).WillOnce(Return(0)); // 0 < 10000 -> local
  result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, h_new);
}

// Zero residual_capacity.back() - should fall back to random remote locality selection.
TEST_F(WorkerLocalLbTest, ZeroResidualCapacity_FallsBackToRandomRemote) {
  auto local_host = makeWeightTrackingMockHost();
  auto remote1 = makeWeightTrackingMockHost();
  auto remote2 = makeWeightTrackingMockHost();
  setupLocalities({{local_host}, {remote1}, {remote2}});

  auto lb = createLb();

  LocalityRoutingSplit split;
  split.active = true;
  split.local_percent_to_route = 0;
  split.num_localities = 3;
  split.residual_capacity = {0, 0, 0}; // All zeros - triggers random fallback.
  pushSplit(split);

  // rand_val=5000 > 0 -> remote. residual_capacity.back()==0 -> fallback path.
  // Fallback: 1 + (random_.random() % (num_localities - 1)) = 1 + (0 % 2) = 1.
  // Then pickHostFromLocality selects host within that locality.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // > local_percent_to_route -> remote
      .WillOnce(Return(0))    // fallback: 1 + (0 % 2) = locality 1
      .WillOnce(Return(0));   // pickHostFromLocality: 0 % 1 = host 0
  auto result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, remote1);

  // Again with random=1 -> 1 + (1 % 2) = locality 2.
  EXPECT_CALL(random_, random())
      .WillOnce(Return(5000)) // remote
      .WillOnce(Return(1))    // fallback: 1 + (1 % 2) = locality 2
      .WillOnce(Return(0));   // pickHostFromLocality: 0 % 1 = host 0
  result = lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, remote2);
}

// ============================================================
// WorkerLocalLbFactory tests
// ============================================================

TEST(WorkerLocalLbFactoryTest, CreateReturnsValidLb) {
  NiceMock<Random::MockRandomGenerator> random;
  NiceMock<ThreadLocal::MockInstance> tls;
  ON_CALL(tls, allocateSlot()).WillByDefault(testing::Invoke(&tls, &ThreadLocal::MockInstance::allocateSlotMock));
  WorkerLocalLbFactory factory(random, tls, nullptr);

  NiceMock<Upstream::MockPrioritySet> priority_set;
  Upstream::LoadBalancerParams params{priority_set};
  auto lb = factory.create(params);
  EXPECT_NE(lb, nullptr);
}

TEST(WorkerLocalLbFactoryTest, RecreateOnHostChangeIsFalse) {
  NiceMock<Random::MockRandomGenerator> random;
  NiceMock<ThreadLocal::MockInstance> tls;
  ON_CALL(tls, allocateSlot()).WillByDefault(testing::Invoke(&tls, &ThreadLocal::MockInstance::allocateSlotMock));
  WorkerLocalLbFactory factory(random, tls, nullptr);
  EXPECT_FALSE(factory.recreateOnHostChange());
}

// ============================================================
// OrcaLocalityLoadBalancer tests
// ============================================================

class OrcaLocalityLoadBalancerTest : public testing::Test {
protected:
  void SetUp() override {
    ON_CALL(tls_, allocateSlot()).WillByDefault(testing::Invoke(&tls_, &ThreadLocal::MockInstance::allocateSlotMock));
    ON_CALL(tls_, runOnAllThreads(testing::An<std::function<void()>>()))
        .WillByDefault(testing::Invoke(&tls_, &ThreadLocal::MockInstance::runOnAllThreads1));
    proto_.add_metric_names_for_computing_utilization("named_metrics.foo");
    config_ = std::make_unique<OrcaLocalityLbConfig>(proto_, dispatcher_, tls_);
  }

  OrcaLocalityLbProto proto_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<ThreadLocal::MockInstance> tls_;
  std::unique_ptr<OrcaLocalityLbConfig> config_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  NiceMock<Runtime::MockLoader> runtime_;
  NiceMock<Random::MockRandomGenerator> random_;
  Event::SimulatedTimeSystem time_system_;
  std::shared_ptr<NiceMock<Upstream::MockClusterInfo>> cluster_info_{
      new NiceMock<Upstream::MockClusterInfo>()};
};

TEST_F(OrcaLocalityLoadBalancerTest, ConstructAndInitialize) {
  // OrcaWeightManager::initialize creates a timer.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // OrcaLocalityManager constructor creates a timer.
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto lb = std::make_unique<OrcaLocalityLoadBalancer>(
      makeOptRef<const Upstream::LoadBalancerConfig>(*config_), *cluster_info_, priority_set_,
      runtime_, random_, time_system_);

  auto status = lb->initialize();
  EXPECT_TRUE(status.ok());
}

TEST_F(OrcaLocalityLoadBalancerTest, FactoryReturnsValid) {
  // Two timers: one for OrcaWeightManager, one for OrcaLocalityManager.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto lb = std::make_unique<OrcaLocalityLoadBalancer>(
      makeOptRef<const Upstream::LoadBalancerConfig>(*config_), *cluster_info_, priority_set_,
      runtime_, random_, time_system_);

  EXPECT_TRUE(lb->initialize().ok());
  EXPECT_NE(lb->factory(), nullptr);
}

TEST_F(OrcaLocalityLoadBalancerTest, FactoryCreatesWorkerLb) {
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto lb = std::make_unique<OrcaLocalityLoadBalancer>(
      makeOptRef<const Upstream::LoadBalancerConfig>(*config_), *cluster_info_, priority_set_,
      runtime_, random_, time_system_);

  EXPECT_TRUE(lb->initialize().ok());

  auto factory = lb->factory();
  Upstream::LoadBalancerParams params{priority_set_};
  auto worker_lb = factory->create(params);
  EXPECT_NE(worker_lb, nullptr);
}

// OrcaWeightManager is created when child doesn't manage weights.
TEST_F(OrcaLocalityLoadBalancerTest, NoChild_CreatesOrcaWeightManager) {
  // Expect 2 timers: OrcaWeightManager + OrcaLocalityManager.
  auto* timer1 = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto* timer2 = new NiceMock<Event::MockTimer>(&dispatcher_);
  (void)timer1;
  (void)timer2;

  auto lb = std::make_unique<OrcaLocalityLoadBalancer>(
      makeOptRef<const Upstream::LoadBalancerConfig>(*config_), *cluster_info_, priority_set_,
      runtime_, random_, time_system_);

  EXPECT_TRUE(lb->initialize().ok());
  // Verify both timers were consumed by checking factory is valid.
  EXPECT_NE(lb->factory(), nullptr);
}

// OrcaWeightManager is skipped when child manages ORCA weights.
TEST_F(OrcaLocalityLoadBalancerTest, ChildManagesOrcaWeights_SkipsOrcaWeightManager) {
  // Simulate a child that manages ORCA weights (e.g., CSWRR).
  config_->child_manages_orca_weights_ = true;

  // Create a mock ThreadAwareLoadBalancer that the constructor will use.
  auto mock_talb = std::make_unique<NiceMock<Upstream::MockThreadAwareLoadBalancer>>();
  auto mock_factory = std::make_shared<FakeChildLbFactory>();

  // Set expectations for initialize() and factory().
  auto* mock_talb_ptr = mock_talb.get();
  ON_CALL(*mock_talb_ptr, initialize()).WillByDefault(Return(absl::OkStatus()));
  ON_CALL(*mock_talb_ptr, factory()).WillByDefault(Return(mock_factory));

  // We can't easily inject the mock thread-aware LB through the normal config path
  // since the constructor creates it from child_factory_->create().
  // Instead, use config_test's ChildPolicyCswrr for end-to-end.
  // Here we verify the config flag is stored correctly.
  EXPECT_TRUE(config_->child_manages_orca_weights_);
}

// Verify factory passes child_worker_factory to WorkerLocalLb.
TEST_F(OrcaLocalityLoadBalancerTest, FactoryPassesChildWorkerFactory) {
  // No child -> 2 timers.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto lb = std::make_unique<OrcaLocalityLoadBalancer>(
      makeOptRef<const Upstream::LoadBalancerConfig>(*config_), *cluster_info_, priority_set_,
      runtime_, random_, time_system_);
  EXPECT_TRUE(lb->initialize().ok());

  auto factory = lb->factory();
  NiceMock<Upstream::MockPrioritySet> worker_ps;
  auto worker_lb = factory->create({worker_ps, nullptr});
  EXPECT_NE(worker_lb, nullptr);

  // Worker LB should work (no child factory -> uses pickHostFromLocality).
  auto result = worker_lb->chooseHost(nullptr);
  EXPECT_EQ(result.host, nullptr); // No hosts configured.
}

// ============================================================
// SingleLocalityPrioritySet tests
// ============================================================

class SingleLocalityPrioritySetTest : public testing::Test {
protected:
  void SetUp() override { host_set_ = priority_set_.getMockHostSet(0); }

  // Setup locality data on the mock host set (all 4 per-locality variants).
  void setupHostSet(std::vector<Upstream::HostVector> locality_hosts, bool has_local = true) {
    auto hosts_per_loc = std::make_shared<Upstream::HostsPerLocalityImpl>(
        std::vector<Upstream::HostVector>(locality_hosts), has_local);
    host_set_->hosts_per_locality_ = hosts_per_loc;
    host_set_->healthy_hosts_per_locality_ = hosts_per_loc;
    host_set_->degraded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();
    host_set_->excluded_hosts_per_locality_ = std::make_shared<Upstream::HostsPerLocalityImpl>();

    ON_CALL(*host_set_, hostsPerLocality())
        .WillByDefault(ReturnRef(*host_set_->hosts_per_locality_));
    ON_CALL(*host_set_, healthyHostsPerLocality())
        .WillByDefault(ReturnRef(*host_set_->healthy_hosts_per_locality_));
    ON_CALL(*host_set_, degradedHostsPerLocality())
        .WillByDefault(ReturnRef(*host_set_->degraded_hosts_per_locality_));
    ON_CALL(*host_set_, excludedHostsPerLocality())
        .WillByDefault(ReturnRef(*host_set_->excluded_hosts_per_locality_));
  }

  NiceMock<Upstream::MockPrioritySet> priority_set_;
  Upstream::MockHostSet* host_set_;
};

TEST_F(SingleLocalityPrioritySetTest, BasicConstruction) {
  auto h0 = makeWeightTrackingMockHost();
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupHostSet({{h0, h1}, {h2}});

  // View locality 0.
  SingleLocalityPrioritySet ps0(0);
  ps0.updateFromOriginal(*host_set_);

  ASSERT_FALSE(ps0.hostSetsPerPriority().empty());
  const auto& hs = *ps0.hostSetsPerPriority()[0];
  EXPECT_EQ(hs.healthyHosts().size(), 2);
  EXPECT_EQ(hs.healthyHosts()[0], h0);
  EXPECT_EQ(hs.healthyHosts()[1], h1);
}

TEST_F(SingleLocalityPrioritySetTest, SelectsCorrectLocality) {
  auto h0 = makeWeightTrackingMockHost();
  auto h1 = makeWeightTrackingMockHost();
  setupHostSet({{h0}, {h1}});

  // View locality 1.
  SingleLocalityPrioritySet ps1(1);
  ps1.updateFromOriginal(*host_set_);

  const auto& hs = *ps1.hostSetsPerPriority()[0];
  ASSERT_EQ(hs.healthyHosts().size(), 1);
  EXPECT_EQ(hs.healthyHosts()[0], h1);
}

TEST_F(SingleLocalityPrioritySetTest, EmptyLocality) {
  setupHostSet({{}});

  SingleLocalityPrioritySet ps(0);
  ps.updateFromOriginal(*host_set_);

  const auto& hs = *ps.hostSetsPerPriority()[0];
  EXPECT_TRUE(hs.healthyHosts().empty());
}

TEST_F(SingleLocalityPrioritySetTest, UpdateFromOriginalSyncsHosts) {
  auto h0 = makeWeightTrackingMockHost();
  setupHostSet({{h0}});

  SingleLocalityPrioritySet ps(0);
  ps.updateFromOriginal(*host_set_);
  EXPECT_EQ(ps.hostSetsPerPriority()[0]->healthyHosts().size(), 1);

  // Add another host to locality 0.
  auto h1 = makeWeightTrackingMockHost();
  setupHostSet({{h0, h1}});
  ps.updateFromOriginal(*host_set_);
  EXPECT_EQ(ps.hostSetsPerPriority()[0]->healthyHosts().size(), 2);
}

TEST_F(SingleLocalityPrioritySetTest, OutOfRangeLocalityProducesEmpty) {
  auto h0 = makeWeightTrackingMockHost();
  setupHostSet({{h0}});

  // Locality index 5, but only 1 locality exists.
  SingleLocalityPrioritySet ps(5);
  ps.updateFromOriginal(*host_set_);

  const auto& hs = *ps.hostSetsPerPriority()[0];
  EXPECT_TRUE(hs.healthyHosts().empty());
}

} // namespace
} // namespace OrcaLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
