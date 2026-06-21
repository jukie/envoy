#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "envoy/extensions/load_balancing_policies/round_robin/v3/round_robin.pb.h"

#include "source/extensions/load_balancing_policies/load_aware_locality/config.h"
#include "source/extensions/load_balancing_policies/load_aware_locality/load_aware_locality_lb.h"

#include "test/common/upstream/utility.h"
#include "test/mocks/server/factory_context.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/load_balancer.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/mocks/upstream/typed_load_balancer_factory.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/utility.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace LoadAwareLocality {
namespace {

using testing::NiceMock;
using testing::Return;

// Distinct Locality identity for locality-group index `i`. Region strings sort lexicographically by
// index ("r00", "r01", ...) so the default setup order matches a local-first/lexicographic
// ordering.
envoy::config::core::v3::Locality localityForIndex(size_t i) {
  return Upstream::Locality(absl::StrCat("r", absl::Dec(i, absl::kZeroPad2)), "z", "sz");
}

std::shared_ptr<NiceMock<Upstream::MockHost>>
makeWeightTrackingMockHost(uint32_t initial_weight = 1) {
  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  auto weight = std::make_shared<uint32_t>(initial_weight);
  ON_CALL(*host, weight()).WillByDefault([weight]() -> uint32_t { return *weight; });
  ON_CALL(*host, weight(::testing::_)).WillByDefault([weight](uint32_t new_weight) {
    *weight = new_weight;
  });
  return host;
}

Upstream::HostVector makeHosts(uint32_t count, uint32_t weight = 1) {
  Upstream::HostVector hosts;
  hosts.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    hosts.push_back(makeWeightTrackingMockHost(weight));
  }
  return hosts;
}

class RecordingWorkerChildFactory : public Upstream::LoadBalancerFactory {
public:
  explicit RecordingWorkerChildFactory(bool recreate_on_host_change)
      : recreate_on_host_change_(recreate_on_host_change) {}

  Upstream::LoadBalancerPtr create(Upstream::LoadBalancerParams) override {
    ++create_count_;
    return std::make_unique<NiceMock<Upstream::MockLoadBalancer>>();
  }

  bool recreateOnHostChangeDeprecated() const override { return recreate_on_host_change_; }

  uint32_t createCount() const { return create_count_; }

private:
  const bool recreate_on_host_change_;
  uint32_t create_count_{0};
};

class StaticThreadAwareLoadBalancer : public Upstream::ThreadAwareLoadBalancer {
public:
  explicit StaticThreadAwareLoadBalancer(Upstream::LoadBalancerFactorySharedPtr factory)
      : factory_(std::move(factory)) {}

  Upstream::LoadBalancerFactorySharedPtr factory() override { return factory_; }
  absl::Status initialize() override { return absl::OkStatus(); }

private:
  Upstream::LoadBalancerFactorySharedPtr factory_;
};

uint64_t counterValue(NiceMock<Upstream::MockClusterInfo>& info, const std::string& name) {
  auto c = info.stats_store_.findCounterByString(name);
  return c.has_value() ? c->get().value() : 0;
}

class LoadAwareLocalityLbTest : public Event::TestUsingSimulatedTime, public testing::Test {
protected:
  void SetUp() override {
    // Advance past the simulated-time epoch so host ORCA timestamps are non-zero
    // (lastUpdateTimeMs() == 0 means "never reported" in the weight computation).
    simTime().advanceTimeWait(std::chrono::hours(1));
    ON_CALL(context_, mainThreadDispatcher()).WillByDefault(ReturnRef(dispatcher_));
    ON_CALL(random_, random()).WillByDefault([this]() -> uint64_t {
      if (forced_random_index_ < forced_random_values_.size()) {
        return forced_random_values_[forced_random_index_++];
      }
      constexpr uint64_t step = std::numeric_limits<uint64_t>::max() / 100;
      return (random_call_count_++ % 100) * step;
    });
  }

  void forceRandomValues(std::vector<uint64_t> values) {
    forced_random_values_ = std::move(values);
    forced_random_index_ = 0;
  }

  void clearForcedRandomValues() {
    forced_random_values_.clear();
    forced_random_index_ = 0;
  }

  void createLb(double variance_threshold = 0.1, double ewma_alpha = 1.0,
                double remote_probe_fraction = 0.0,
                std::chrono::milliseconds weight_expiration_period = std::chrono::milliseconds(0)) {
    auto weight_update_period = std::chrono::milliseconds(1000);

    envoy::extensions::load_balancing_policies::round_robin::v3::RoundRobin round_robin;
    envoy::config::core::v3::TypedExtensionConfig round_robin_config;
    round_robin_config.set_name("envoy.load_balancing_policies.round_robin");
    round_robin_config.mutable_typed_config()->PackFrom(round_robin);

    auto& round_robin_factory =
        Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(round_robin_config);

    auto round_robin_proto = round_robin_factory.createEmptyConfigProto();
    ASSERT_TRUE(Config::Utility::translateOpaqueConfig(round_robin_config.typed_config(),
                                                       context_.messageValidationVisitor(),
                                                       *round_robin_proto)
                    .ok());
    auto round_robin_lb_config =
        round_robin_factory.loadConfig(context_, *round_robin_proto).value();

    auto lb_config = std::make_unique<LoadAwareLocalityLbConfig>(
        round_robin_factory, round_robin_factory.name(), std::move(round_robin_lb_config),
        weight_update_period, variance_threshold, ewma_alpha, remote_probe_fraction,
        weight_expiration_period, std::vector<std::string>{}, dispatcher_, context_.thread_local_);

    timer_ = new NiceMock<Event::MockTimer>(&dispatcher_);

    thread_aware_lb_ = std::make_unique<LoadAwareLocalityLoadBalancer>(
        *lb_config, cluster_info_, priority_set_, context_.runtime_loader_, random_,
        context_.time_system_);

    ASSERT_TRUE(thread_aware_lb_->initialize().ok());
    factory_ = thread_aware_lb_->factory();
  }

  std::shared_ptr<WorkerLocalLbFactory> createRoundRobinWorkerFactory() {
    envoy::extensions::load_balancing_policies::round_robin::v3::RoundRobin round_robin;
    envoy::config::core::v3::TypedExtensionConfig round_robin_config;
    round_robin_config.set_name("envoy.load_balancing_policies.round_robin");
    round_robin_config.mutable_typed_config()->PackFrom(round_robin);

    auto& round_robin_factory =
        Config::Utility::getAndCheckFactory<Upstream::TypedLoadBalancerFactory>(round_robin_config);

    auto round_robin_proto = round_robin_factory.createEmptyConfigProto();
    EXPECT_TRUE(Config::Utility::translateOpaqueConfig(round_robin_config.typed_config(),
                                                       context_.messageValidationVisitor(),
                                                       *round_robin_proto)
                    .ok());
    auto round_robin_lb_config =
        round_robin_factory.loadConfig(context_, *round_robin_proto).value();

    auto factory = std::make_shared<WorkerLocalLbFactory>(
        round_robin_factory, round_robin_factory.name(),
        LoadBalancerConfigSharedPtr(std::move(round_robin_lb_config)), cluster_info_, priority_set_,
        context_.runtime_loader_, random_, context_.time_system_, context_.thread_local_);
    EXPECT_TRUE(factory->initializeChildLb().ok());
    return factory;
  }

  // Builds a WorkerLocalLbFactory whose child create() yields a StaticThreadAwareLoadBalancer
  // wrapping `child`.
  std::shared_ptr<WorkerLocalLbFactory>
  makeWorkerFactoryWithChild(Upstream::LoadBalancerFactorySharedPtr child,
                             const std::string& name) {
    auto typed_child_factory = std::make_shared<NiceMock<Upstream::MockTypedLoadBalancerFactory>>();
    ON_CALL(*typed_child_factory, name()).WillByDefault(Return(name));
    ON_CALL(*typed_child_factory,
            create(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
        .WillByDefault(
            [child](auto&&...) { return std::make_unique<StaticThreadAwareLoadBalancer>(child); });
    recording_typed_child_factory_ = typed_child_factory;

    auto factory = std::make_shared<WorkerLocalLbFactory>(
        *typed_child_factory, name,
        std::make_shared<Upstream::MockTypedLoadBalancerFactory::EmptyLoadBalancerConfig>(),
        cluster_info_, priority_set_, context_.runtime_loader_, random_, context_.time_system_,
        context_.thread_local_);
    EXPECT_TRUE(factory->initializeChildLb().ok());
    return factory;
  }

  void setupPriorityLocalities(
      uint32_t priority, std::vector<Upstream::HostVector> localities,
      bool has_local_locality = false,
      absl::optional<std::vector<Upstream::HostVector>> healthy_localities = absl::nullopt,
      absl::optional<std::vector<Upstream::HostVector>> degraded_localities = absl::nullopt) {
    auto* host_set = priority_set_.getMockHostSet(priority);
    // Stamp each host with its locality-group identity so the policy can key weights by Locality.
    for (size_t i = 0; i < localities.size(); ++i) {
      for (const auto& host : localities[i]) {
        setHostLocality(host, i);
      }
    }
    Upstream::HostVector all_hosts;
    for (const auto& locality : localities) {
      all_hosts.insert(all_hosts.end(), locality.begin(), locality.end());
    }
    host_set->hosts_ = all_hosts;
    host_set->hosts_per_locality_ =
        Upstream::makeHostsPerLocality(std::move(localities), !has_local_locality);

    if (healthy_localities.has_value()) {
      Upstream::HostVector healthy_hosts;
      for (const auto& locality : *healthy_localities) {
        healthy_hosts.insert(healthy_hosts.end(), locality.begin(), locality.end());
      }
      host_set->healthy_hosts_ = healthy_hosts;
      host_set->healthy_hosts_per_locality_ =
          Upstream::makeHostsPerLocality(std::move(*healthy_localities), !has_local_locality);
    } else {
      host_set->healthy_hosts_ = all_hosts;
      host_set->healthy_hosts_per_locality_ = host_set->hosts_per_locality_;
    }

    if (degraded_localities.has_value()) {
      Upstream::HostVector degraded_hosts;
      for (const auto& locality : *degraded_localities) {
        degraded_hosts.insert(degraded_hosts.end(), locality.begin(), locality.end());
      }
      host_set->degraded_hosts_ = degraded_hosts;
      host_set->degraded_hosts_per_locality_ =
          Upstream::makeHostsPerLocality(std::move(*degraded_localities), !has_local_locality);
    } else {
      host_set->degraded_hosts_.clear();
      host_set->degraded_hosts_per_locality_ =
          Upstream::makeHostsPerLocality({}, !has_local_locality);
    }
  }

  void setupLocalities(
      std::vector<Upstream::HostVector> localities, bool has_local_locality = false,
      absl::optional<std::vector<Upstream::HostVector>> healthy_localities = absl::nullopt,
      absl::optional<std::vector<Upstream::HostVector>> degraded_localities = absl::nullopt) {
    setupPriorityLocalities(0, std::move(localities), has_local_locality,
                            std::move(healthy_localities), std::move(degraded_localities));
  }

  // Stamps `host` with the Locality identity for locality-group index `i`.
  void setHostLocality(const Upstream::HostSharedPtr& host, size_t i) {
    setHostLocalityExplicit(host, localityForIndex(i));
  }

  // Stamps `host` with an explicit Locality identity. The Locality is owned by the fixture (stable
  // address) so the mock's locality() can return a reference to it for the test's lifetime.
  void setHostLocalityExplicit(const Upstream::HostSharedPtr& host,
                               const envoy::config::core::v3::Locality& locality) {
    auto* mock = dynamic_cast<Upstream::MockHost*>(host.get());
    if (mock == nullptr) {
      return;
    }
    localities_.push_back(locality);
    ON_CALL(*mock, locality()).WillByDefault(testing::ReturnRef(localities_.back()));
  }

  // Builds a LocalityWeightsMap keyed by the canonical per-index Locality identities, matching the
  // setup order used by setupLocalities/reshapeLocalities (locality i ↔ localityForIndex(i)).
  static LocalityWeightsMap makeWeightsMap(const std::vector<double>& weights) {
    LocalityWeightsMap map;
    for (size_t i = 0; i < weights.size(); ++i) {
      map[localityForIndex(i)] = weights[i];
    }
    return map;
  }

  // Triggers a routing-weight recompute by re-running the thread-aware initialize.
  void recomputeWeights() { ASSERT_TRUE(thread_aware_lb_->initialize().ok()); }

  // Reshapes a priority's host set, then fires the membership update callback. Two modes:
  //  - full membership reshape: sets hosts_/healthy_hosts_ and both per-locality vectors from
  //    `localities` (healthy mirrors all hosts).
  //  - health-only flip (when `healthy_override` is set): leaves hosts_/hosts_per_locality_ intact
  //    and only sets healthy_hosts_/healthy_hosts_per_locality_ from `healthy_override`.
  void reshapeLocalities(
      uint32_t priority, std::vector<Upstream::HostVector> localities,
      Upstream::HostVector added = {}, Upstream::HostVector removed = {}, bool has_local = false,
      absl::optional<std::vector<Upstream::HostVector>> healthy_override = absl::nullopt) {
    auto* host_set = priority_set_.getMockHostSet(priority);
    if (healthy_override.has_value()) {
      Upstream::HostVector healthy_hosts;
      for (const auto& locality : *healthy_override) {
        healthy_hosts.insert(healthy_hosts.end(), locality.begin(), locality.end());
      }
      host_set->healthy_hosts_ = healthy_hosts;
      host_set->healthy_hosts_per_locality_ =
          Upstream::makeHostsPerLocality(std::move(*healthy_override), !has_local);
    } else {
      for (size_t i = 0; i < localities.size(); ++i) {
        for (const auto& host : localities[i]) {
          setHostLocality(host, i);
        }
      }
      Upstream::HostVector all_hosts;
      for (const auto& locality : localities) {
        all_hosts.insert(all_hosts.end(), locality.begin(), locality.end());
      }
      host_set->hosts_ = all_hosts;
      host_set->healthy_hosts_ = all_hosts;
      host_set->hosts_per_locality_ =
          Upstream::makeHostsPerLocality(std::move(localities), !has_local);
      host_set->healthy_hosts_per_locality_ = host_set->hosts_per_locality_;
    }
    priority_set_.runUpdateCallbacks(priority, added, removed);
  }

  static xds::data::orca::v3::OrcaLoadReport makeOrcaReport(double app_utilization) {
    xds::data::orca::v3::OrcaLoadReport report;
    report.set_application_utilization(app_utilization);
    return report;
  }

  void setHostUtilization(Upstream::HostSharedPtr host, double utilization) {
    auto host_data = host->typedLbPolicyData<LocalityLbHostData>();
    ASSERT(host_data.has_value());
    EXPECT_TRUE(host_data->onOrcaLoadReport(makeOrcaReport(utilization), stream_info_).ok());
  }

  void setUtilizationForHosts(const Upstream::HostVector& hosts, double utilization) {
    for (const auto& host : hosts) {
      setHostUtilization(host, utilization);
    }
  }

  int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               context_.time_system_.monotonicTime().time_since_epoch())
        .count();
  }

  Upstream::LoadBalancerPtr createWorkerLb() {
    EXPECT_NE(nullptr, factory_);
    auto worker_lb = factory_->create({priority_set_, nullptr});
    EXPECT_NE(nullptr, worker_lb);
    return worker_lb;
  }

  const RoutingWeightsSnapshot* routingSnapshot() const {
    auto* typed_factory = dynamic_cast<WorkerLocalLbFactory*>(factory_.get());
    EXPECT_NE(nullptr, typed_factory);
    return typed_factory != nullptr ? typed_factory->routingWeights() : nullptr;
  }

  void publishSnapshot(std::shared_ptr<RoutingWeightsSnapshot> snapshot) {
    auto* typed_factory = dynamic_cast<WorkerLocalLbFactory*>(factory_.get());
    ASSERT_NE(nullptr, typed_factory);
    typed_factory->updateRoutingWeights(std::move(snapshot));
  }

  // Builds an advisory locality-weights snapshot. Priority/health/panic selection is now live
  // worker state (LoadBalancerBase), so the snapshot only carries per-priority locality weights.
  std::shared_ptr<RoutingWeightsSnapshot>
  makeSnapshot(std::vector<PriorityRoutingWeights> priority_weights) {
    auto snapshot = std::make_shared<RoutingWeightsSnapshot>();
    snapshot->priority_weights = std::move(priority_weights);
    return snapshot;
  }

  // Publishes a single-priority snapshot with Healthy weights keyed by per-index Locality identity.
  void publishHealthyWeights(const std::vector<double>& weights) {
    PriorityRoutingWeights pw;
    auto& healthy =
        pw.by_source[static_cast<size_t>(PriorityRoutingWeights::SelectionSource::Healthy)];
    healthy.weights = makeWeightsMap(weights);
    publishSnapshot(makeSnapshot({pw}));
  }

  absl::flat_hash_map<const Upstream::Host*, int> countPicks(Upstream::LoadBalancer& lb,
                                                             int num_picks) {
    absl::flat_hash_map<const Upstream::Host*, int> counts;
    for (int i = 0; i < num_picks; ++i) {
      auto result = lb.chooseHost(nullptr);
      EXPECT_NE(nullptr, result.host);
      if (result.host != nullptr) {
        counts[result.host.get()]++;
      }
    }
    return counts;
  }

  bool hostSeen(Upstream::LoadBalancer& lb, const Upstream::HostConstSharedPtr& target,
                int picks = 200) {
    for (int i = 0; i < picks; ++i) {
      auto result = lb.chooseHost(nullptr);
      if (result.host == target) {
        return true;
      }
    }
    return false;
  }

  void expectOnlyHost(Upstream::LoadBalancer& lb, const Upstream::HostConstSharedPtr& host,
                      int picks = 50) {
    for (int i = 0; i < picks; ++i) {
      auto result = lb.chooseHost(nullptr);
      ASSERT_NE(nullptr, result.host);
      EXPECT_EQ(host, result.host);
    }
  }

  // Asserts the live routing snapshot's `source` weights for `priority` equal `expected`, matching
  // each entry by Locality identity (expected[i] ↔ localityForIndex(i)). A missing identity reads
  // as 0.0 (a locality not in the snapshot map carries no weight). Asserts the snapshot is
  // non-null.
  void expectWeights(PriorityRoutingWeights::SelectionSource source, std::vector<double> expected,
                     double tol = 0.01, uint32_t priority = 0) {
    const auto* snapshot = routingSnapshot();
    ASSERT_NE(nullptr, snapshot);
    const auto& weights = snapshot->priority_weights[priority].weightsFor(source);
    for (size_t i = 0; i < expected.size(); ++i) {
      SCOPED_TRACE(absl::StrCat("locality ", i));
      auto it = weights.find(localityForIndex(i));
      const double actual = it != weights.end() ? it->second : 0.0;
      EXPECT_NEAR(actual, expected[i], tol);
    }
  }

  // Reads the snapshot weight for locality-group index `i` (by identity); 0.0 if absent.
  double weightForIndex(PriorityRoutingWeights::SelectionSource source, size_t i,
                        uint32_t priority = 0) {
    const auto* snapshot = routingSnapshot();
    EXPECT_NE(nullptr, snapshot);
    if (snapshot == nullptr) {
      return 0.0;
    }
    const auto& weights = snapshot->priority_weights[priority].weightsFor(source);
    auto it = weights.find(localityForIndex(i));
    return it != weights.end() ? it->second : 0.0;
  }

  // Asserts the live snapshot's all_local flag for `source`/`priority`. Asserts snapshot non-null.
  void expectAllLocal(bool expected,
                      PriorityRoutingWeights::SelectionSource source =
                          PriorityRoutingWeights::SelectionSource::Healthy,
                      uint32_t priority = 0) {
    const auto* snapshot = routingSnapshot();
    ASSERT_NE(nullptr, snapshot);
    EXPECT_EQ(expected, snapshot->priority_weights[priority].allLocalFor(source));
  }

  // Asserts a single timer tick increments counter `name` by exactly `delta`.
  void expectCounterIncrementsPerTick(const std::string& name, uint64_t delta) {
    const uint64_t before = counterValue(cluster_info_, name);
    timer_->invokeCallback();
    EXPECT_EQ(before + delta, counterValue(cluster_info_, name));
  }

  NiceMock<Server::Configuration::MockServerFactoryContext> context_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Upstream::MockClusterInfo> cluster_info_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  NiceMock<Random::MockRandomGenerator> random_;
  NiceMock<StreamInfo::MockStreamInfo> stream_info_;
  uint64_t random_call_count_{0};
  std::vector<uint64_t> forced_random_values_;
  size_t forced_random_index_{0};
  Event::MockTimer* timer_{};
  // Stable storage for Locality identities returned by mock hosts' locality() (addresses must
  // outlive the hosts; std::deque keeps element addresses stable across pushes).
  std::deque<envoy::config::core::v3::Locality> localities_;

  Upstream::ThreadAwareLoadBalancerPtr thread_aware_lb_;
  Upstream::LoadBalancerFactorySharedPtr factory_;
  std::shared_ptr<NiceMock<Upstream::MockTypedLoadBalancerFactory>> recording_typed_child_factory_;
};

// Table-driven single-tick weight-computation cases: set up localities, push one uniform ORCA
// utilization per locality, recompute once, and assert the resulting Healthy snapshot weights.
// Multi-tick (EWMA), reshape, and distribution-sampling scenarios stay as dedicated TEST_Fs.
struct WeightComputationCase {
  std::string test_name;
  std::vector<int> locality_sizes; // host count per locality group
  bool has_local_locality;
  double variance_threshold;
  double remote_probe_fraction;
  std::vector<double> locality_utils; // utilization applied to every host in the locality
  bool expect_all_local;
  std::vector<double> expected_healthy_weights;
  double tol;
};

class WeightComputationTest : public LoadAwareLocalityLbTest,
                              public testing::WithParamInterface<WeightComputationCase> {};

TEST_P(WeightComputationTest, ComputesHealthyWeights) {
  const WeightComputationCase& c = GetParam();
  std::vector<Upstream::HostVector> localities;
  for (int size : c.locality_sizes) {
    localities.push_back(makeHosts(size));
  }
  setupLocalities(localities, c.has_local_locality);
  createLb(c.variance_threshold, /*ewma_alpha=*/1.0, c.remote_probe_fraction);

  for (size_t i = 0; i < localities.size(); ++i) {
    setUtilizationForHosts(localities[i], c.locality_utils[i]);
  }
  recomputeWeights();

  expectAllLocal(c.expect_all_local);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, c.expected_healthy_weights,
                c.tol);
}

INSTANTIATE_TEST_SUITE_P(WeightComputation, WeightComputationTest,
                         testing::ValuesIn<WeightComputationCase>({
                             {"ScaleWithEligibleHostCounts",
                              {10, 2},
                              false,
                              0.1,
                              0.0,
                              {0.5, 0.5},
                              false,
                              {5.0, 1.0},
                              0.01},
                             {"LocalSaturatedSkipsAllLocal",
                              {1, 1},
                              true,
                              1.0,
                              0.0,
                              {1.0, 0.3},
                              false,
                              {0.0, 0.7},
                              0.01},
                             {"ProbeDistributionAcrossRemotes",
                              {1, 2, 4},
                              true,
                              1.0,
                              0.06,
                              {0.5, 0.5, 0.5},
                              true,
                              {0.94, 0.02, 0.04},
                              0.01},
                             {"ProbeRedistributionClampsLocalAtZero",
                              {1, 1},
                              true,
                              0.0,
                              2.0,
                              {0.3, 0.0},
                              false,
                              {0.0, 1.7},
                              0.01},
                             {"LocalPreferenceRemoteHostWeightedTarget",
                              {1, 9, 1},
                              true,
                              0.01,
                              0.0,
                              {0.5, 0.6, 0.0},
                              true,
                              {1.0, 0.0, 0.0},
                              0.01},
                         }),
                         [](const testing::TestParamInfo<WeightComputationCase>& info) {
                           return info.param.test_name;
                         });

TEST_F(LoadAwareLocalityLbTest, EmptyAndSingleLocalitySmoke) {
  createLb();

  auto empty_lb = createWorkerLb();
  ASSERT_NE(nullptr, empty_lb);
  EXPECT_EQ(nullptr, empty_lb->chooseHost(nullptr).host);
  EXPECT_EQ(nullptr, empty_lb->peekAnotherHost(nullptr));

  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h1}});
  recomputeWeights();

  auto populated_lb = createWorkerLb();
  ASSERT_NE(nullptr, populated_lb);
  EXPECT_EQ(h1, populated_lb->chooseHost(nullptr).host);
  EXPECT_EQ(h1, populated_lb->peekAnotherHost(nullptr));
  EXPECT_FALSE(populated_lb->lifetimeCallbacks().has_value());
  std::vector<uint8_t> hash_key;
  EXPECT_FALSE(populated_lb->selectExistingConnection(nullptr, *h1, hash_key).has_value());
}

TEST_F(LoadAwareLocalityLbTest, EmptyPrioritySetHasNoAvailableHost) {
  // Priority 0 exists (the production invariant for LoadBalancerBase) but carries no hosts.
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_.clear();
  host_set->healthy_hosts_.clear();

  auto child_factory = std::make_shared<RecordingWorkerChildFactory>(false);
  auto factory = makeWorkerFactoryWithChild(child_factory, "mock_empty_priority");

  auto worker_lb = factory->create({priority_set_, nullptr});
  ASSERT_NE(nullptr, worker_lb);
  EXPECT_EQ(nullptr, worker_lb->chooseHost(nullptr).host);
  EXPECT_EQ(nullptr, worker_lb->peekAnotherHost(nullptr));
}

TEST_F(LoadAwareLocalityLbTest, BasicRoutingWeightsTrackNoDataFreshDataAndZeroUtilization) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb();

  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 1.0});

  setHostUtilization(h1, 0.9);
  setHostUtilization(h2, 0.1);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.1, 0.9});

  setHostUtilization(h1, 0.0);
  setHostUtilization(h2, 0.9);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 0.1});
}

TEST_F(LoadAwareLocalityLbTest, WeightExpirationIgnoresStaleData) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0,
           std::chrono::milliseconds(5000));

  // No utilization reported — hosts appear as never-reported, treated the same as expired.
  recomputeWeights();

  // Unreported hosts are ignored — weights are equal (host count only).
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 1.0});

  // Fresh data at current time is used.
  setHostUtilization(h1, 0.9);
  setHostUtilization(h2, 0.1);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.1, 0.9});
}

TEST_F(LoadAwareLocalityLbTest, WeightExpirationDisabledUsesStaleData) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  // Expiration disabled (0ms) — data is always used regardless of age.
  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0,
           std::chrono::milliseconds(0));

  setHostUtilization(h1, 0.9);
  setHostUtilization(h2, 0.1);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.1, 0.9});
}

TEST_F(LoadAwareLocalityLbTest, StaleRemoteCarriesPriorNotZero) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0,
           std::chrono::milliseconds(5000));

  // Local 0.5 is within the variance threshold of remote 0.6, so local is preferred.
  setHostUtilization(h_local, 0.5);
  setHostUtilization(h_remote, 0.6);
  recomputeWeights();

  expectAllLocal(true);

  // Age the remote past expiration while keeping the local fresh. The remote's last-known 0.6
  // must be carried (not reset to 0); otherwise the remote would look idle and local would
  // spuriously spill toward it.
  context_.time_system_.advanceTimeWait(std::chrono::milliseconds(6000));
  setHostUtilization(h_local, 0.5);
  recomputeWeights();

  expectAllLocal(true);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 0.0});
}

TEST_F(LoadAwareLocalityLbTest, StaleLocalityHostCountWeighted) {
  auto locality_a = makeHosts(3);
  auto locality_b = makeHosts(2);
  setupLocalities({locality_a, locality_b});

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0,
           std::chrono::milliseconds(5000));

  setUtilizationForHosts(locality_a, 0.8);
  setUtilizationForHosts(locality_b, 0.5);
  recomputeWeights();

  // Age locality_b past expiration while keeping locality_a fresh. Locality_b is now all-stale
  // and falls back to its host-count baseline (2), not host_count * (1 - 0.5) = 1.
  context_.time_system_.advanceTimeWait(std::chrono::milliseconds(6000));
  setUtilizationForHosts(locality_a, 0.8);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.6, 2.0});
}

TEST_F(LoadAwareLocalityLbTest, LocalPreferenceThresholdBoundaries) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0);

  setHostUtilization(h_local, 0.3);
  setHostUtilization(h_remote, 0.3);
  recomputeWeights();
  expectAllLocal(true);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 0.0});

  setHostUtilization(h_local, 0.6);
  setHostUtilization(h_remote, 0.5);
  recomputeWeights();
  expectAllLocal(true);

  setHostUtilization(h_local, 0.8);
  setHostUtilization(h_remote, 0.2);
  recomputeWeights();
  expectAllLocal(false);
  EXPECT_GT(weightForIndex(PriorityRoutingWeights::SelectionSource::Healthy, 1),
            weightForIndex(PriorityRoutingWeights::SelectionSource::Healthy, 0));
}

TEST_F(LoadAwareLocalityLbTest, NoHealthyHostsWithLocalLocalityDefaultsToLocal) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true,
                  std::vector<Upstream::HostVector>{{}, {}});

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0);

  expectAllLocal(true);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 0.0});
}

TEST_F(LoadAwareLocalityLbTest, ProbeRedistributesToRemoteAndSkipsWhenNoRemoteHealthyHosts) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);

  createLb(/*variance_threshold=*/1.0, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.05);

  setHostUtilization(h_local, 0.3);
  setHostUtilization(h_remote, 1.0);
  recomputeWeights();

  expectAllLocal(true);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.95, 0.05});

  reshapeLocalities(0, /*localities=*/{}, /*added=*/{}, /*removed=*/{}, /*has_local=*/true,
                    std::vector<Upstream::HostVector>{{h_local}, {}});
  recomputeWeights();

  expectAllLocal(true);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 0.0});
}

TEST_F(LoadAwareLocalityLbTest, ProbeSkipsRedistributionWhenTargetAlreadyMetOrTotalIsZero) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);

  createLb(/*variance_threshold=*/0.0, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.1);

  setHostUtilization(h_local, 0.6);
  setHostUtilization(h_remote, 0.5);
  recomputeWeights();

  auto snapshot = routingSnapshot();
  ASSERT_NE(nullptr, snapshot);
  ASSERT_EQ(2, snapshot->priority_weights[0].by_source[0].weights.size());
  EXPECT_FALSE(snapshot->priority_weights[0].by_source[0].all_local);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.4, 0.5});

  setHostUtilization(h_local, 1.0);
  setHostUtilization(h_remote, 1.0);
  recomputeWeights();

  expectAllLocal(false);
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 1.0});
}

TEST_F(LoadAwareLocalityLbTest, HealthyWeightsCanDivergeFromAllHostWeights) {
  auto all_a = makeHosts(4);
  auto all_b = makeHosts(2);
  Upstream::HostVector healthy_a = {all_a[0]};
  Upstream::HostVector healthy_b = {all_b[0], all_b[1]};
  setupLocalities({all_a, all_b}, /*has_local_locality=*/false,
                  std::vector<Upstream::HostVector>{healthy_a, healthy_b});

  createLb();

  const auto* snapshot = routingSnapshot();
  ASSERT_NE(nullptr, snapshot);
  ASSERT_EQ(2, snapshot->priority_weights[0].by_source[0].weights.size());
  ASSERT_EQ(2,
            snapshot->priority_weights[0]
                .by_source[static_cast<size_t>(PriorityRoutingWeights::SelectionSource::AllHosts)]
                .weights.size());
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {1.0, 2.0});
  expectWeights(PriorityRoutingWeights::SelectionSource::AllHosts, {4.0, 2.0});
}

TEST_F(LoadAwareLocalityLbTest, AllLocalitiesSaturatedFallBacksToHostCount) {
  auto locality_a = makeHosts(3);
  auto locality_b = makeHosts(1);
  setupLocalities({locality_a, locality_b});

  createLb();

  setUtilizationForHosts(locality_a, 1.0);
  setUtilizationForHosts(locality_b, 1.0);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {3.0, 1.0});

  auto worker_lb = createWorkerLb();
  auto counts = countPicks(*worker_lb, 400);
  int locality_a_count = 0;
  for (const auto& host : locality_a) {
    locality_a_count += counts[host.get()];
  }
  EXPECT_GT(locality_a_count, counts[locality_b[0].get()] * 2);
}

TEST_F(LoadAwareLocalityLbTest, EwmaLifecycleDampensSpikesAndClearsExpiredState) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/0.3, /*remote_probe_fraction=*/0.0,
           std::chrono::milliseconds(5000));

  setHostUtilization(h1, 0.5);
  setHostUtilization(h2, 0.5);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.5, 0.5});

  setHostUtilization(h1, 1.0);
  recomputeWeights();
  expectWeights(PriorityRoutingWeights::SelectionSource::Healthy, {0.35, 0.5}, 0.05);

  context_.time_system_.advanceTimeWait(std::chrono::milliseconds(6000));
  recomputeWeights();

  const auto* snapshot = routingSnapshot();
  ASSERT_NE(nullptr, snapshot);
  EXPECT_NEAR(weightForIndex(PriorityRoutingWeights::SelectionSource::Healthy, 1), 1.0, 0.01);
}

TEST_F(LoadAwareLocalityLbTest, EwmaTopologyChangeResetsOnLocalityCountChange) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/0.3);

  setHostUtilization(h1, 0.5);
  setHostUtilization(h2, 0.5);
  recomputeWeights();

  auto h3 = makeWeightTrackingMockHost();
  setHostLocality(h3, 2);
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_ = {h1, h2, h3};
  host_set->healthy_hosts_ = {h1, h2, h3};
  host_set->hosts_per_locality_ =
      Upstream::makeHostsPerLocality({{h1}, {h2}, {h3}}, /*force_no_local_locality=*/true);
  host_set->healthy_hosts_per_locality_ = host_set->hosts_per_locality_;

  // Re-initialize to attach LocalityLbHostData to the new host, then set its utilization.
  recomputeWeights();
  setHostUtilization(h3, 0.8);
  recomputeWeights();

  const auto* snapshot = routingSnapshot();
  ASSERT_NE(nullptr, snapshot);
  ASSERT_EQ(3, snapshot->priority_weights[0].by_source[0].weights.size());
  EXPECT_NEAR(weightForIndex(PriorityRoutingWeights::SelectionSource::Healthy, 2), 0.2, 0.05);
}

TEST_F(LoadAwareLocalityLbTest, HealthyAndDegradedSourcesUseDifferentSnapshots) {
  auto healthy_host = makeWeightTrackingMockHost();
  auto degraded_host = makeWeightTrackingMockHost();

  setupLocalities({{healthy_host}, {degraded_host}},
                  /*has_local_locality=*/false,
                  std::vector<Upstream::HostVector>{{healthy_host}, {}},
                  std::vector<Upstream::HostVector>{{}, {degraded_host}});

  createLb();

  expectWeights(PriorityRoutingWeights::SelectionSource::Degraded, {0.0, 1.0});

  auto worker_lb = createWorkerLb();
  auto counts = countPicks(*worker_lb, 800);
  EXPECT_GT(counts[healthy_host.get()], 300);
  EXPECT_GT(counts[degraded_host.get()], 100);
}

TEST_F(LoadAwareLocalityLbTest, HealthTransitionRefreshesHealthyAndDegradedChildren) {
  auto healthy_host = makeWeightTrackingMockHost();
  auto remote_host = makeWeightTrackingMockHost();
  setupLocalities({{healthy_host}, {remote_host}},
                  /*has_local_locality=*/false,
                  std::vector<Upstream::HostVector>{{healthy_host}, {remote_host}},
                  std::vector<Upstream::HostVector>{{}, {}});

  createLb();
  auto worker_lb = createWorkerLb();
  EXPECT_TRUE(hostSeen(*worker_lb, healthy_host, 100));
  EXPECT_TRUE(hostSeen(*worker_lb, remote_host, 100));

  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->healthy_hosts_ = {healthy_host};
  host_set->healthy_hosts_per_locality_ =
      Upstream::makeHostsPerLocality(std::vector<Upstream::HostVector>{{healthy_host}, {}},
                                     /*force_no_local_locality=*/true);
  host_set->degraded_hosts_ = {remote_host};
  host_set->degraded_hosts_per_locality_ =
      Upstream::makeHostsPerLocality(std::vector<Upstream::HostVector>{{}, {remote_host}},
                                     /*force_no_local_locality=*/true);
  priority_set_.runUpdateCallbacks(0, {}, {});
  recomputeWeights();

  const auto* snapshot = routingSnapshot();
  ASSERT_NE(nullptr, snapshot);
  EXPECT_GT(weightForIndex(PriorityRoutingWeights::SelectionSource::Healthy, 0), 0.0);
  EXPECT_NEAR(weightForIndex(PriorityRoutingWeights::SelectionSource::Healthy, 1), 0.0, 0.01);
  EXPECT_NEAR(weightForIndex(PriorityRoutingWeights::SelectionSource::Degraded, 0), 0.0, 0.01);
  EXPECT_GT(weightForIndex(PriorityRoutingWeights::SelectionSource::Degraded, 1), 0.0);

  EXPECT_TRUE(hostSeen(*worker_lb, healthy_host, 200));
  EXPECT_TRUE(hostSeen(*worker_lb, remote_host, 400));
}

TEST_F(LoadAwareLocalityLbTest, PanicUsesAllHosts) {
  auto healthy_host = makeWeightTrackingMockHost();
  auto unhealthy_host_1 = makeWeightTrackingMockHost();
  auto unhealthy_host_2 = makeWeightTrackingMockHost();

  setupLocalities({{healthy_host}, {unhealthy_host_1, unhealthy_host_2}},
                  /*has_local_locality=*/false,
                  std::vector<Upstream::HostVector>{{healthy_host}, {}},
                  std::vector<Upstream::HostVector>{{}, {}});

  createLb();

  // Only 1 of 3 hosts healthy → live LoadBalancerBase panic on priority 0; all hosts selectable.
  expectWeights(PriorityRoutingWeights::SelectionSource::AllHosts, {1.0, 2.0});

  auto worker_lb = createWorkerLb();
  auto counts = countPicks(*worker_lb, 400);
  EXPECT_GT(counts[healthy_host.get()], 100);
  EXPECT_GT(counts[unhealthy_host_1.get()] + counts[unhealthy_host_2.get()], 200);
  EXPECT_GT(cluster_info_.lbStats().lb_healthy_panic_.value(), 0u);
}

TEST_F(LoadAwareLocalityLbTest, FailTrafficOnPanicReturnsNoHost) {
  cluster_info_.lb_config_.mutable_zone_aware_lb_config()->set_fail_traffic_on_panic(true);

  auto healthy_host = makeWeightTrackingMockHost();
  auto unhealthy_host_1 = makeWeightTrackingMockHost();
  auto unhealthy_host_2 = makeWeightTrackingMockHost();
  // Only 1 of 3 hosts healthy → live panic on priority 0.
  setupLocalities({{healthy_host}, {unhealthy_host_1, unhealthy_host_2}},
                  /*has_local_locality=*/false,
                  std::vector<Upstream::HostVector>{{healthy_host}, {}},
                  std::vector<Upstream::HostVector>{{}, {}});

  createLb();

  auto worker_lb = createWorkerLb();
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(nullptr, worker_lb->chooseHost(nullptr).host);
    EXPECT_EQ(nullptr, worker_lb->peekAnotherHost(nullptr));
  }
  EXPECT_GT(cluster_info_.lbStats().lb_healthy_panic_.value(), 0u);
}

TEST_F(LoadAwareLocalityLbTest, LiveSnapshotRefreshUsesLatestWeights) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb();
  auto worker_lb = createWorkerLb();
  EXPECT_TRUE(hostSeen(*worker_lb, h1, 100));
  EXPECT_TRUE(hostSeen(*worker_lb, h2, 100));

  publishHealthyWeights({0.0, 1.0});

  auto counts = countPicks(*worker_lb, 100);
  EXPECT_GE(counts[h2.get()], 98);
}

TEST_F(LoadAwareLocalityLbTest, MembershipAndTopologyUpdatesShareTheSameWorkerLb) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb();
  auto worker_lb = createWorkerLb();
  EXPECT_TRUE(hostSeen(*worker_lb, h1, 100));
  EXPECT_TRUE(hostSeen(*worker_lb, h2, 100));

  auto h3 = makeWeightTrackingMockHost();
  reshapeLocalities(0, {{h1}, {h2, h3}}, /*added=*/{h3});
  EXPECT_TRUE(hostSeen(*worker_lb, h3, 200));

  reshapeLocalities(0, {{h1}, {h3}}, /*added=*/{}, /*removed=*/{h2});
  EXPECT_FALSE(hostSeen(*worker_lb, h2, 200));
  EXPECT_TRUE(hostSeen(*worker_lb, h3, 200));

  auto h4 = makeWeightTrackingMockHost();
  reshapeLocalities(0, {{h1}, {h3}, {h4}}, /*added=*/{h4});

  publishHealthyWeights({1.0, 1.0, 1.0});
  EXPECT_TRUE(hostSeen(*worker_lb, h4, 300));
}

TEST_F(LoadAwareLocalityLbTest, StaleSnapshotFallbackAndAllLocalitiesRemovedAreGraceful) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  auto h3 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}, {h3}});

  createLb();
  setHostUtilization(h1, 0.5);
  setHostUtilization(h2, 0.5);
  setHostUtilization(h3, 0.5);
  recomputeWeights();

  auto worker_lb = createWorkerLb();
  EXPECT_TRUE(hostSeen(*worker_lb, h1, 200));
  EXPECT_TRUE(hostSeen(*worker_lb, h2, 200));
  EXPECT_TRUE(hostSeen(*worker_lb, h3, 200));

  reshapeLocalities(0, {{h1}, {}, {}}, /*added=*/{}, /*removed=*/{h2, h3});

  expectOnlyHost(*worker_lb, h1, 100);
  for (int i = 0; i < 50; ++i) {
    auto host = worker_lb->peekAnotherHost(nullptr);
    ASSERT_NE(nullptr, host);
    EXPECT_EQ(h1, host);
    worker_lb->chooseHost(nullptr);
  }

  reshapeLocalities(0, {{}, {}, {}}, /*added=*/{}, /*removed=*/{h1});

  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(nullptr, worker_lb->chooseHost(nullptr).host);
    EXPECT_EQ(nullptr, worker_lb->peekAnotherHost(nullptr));
  }
}

TEST_F(LoadAwareLocalityLbTest, EmptyDeltaUpdatesRefreshChildWeights) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1, h2}});

  createLb();
  auto worker_lb = createWorkerLb();

  auto initial_counts = countPicks(*worker_lb, 200);
  EXPECT_GT(initial_counts[h1.get()], 50);
  EXPECT_GT(initial_counts[h2.get()], 50);

  h1->weight(100);
  priority_set_.runUpdateCallbacks(0, {}, {});

  auto updated_counts = countPicks(*worker_lb, 400);
  EXPECT_GT(updated_counts[h1.get()], updated_counts[h2.get()] * 10);
  EXPECT_GT(updated_counts[h1.get()], 300);
}

TEST_F(LoadAwareLocalityLbTest, InPlaceUpdateTopologyMismatchWaitsForMembershipChange) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb();
  auto worker_lb = createWorkerLb();

  auto h3 = makeWeightTrackingMockHost();
  reshapeLocalities(0, {{h1}, {h2}, {h3}});
  EXPECT_TRUE(hostSeen(*worker_lb, h1, 100));
  EXPECT_TRUE(hostSeen(*worker_lb, h2, 100));
  EXPECT_FALSE(hostSeen(*worker_lb, h3, 100));

  priority_set_.runUpdateCallbacks(0, {h3}, {});
  publishHealthyWeights({1.0, 1.0, 1.0});
  EXPECT_TRUE(hostSeen(*worker_lb, h3, 300));
}

TEST_F(LoadAwareLocalityLbTest, PriorityFailoverAndFailback) {
  auto h_p0 = makeWeightTrackingMockHost();
  auto h_p1 = makeWeightTrackingMockHost();

  setupPriorityLocalities(0, {{h_p0}}, /*has_local_locality=*/false,
                          std::vector<Upstream::HostVector>{{}});
  setupPriorityLocalities(1, {{h_p1}});

  createLb();
  auto worker_lb = createWorkerLb();

  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(h_p1, worker_lb->chooseHost(nullptr).host);
  }

  reshapeLocalities(0, /*localities=*/{}, /*added=*/{}, /*removed=*/{}, /*has_local=*/false,
                    std::vector<Upstream::HostVector>{{h_p0}});
  recomputeWeights();

  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(h_p0, worker_lb->chooseHost(nullptr).host);
  }

  reshapeLocalities(0, /*localities=*/{}, /*added=*/{}, /*removed=*/{}, /*has_local=*/false,
                    std::vector<Upstream::HostVector>{{}});
  recomputeWeights();

  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(h_p1, worker_lb->chooseHost(nullptr).host);
  }
}

TEST_F(LoadAwareLocalityLbTest, FailoverIsCallbackFreshNotTimerGated) {
  auto h_p0 = makeWeightTrackingMockHost();
  auto h_p1 = makeWeightTrackingMockHost();

  setupPriorityLocalities(0, {{h_p0}});
  setupPriorityLocalities(1, {{h_p1}});

  createLb();
  auto worker_lb = createWorkerLb();

  // Priority 0 is healthy, so live failover keeps load on it.
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(h_p0, worker_lb->chooseHost(nullptr).host);
  }

  // Flip priority 0 to all-unhealthy (keep hosts_, clear healthy_hosts_) and fire the membership
  // update callback ONLY. The weight-update timer is NOT invoked and simulated time is NOT
  // advanced past weight_update_period, so no new routing-weight snapshot is published.
  reshapeLocalities(0, /*localities=*/{}, /*added=*/{}, /*removed=*/{}, /*has_local=*/false,
                    std::vector<Upstream::HostVector>{{}});

  // Failover to priority 1 happens immediately from the live LoadBalancerBase callback, proving it
  // is not gated on a timer-published weight snapshot.
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(h_p1, worker_lb->chooseHost(nullptr).host);
  }
}

TEST_F(LoadAwareLocalityLbTest, PriorityUpdateGuardPaths) {
  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h1}});

  createLb();
  auto worker_lb = createWorkerLb();
  EXPECT_TRUE(hostSeen(*worker_lb, h1, 50));

  // Add a healthy priority 1. Priority 0 is still healthy, so live failover keeps load there.
  auto h_p1 = makeWeightTrackingMockHost();
  setupPriorityLocalities(1, {{h_p1}});
  priority_set_.runUpdateCallbacks(1, {h_p1}, {});
  PriorityRoutingWeights p0;
  p0.by_source[0].weights = makeWeightsMap({1.0});
  PriorityRoutingWeights p1;
  p1.by_source[0].weights = makeWeightsMap({1.0});
  publishSnapshot(makeSnapshot({p0, p1}));
  expectOnlyHost(*worker_lb, h1, 50);

  // Drop priority 0 to zero availability; live failover shifts load to priority 1.
  reshapeLocalities(0, /*localities=*/{}, /*added=*/{}, /*removed=*/{}, /*has_local=*/false,
                    std::vector<Upstream::HostVector>{{}});
  EXPECT_TRUE(hostSeen(*worker_lb, h_p1, 100));

  // Growing the backing priority set without a matching snapshot is handled gracefully.
  priority_set_.getMockHostSet(5);
  priority_set_.runUpdateCallbacks(5, {makeWeightTrackingMockHost()}, {});
  priority_set_.runUpdateCallbacks(5, {}, {});
  EXPECT_TRUE(hostSeen(*worker_lb, h_p1, 100));
}

TEST_F(LoadAwareLocalityLbTest, AdvisorySnapshotSizeMismatchesAreHandledGracefully) {
  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h1}});

  createLb();
  auto worker_lb = createWorkerLb();

  PriorityRoutingWeights weights;
  weights.by_source[0].weights = makeWeightsMap({1.0});

  // The advisory snapshot may carry more priorities than the live priority set; the single live
  // healthy host stays selected (priority is live, not snapshot-driven).
  publishSnapshot(makeSnapshot({weights, weights}));
  EXPECT_TRUE(hostSeen(*worker_lb, h1, 20));

  publishSnapshot(makeSnapshot({weights}));
  EXPECT_TRUE(hostSeen(*worker_lb, h1, 20));
  EXPECT_EQ(h1, worker_lb->peekAnotherHost(nullptr));
}

TEST_F(LoadAwareLocalityLbTest, ShortAdvisorySnapshotStillRoutesLivePriority) {
  auto h_p1 = makeWeightTrackingMockHost();
  setupPriorityLocalities(0, {});
  setupPriorityLocalities(1, {{h_p1}});

  createLb();
  auto worker_lb = createWorkerLb();

  // Priority 0 has no hosts; live failover routes to the healthy priority 1 even though the
  // advisory snapshot only carries weights for priority 0.
  publishHealthyWeights({1.0});

  expectOnlyHost(*worker_lb, h_p1, 20);
  EXPECT_EQ(h_p1, worker_lb->peekAnotherHost(nullptr));
}

TEST_F(LoadAwareLocalityLbTest, EmptyHigherPriorityDoesNotBreakLiveRouting) {
  auto h_p0 = makeWeightTrackingMockHost();
  setupPriorityLocalities(0, {{h_p0}});
  setupPriorityLocalities(1, {});

  createLb();
  auto worker_lb = createWorkerLb();

  // Priority 1 has no hosts; live failover keeps load on the healthy priority 0.
  PriorityRoutingWeights p0;
  p0.by_source[0].weights = makeWeightsMap({1.0});
  PriorityRoutingWeights p1;
  publishSnapshot(makeSnapshot({p0, p1}));

  expectOnlyHost(*worker_lb, h_p0, 20);
}

TEST_F(LoadAwareLocalityLbTest, SelectLocalityFallsBackForCountMismatchAndZeroTotals) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb();
  auto worker_lb = createWorkerLb();

  // Snapshot has only locality 0's identity; the worker's locality 1 (missing → 0.0) is excluded,
  // so all traffic stays on h1.
  publishHealthyWeights({1.0});
  expectOnlyHost(*worker_lb, h1, 20);

  publishHealthyWeights({});
  expectOnlyHost(*worker_lb, h1, 20);

  // Both live localities have weight 0 (locality 2's 5.0 is not a live identity), so selection
  // falls back to locality 0.
  publishHealthyWeights({0.0, 0.0, 5.0});
  expectOnlyHost(*worker_lb, h1, 20);
}

TEST_F(LoadAwareLocalityLbTest, SelectLocalityIgnoresWeightsForUnknownLocalities) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  createLb();
  auto worker_lb = createWorkerLb();

  // The snapshot carries a third locality identity (weight 100.0) that the worker does not have.
  // It is ignored; the two live localities split traffic by their identity-matched weights (1:1).
  publishHealthyWeights({1.0, 1.0, 100.0});

  auto counts = countPicks(*worker_lb, 400);
  EXPECT_GT(counts[h1.get()], 120);
  EXPECT_GT(counts[h2.get()], 120);
}

TEST_F(LoadAwareLocalityLbTest, SelectLocalityRoundingGuardReturnsLastLocality) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  auto h3 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}, {h3}});

  createLb();
  auto worker_lb = createWorkerLb();

  publishHealthyWeights({0.2, 0.3, 0.5});

  // The hash draw (chooseHostSet) consumes the first random value even with a single priority,
  // so the second draw must drive locality selection.
  forceRandomValues({0, std::numeric_limits<uint64_t>::max()});
  EXPECT_EQ(h3, worker_lb->chooseHost(nullptr).host);
}

// Advisory weights are keyed by Locality identity, so adding a locality that shifts another's
// lexicographic index between the snapshot tick and the worker's live membership must NOT mis-map
// the weight. Set up local A + remote C with distinct weights, publish a snapshot, then add remote
// B (sorts between A and C, shifting C from index 1 to index 2) WITHOUT recomputing weights, and
// assert C's traffic still follows C's weight (and the new B, absent from the snapshot, gets none).
TEST_F(LoadAwareLocalityLbTest, AdvisoryWeightsFollowLocalityIdentityAcrossIndexShift) {
  const auto locality_a = Upstream::Locality("a", "z", "sz"); // local, index 0
  const auto locality_b = Upstream::Locality("b", "z", "sz"); // sorts between A and C
  const auto locality_c =
      Upstream::Locality("c", "z", "sz"); // index 1, shifts to 2 when B is added

  auto host_a = makeWeightTrackingMockHost();
  auto host_c = makeWeightTrackingMockHost();
  setHostLocalityExplicit(host_a, locality_a);
  setHostLocalityExplicit(host_c, locality_c);

  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_ = {host_a, host_c};
  host_set->healthy_hosts_ = {host_a, host_c};
  host_set->hosts_per_locality_ = Upstream::makeHostsPerLocality({{host_a}, {host_c}});
  host_set->healthy_hosts_per_locality_ = host_set->hosts_per_locality_;

  createLb();
  auto worker_lb = createWorkerLb();

  // Snapshot computed for the {A, C} ordering: A gets almost no weight, C gets all of it.
  PriorityRoutingWeights weights;
  weights.by_source[0].weights[locality_a] = 0.0;
  weights.by_source[0].weights[locality_c] = 1.0;
  publishSnapshot(makeSnapshot({weights}));
  expectOnlyHost(*worker_lb, host_c, 50);

  // Add remote B, whose identity sorts between A and C, so C's index shifts from 1 to 2. The
  // snapshot is NOT recomputed: it still only knows A and C by identity.
  auto host_b = makeWeightTrackingMockHost();
  setHostLocalityExplicit(host_b, locality_b);
  host_set->hosts_ = {host_a, host_b, host_c};
  host_set->healthy_hosts_ = {host_a, host_b, host_c};
  host_set->hosts_per_locality_ = Upstream::makeHostsPerLocality({{host_a}, {host_b}, {host_c}});
  host_set->healthy_hosts_per_locality_ = host_set->hosts_per_locality_;
  priority_set_.runUpdateCallbacks(0, {host_b}, {});

  // Identity mapping survives the index shift: C still carries all the weight (its weight was NOT
  // applied to B at the now-stale index 1), and B — absent from the snapshot — receives nothing.
  expectOnlyHost(*worker_lb, host_c, 50);
  EXPECT_FALSE(hostSeen(*worker_lb, host_b, 200));
  EXPECT_FALSE(hostSeen(*worker_lb, host_a, 200));
}

TEST_F(LoadAwareLocalityLbTest, DedicatedStatsAllOverloadedTotalIncOncePerTickWhenAllZeroHeadroom) {
  auto h_a = makeHosts(2);
  auto h_b = makeHosts(1);
  setupLocalities({h_a, h_b});

  createLb();
  EXPECT_EQ(0, counterValue(cluster_info_, "load_aware_locality.all_overloaded_total"));

  // Make all localities fully saturated — headroom = 0.
  setUtilizationForHosts(h_a, 1.0);
  setUtilizationForHosts(h_b, 1.0);
  recomputeWeights();
  EXPECT_EQ(1, counterValue(cluster_info_, "load_aware_locality.all_overloaded_total"));

  // Next tick, still saturated — counter goes to 2 (one per tick, not per source).
  expectCounterIncrementsPerTick("load_aware_locality.all_overloaded_total", 1);
}

TEST_F(LoadAwareLocalityLbTest, DedicatedStatsLocalPreferredTotalIncOncePerTickOnVarianceSnap) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);

  // High threshold so local is NOT preferred when both are saturated (utils=1.0).
  createLb(/*variance_threshold=*/0.0, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0);
  // Cold start: no data yet, both utils=0 → utils[0]=0 <= 0+0.0=0 → snap fires.
  const uint64_t after_create =
      counterValue(cluster_info_, "load_aware_locality.local_preferred_total");
  EXPECT_GE(after_create, 0u); // ≥0 — snap may fire at cold start

  // Force high local utilization so snap does NOT fire.
  setHostUtilization(h_local, 0.9);
  setHostUtilization(h_remote, 0.1);
  recomputeWeights();
  // 0.9 > 0.1 + 0.0 → no snap.
  const uint64_t after_no_snap =
      counterValue(cluster_info_, "load_aware_locality.local_preferred_total");
  EXPECT_EQ(after_create, after_no_snap);

  // Now snap fires (local <= remote + threshold).
  setHostUtilization(h_local, 0.1);
  setHostUtilization(h_remote, 0.9);
  recomputeWeights();
  EXPECT_EQ(after_no_snap + 1,
            counterValue(cluster_info_, "load_aware_locality.local_preferred_total"));

  // Another snap tick.
  expectCounterIncrementsPerTick("load_aware_locality.local_preferred_total", 1);
}

TEST_F(LoadAwareLocalityLbTest, DedicatedStatsProbeActiveTotalIncOncePerTickWhenProbeKicksIn) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);

  // remote_probe_fraction=0.05 → probe redistribution fires when remote < 5% of total.
  // variance_threshold=1.0 so local is always preferred.
  createLb(/*variance_threshold=*/1.0, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.05);
  // Cold start tick: utils=0, local preferred, probe fires on the initial tick.
  const uint64_t after_create =
      counterValue(cluster_info_, "load_aware_locality.probe_active_total");
  EXPECT_GE(after_create, 0u);

  setHostUtilization(h_local, 0.3);
  setHostUtilization(h_remote, 1.0);
  recomputeWeights();
  // all_local fires (local preferred), then probe redistribution shifts weight to remote.
  EXPECT_EQ(after_create + 1,
            counterValue(cluster_info_, "load_aware_locality.probe_active_total"));

  expectCounterIncrementsPerTick("load_aware_locality.probe_active_total", 1);
}

TEST_F(LoadAwareLocalityLbTest, DedicatedStatsStaleLocalityTotalCountsOncePerStalePerTick) {
  auto locality_a = makeHosts(2);
  auto locality_b = makeHosts(1);
  auto locality_c = makeHosts(3);
  setupLocalities({locality_a, locality_b, locality_c});

  createLb(/*variance_threshold=*/0.1, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0,
           std::chrono::milliseconds(5000));

  // Give all localities fresh data and re-initialize so we have a clean baseline.
  setUtilizationForHosts(locality_a, 0.5);
  setUtilizationForHosts(locality_b, 0.4);
  setUtilizationForHosts(locality_c, 0.6);
  recomputeWeights();
  // Record counter after the tick where all data is fresh (should add 0).
  const uint64_t baseline = counterValue(cluster_info_, "load_aware_locality.stale_locality_total");

  // Age locality_b and locality_c past expiration; locality_a stays fresh.
  context_.time_system_.advanceTimeWait(std::chrono::milliseconds(6000));
  setUtilizationForHosts(locality_a, 0.5);
  recomputeWeights();
  // 2 stale localities (b and c) — counter adds 2 per tick, NOT 6 (not 3× for 3 sources).
  EXPECT_EQ(baseline + 2, counterValue(cluster_info_, "load_aware_locality.stale_locality_total"));

  // Next tick, same state → adds another 2.
  expectCounterIncrementsPerTick("load_aware_locality.stale_locality_total", 2);
}

TEST_F(LoadAwareLocalityLbTest, ZoneRoutingStatsCoverHealthyRoutingPaths) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);

  createLb(/*variance_threshold=*/1.0, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0);
  setHostUtilization(h_local, 0.3);
  setHostUtilization(h_remote, 0.3);
  recomputeWeights();

  auto worker_lb = createWorkerLb();
  expectOnlyHost(*worker_lb, h_local, 10);
  EXPECT_EQ(10, static_cast<int>(cluster_info_.lbStats().lb_zone_routing_all_directly_.value()));

  setupLocalities({{h_local}, {h_remote}}, /*has_local_locality=*/true);
  createLb(/*variance_threshold=*/0.0, /*ewma_alpha=*/1.0, /*remote_probe_fraction=*/0.0);
  setHostUtilization(h_local, 0.31);
  setHostUtilization(h_remote, 0.3);
  recomputeWeights();

  worker_lb = createWorkerLb();
  for (int i = 0; i < 200; ++i) {
    worker_lb->chooseHost(nullptr);
  }
  EXPECT_GT(cluster_info_.lbStats().lb_zone_routing_sampled_.value(), 0);
  EXPECT_GT(cluster_info_.lbStats().lb_zone_routing_cross_zone_.value(), 0);
}

TEST_F(LoadAwareLocalityLbTest, ZoneRoutingStatsCoverDegradedAndPanicSources) {
  auto h_local = makeWeightTrackingMockHost();
  auto h_remote = makeWeightTrackingMockHost();
  // No healthy hosts, all degraded (but enough to avoid panic): live priority targets the
  // Degraded source.
  setupLocalities({{h_local}, {h_remote}},
                  /*has_local_locality=*/true, std::vector<Upstream::HostVector>{{}, {}},
                  std::vector<Upstream::HostVector>{{h_local}, {h_remote}});

  createLb();
  recomputeWeights();

  auto worker_lb = createWorkerLb();
  auto degraded_counts = countPicks(*worker_lb, 400);
  EXPECT_GT(degraded_counts[h_local.get()] + degraded_counts[h_remote.get()], 0);

  auto h_remote_2 = makeWeightTrackingMockHost();
  setupLocalities({{h_local}, {h_remote, h_remote_2}},
                  /*has_local_locality=*/true, std::vector<Upstream::HostVector>{{h_local}, {}},
                  std::vector<Upstream::HostVector>{{}, {}});

  createLb();
  recomputeWeights();
  worker_lb = createWorkerLb();
  auto panic_counts = countPicks(*worker_lb, 400);
  EXPECT_GT(panic_counts[h_local.get()] + panic_counts[h_remote.get()] +
                panic_counts[h_remote_2.get()],
            0);
  EXPECT_GT(cluster_info_.lbStats().lb_zone_routing_sampled_.value() +
                cluster_info_.lbStats().lb_zone_routing_cross_zone_.value() +
                cluster_info_.lbStats().lb_zone_routing_all_directly_.value(),
            0u);
}

TEST_F(LoadAwareLocalityLbTest, ZoneRoutingStatsStayZeroWithoutLocalLocality) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}}, /*has_local_locality=*/false);

  createLb();
  setHostUtilization(h1, 0.3);
  setHostUtilization(h2, 0.3);
  recomputeWeights();

  auto worker_lb = createWorkerLb();
  for (int i = 0; i < 100; ++i) {
    worker_lb->chooseHost(nullptr);
  }

  EXPECT_EQ(0, cluster_info_.lbStats().lb_zone_routing_all_directly_.value());
  EXPECT_EQ(0, cluster_info_.lbStats().lb_zone_routing_cross_zone_.value());
  EXPECT_EQ(0, cluster_info_.lbStats().lb_zone_routing_sampled_.value());
}

TEST_F(LoadAwareLocalityLbTest, EmptyLocalityHostsDoNotCrash) {
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_.clear();
  host_set->healthy_hosts_.clear();
  host_set->hosts_per_locality_ =
      Upstream::makeHostsPerLocality({}, /*force_no_local_locality=*/true);
  host_set->healthy_hosts_per_locality_ = host_set->hosts_per_locality_;

  createLb();
  auto worker_lb = createWorkerLb();
  EXPECT_EQ(nullptr, worker_lb->chooseHost(nullptr).host);
}

TEST_F(LoadAwareLocalityLbTest, WorkerFallsBackWithoutPublishedRoutingSnapshot) {
  auto h1 = makeWeightTrackingMockHost();
  auto h2 = makeWeightTrackingMockHost();
  setupLocalities({{h1}, {h2}});

  auto factory = createRoundRobinWorkerFactory();
  auto worker_lb = factory->create({priority_set_, nullptr});
  ASSERT_NE(nullptr, worker_lb);

  expectOnlyHost(*worker_lb, h1, 20);
  EXPECT_EQ(h1, worker_lb->peekAnotherHost(nullptr));
}

TEST_F(LoadAwareLocalityLbTest, InitializePropagatesChildFactoryError) {
  NiceMock<Upstream::MockTypedLoadBalancerFactory> null_child_factory;
  ON_CALL(null_child_factory, name()).WillByDefault(Return("mock_null_factory"));
  ON_CALL(null_child_factory,
          create(testing::_, testing::_, testing::_, testing::_, testing::_, testing::_))
      .WillByDefault(testing::Return(testing::ByMove(nullptr)));

  auto lb_config = std::make_unique<LoadAwareLocalityLbConfig>(
      null_child_factory, "mock_null_factory", nullptr, std::chrono::milliseconds(1000), 0.1, 0.3,
      0.03, std::chrono::milliseconds(180000), std::vector<std::string>{}, dispatcher_,
      context_.thread_local_);

  LoadAwareLocalityLoadBalancer lb(*lb_config, cluster_info_, priority_set_,
                                   context_.runtime_loader_, random_, context_.time_system_);
  auto status = lb.initialize();
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(status.message(), testing::HasSubstr("mock_null_factory"));
}

TEST_F(LoadAwareLocalityLbTest, ChildRecreatedWhenUnderlyingFactoryRequiresIt) {
  auto local = makeWeightTrackingMockHost();
  auto remote = makeWeightTrackingMockHost();
  setupLocalities({{local}, {remote}});

  auto child_factory = std::make_shared<RecordingWorkerChildFactory>(true);
  auto factory = makeWorkerFactoryWithChild(child_factory, "mock_recreate_child");

  auto worker_lb = factory->create({priority_set_, nullptr});
  ASSERT_NE(nullptr, worker_lb);
  const uint32_t initial_create_count = child_factory->createCount();
  EXPECT_GT(initial_create_count, 0);

  auto extra = makeWeightTrackingMockHost();
  reshapeLocalities(0, {{local}, {remote, extra}}, /*added=*/{extra});
  EXPECT_GT(child_factory->createCount(), initial_create_count);
}

TEST_F(LoadAwareLocalityLbTest, MetricNamesDriveUtilization) {
  // LocalityLbHostData with metric_names={"named_metrics.foo"} delegates to
  // OrcaLoadReportHandler::getUtilizationFromOrcaReport, which follows the runtime-feature
  // controlled precedence: named_metrics → application_utilization → cpu_utilization
  // (when the default orca_weight_manager_use_named_metrics_first flag is enabled).
  Event::GlobalTimeSystem ts;

  {
    // Named metric present → use its value.
    LocalityLbHostData slot(ts, {"named_metrics.foo"});
    xds::data::orca::v3::OrcaLoadReport report;
    (*report.mutable_named_metrics())["foo"] = 0.6;
    ASSERT_TRUE(slot.onOrcaLoadReport(report, stream_info_).ok());
    EXPECT_DOUBLE_EQ(0.6, slot.utilization());
  }

  {
    // Named metric absent and no application_utilization → cpu_utilization fallback.
    LocalityLbHostData slot(ts, {"named_metrics.foo"});
    xds::data::orca::v3::OrcaLoadReport report;
    report.set_cpu_utilization(0.3);
    ASSERT_TRUE(slot.onOrcaLoadReport(report, stream_info_).ok());
    EXPECT_DOUBLE_EQ(0.3, slot.utilization());
  }

  {
    // Empty metric_names → application_utilization used when set.
    LocalityLbHostData slot(ts, {});
    xds::data::orca::v3::OrcaLoadReport report;
    report.set_application_utilization(0.7);
    ASSERT_TRUE(slot.onOrcaLoadReport(report, stream_info_).ok());
    EXPECT_DOUBLE_EQ(0.7, slot.utilization());
  }

  {
    // Empty metric_names and no application_utilization → cpu_utilization fallback.
    LocalityLbHostData slot(ts, {});
    xds::data::orca::v3::OrcaLoadReport report;
    report.set_cpu_utilization(0.4);
    ASSERT_TRUE(slot.onOrcaLoadReport(report, stream_info_).ok());
    EXPECT_DOUBLE_EQ(0.4, slot.utilization());
  }
}

// Tests 1+2: chooseHost and peekAnotherHost empty-priority-set guard.
// The guards at the top of each function return early when hostSetsPerPriority() is empty.
// We construct the LB against a real priority set (so LoadBalancerBase initialises safely), then
// override the mock to present an empty set for subsequent calls.
TEST_F(LoadAwareLocalityLbTest, EmptyPrioritySetGuardInChooseHostAndPeekAnotherHost) {
  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h1}});

  createLb();
  auto worker_lb = createWorkerLb();

  // Override so that hostSetsPerPriority() returns empty from this point on.
  static const std::vector<Upstream::HostSetPtr> kEmptyHostSets;
  ON_CALL(testing::Const(priority_set_), hostSetsPerPriority())
      .WillByDefault(testing::ReturnRef(kEmptyHostSets));

  // Both guards fire: `if (priority_set_.hostSetsPerPriority().empty()) return {nullptr/nullptr}`.
  EXPECT_EQ(nullptr, worker_lb->chooseHost(nullptr).host);
  EXPECT_EQ(nullptr, worker_lb->peekAnotherHost(nullptr));
}

// Test 3: empty-delta (no-rebuild) priority-count-mismatch early-return.
// When the priority count in the live set grows but the worker hasn't rebuilt yet, an empty-delta
// callback runs syncPriority with allow_rebuild=false. The size mismatch guard returns early
// without crashing.
TEST_F(LoadAwareLocalityLbTest, InPlaceHostUpdateSizeMismatchReturnsEarly) {
  auto h1 = makeWeightTrackingMockHost();
  setupLocalities({{h1}});

  createLb();
  auto worker_lb = createWorkerLb();
  expectOnlyHost(*worker_lb, h1, 10);

  // Grow the backing priority set to 6 priorities without a membership-delta rebuild first.
  priority_set_.getMockHostSet(5);
  // Empty-delta update on priority 5: syncPriority(5, allow_rebuild=false).
  // host_sets.size()=6, per_priority_locality_.size()=1 → size-mismatch guard returns early.
  priority_set_.runUpdateCallbacks(5, {}, {});

  // The LB is still functional for priority 0 (early-return did not corrupt state).
  expectOnlyHost(*worker_lb, h1, 10);
}

// Test 4: storeUtilization non-finite guard.
// When getUtilizationFromOrcaReport returns a non-finite value (Inf/NaN), storeUtilization
// discards it: utilization() stays 0.0 and lastUpdateTimeMs() stays 0.
TEST_F(LoadAwareLocalityLbTest, StoreUtilizationRejectsNonFiniteValue) {
  Event::GlobalTimeSystem ts;
  LocalityLbHostData slot(ts, {});

  // cpu_utilization is the last-resort fallback when no metric names are configured and
  // application_utilization is 0. Setting it to Inf produces a non-finite util value.
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(std::numeric_limits<double>::infinity());
  ASSERT_TRUE(slot.onOrcaLoadReport(report, stream_info_).ok());

  // Non-finite value rejected: slot remains at its initial state.
  EXPECT_DOUBLE_EQ(0.0, slot.utilization());
  EXPECT_EQ(0, slot.lastUpdateTimeMs());
}

} // namespace
} // namespace LoadAwareLocality
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
