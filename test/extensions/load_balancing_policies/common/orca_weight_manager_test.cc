#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

#include "envoy/grpc/status.h"

#include "source/common/network/utility.h"
#include "source/common/stats/isolated_store_impl.h"
#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace Common {
namespace {

using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

// ============================================================
// OrcaLoadReportHandler static method tests
// ============================================================

TEST(OrcaLoadReportHandlerTest, GetUtilizationFromOrcaReport_ApplicationUtilization) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_application_utilization(0.5);
  report.set_cpu_utilization(0.6);
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}),
            0.5);
}

TEST(OrcaLoadReportHandlerTest, GetUtilizationFromOrcaReport_NamedMetrics) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.mutable_named_metrics()->insert({"foo", 0.3});
  report.set_cpu_utilization(0.6);
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}),
            0.3);
}

TEST(OrcaLoadReportHandlerTest, GetUtilizationFromOrcaReport_Preference) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_application_utilization(0.5);
  report.mutable_named_metrics()->insert({"foo", 0.3});
  report.set_cpu_utilization(0.6);

  // By default (flag enabled), named metrics win.
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}),
            0.3);

  // With flag disabled, application utilization wins.
  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues(
      {{"envoy.reloadable_features.orca_weight_manager_use_named_metrics_first", "false"}});
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}),
            0.5);
}

TEST(OrcaLoadReportHandlerTest,
     GetUtilizationFromOrcaReport_Preference_FlagDisabled_FallbackToNamedMetrics) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.mutable_named_metrics()->insert({"foo", 0.3});
  report.set_cpu_utilization(0.6);

  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues(
      {{"envoy.reloadable_features.orca_weight_manager_use_named_metrics_first", "false"}});

  // application_utilization is not set (0), so it falls back to named metrics.
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}),
            0.3);
}

TEST(OrcaLoadReportHandlerTest,
     GetUtilizationFromOrcaReport_Preference_FlagDisabled_FallbackToCpu) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.6);

  TestScopedRuntime scoped_runtime;
  scoped_runtime.mergeValues(
      {{"envoy.reloadable_features.orca_weight_manager_use_named_metrics_first", "false"}});

  // application_utilization and named metrics are not set, so it falls back to cpu.
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}),
            0.6);
}

TEST(OrcaLoadReportHandlerTest, GetUtilizationFromOrcaReport_CpuUtilizationFallback) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.mutable_named_metrics()->insert({"bar", 0.3});
  report.set_cpu_utilization(0.6);
  // "named_metrics.foo" doesn't match "bar", so falls through to cpu_utilization.
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}),
            0.6);
}

TEST(OrcaLoadReportHandlerTest, GetUtilizationFromOrcaReport_NoUtilization) {
  xds::data::orca::v3::OrcaLoadReport report;
  EXPECT_EQ(OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"}), 0);
}

TEST(OrcaLoadReportHandlerTest, CalculateWeightFromOrcaReport_Valid) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_application_utilization(0.5);
  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 0.0);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 2000);
}

TEST(OrcaLoadReportHandlerTest, CalculateWeightFromOrcaReport_NoQps) {
  xds::data::orca::v3::OrcaLoadReport report;
  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 0.0);
  EXPECT_EQ(result.status(), absl::InvalidArgumentError("QPS must be positive"));
}

TEST(OrcaLoadReportHandlerTest, CalculateWeightFromOrcaReport_NoUtilization) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 0.0);
  EXPECT_EQ(result.status(), absl::InvalidArgumentError("Utilization must be positive"));
}

TEST(OrcaLoadReportHandlerTest, CalculateWeightFromOrcaReport_MaxWeight) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(10000000000000L);
  report.set_application_utilization(0.0000001);
  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 0.0);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), std::numeric_limits<uint32_t>::max());
}

TEST(OrcaLoadReportHandlerTest, CalculateWeightFromOrcaReport_ErrorPenalty) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_eps(100);
  report.set_application_utilization(0.5);
  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 2.0);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value(), 1428);
}

// ============================================================
// OrcaHostLbPolicyData tests
// ============================================================

TEST(OrcaHostLbPolicyDataTest, GetWeightIfValid_Blackout) {
  OrcaHostLbPolicyData data(nullptr, 42,
                            /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
                            /*last_update_time=*/MonotonicTime(std::chrono::seconds(10)));
  // max_non_empty_since (2) < non_empty_since (5) → blackout period, return nullopt.
  EXPECT_FALSE(data.getWeightIfValid(MonotonicTime(std::chrono::seconds(2)),
                                     MonotonicTime(std::chrono::seconds(1))));
  // non_empty_since_ should not be reset during blackout.
  EXPECT_EQ(data.non_empty_since_.load(), MonotonicTime(std::chrono::seconds(5)));
}

TEST(OrcaHostLbPolicyDataTest, GetWeightIfValid_Expiration) {
  OrcaHostLbPolicyData data(nullptr, 42,
                            /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
                            /*last_update_time=*/MonotonicTime(std::chrono::seconds(7)));
  // last_update_time (7) < min_last_update_time (8) → expired, return nullopt.
  EXPECT_FALSE(data.getWeightIfValid(MonotonicTime(std::chrono::seconds(2)),
                                     MonotonicTime(std::chrono::seconds(8))));
  // Expiration resets non_empty_since_ to default.
  EXPECT_EQ(data.non_empty_since_.load(), OrcaHostLbPolicyData::kDefaultNonEmptySince);
}

TEST(OrcaHostLbPolicyDataTest, GetWeightIfValid_Valid) {
  OrcaHostLbPolicyData data(nullptr, 42,
                            /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
                            /*last_update_time=*/MonotonicTime(std::chrono::seconds(10)));
  // non_empty_since (1) <= max_non_empty_since (2) and
  // last_update_time (10) >= min_last_update_time (8) → valid.
  auto result = data.getWeightIfValid(MonotonicTime(std::chrono::seconds(2)),
                                      MonotonicTime(std::chrono::seconds(8)));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 42);
}

TEST(OrcaHostLbPolicyDataTest, GetWeightIfValid_ExactBlackoutBoundary) {
  OrcaHostLbPolicyData data(nullptr, 42,
                            /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
                            /*last_update_time=*/MonotonicTime(std::chrono::seconds(10)));
  // max_non_empty_since == non_empty_since → NOT blackout, weight is valid.
  auto result = data.getWeightIfValid(MonotonicTime(std::chrono::seconds(5)),
                                      MonotonicTime(std::chrono::seconds(1)));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 42);
}

TEST(OrcaHostLbPolicyDataTest, GetWeightIfValid_ExactExpirationBoundary) {
  OrcaHostLbPolicyData data(nullptr, 42,
                            /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
                            /*last_update_time=*/MonotonicTime(std::chrono::seconds(10)));
  // last_update_time == min_last_update_time → NOT expired, weight is valid.
  auto result = data.getWeightIfValid(MonotonicTime(std::chrono::seconds(2)),
                                      MonotonicTime(std::chrono::seconds(10)));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 42);
}

TEST(OrcaHostLbPolicyDataTest, UpdateWeightNow_FirstUpdate) {
  OrcaHostLbPolicyData data(nullptr);
  EXPECT_EQ(data.non_empty_since_.load(), OrcaHostLbPolicyData::kDefaultNonEmptySince);
  EXPECT_EQ(data.weight_.load(), 1);

  MonotonicTime now(std::chrono::seconds(30));
  data.updateWeightNow(2000, now);
  EXPECT_EQ(data.weight_.load(), 2000);
  EXPECT_EQ(data.last_update_time_.load(), now);
  // First update sets non_empty_since_.
  EXPECT_EQ(data.non_empty_since_.load(), now);
}

TEST(OrcaHostLbPolicyDataTest, UpdateWeightNow_SubsequentUpdate) {
  MonotonicTime first_time(std::chrono::seconds(10));
  OrcaHostLbPolicyData data(nullptr, 100, first_time, first_time);

  MonotonicTime second_time(std::chrono::seconds(20));
  data.updateWeightNow(200, second_time);
  EXPECT_EQ(data.weight_.load(), 200);
  EXPECT_EQ(data.last_update_time_.load(), second_time);
  // non_empty_since_ should not be changed on subsequent update.
  EXPECT_EQ(data.non_empty_since_.load(), first_time);
}

TEST(OrcaHostLbPolicyDataTest, OnOrcaLoadReport_Success) {
  OrcaWeightManagerConfig config;
  config.metric_names_for_computing_utilization = {"named_metrics.foo"};
  config.error_utilization_penalty = 0.0;
  config.blackout_period = std::chrono::milliseconds(10000);
  config.weight_expiration_period = std::chrono::milliseconds(180000);
  config.weight_update_period = std::chrono::milliseconds(1000);

  Event::SimulatedTimeSystem time_system;
  time_system.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));

  auto handler = std::make_shared<OrcaLoadReportHandler>(config, time_system);
  OrcaHostLbPolicyData data(handler);

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_application_utilization(0.5);

  Envoy::StreamInfo::MockStreamInfo mock_stream_info;
  EXPECT_EQ(data.onOrcaLoadReport(report, mock_stream_info), absl::OkStatus());
  EXPECT_EQ(data.weight_.load(), 2000);
  EXPECT_EQ(data.non_empty_since_.load(), MonotonicTime(std::chrono::seconds(30)));
  EXPECT_EQ(data.last_update_time_.load(), MonotonicTime(std::chrono::seconds(30)));
}

TEST(OrcaHostLbPolicyDataTest, OnOrcaLoadReport_ErrorPreservesState) {
  OrcaWeightManagerConfig config;
  config.metric_names_for_computing_utilization = {"named_metrics.foo"};
  config.error_utilization_penalty = 0.0;
  config.blackout_period = std::chrono::milliseconds(10000);
  config.weight_expiration_period = std::chrono::milliseconds(180000);
  config.weight_update_period = std::chrono::milliseconds(1000);

  Event::SimulatedTimeSystem time_system;
  time_system.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));

  auto handler = std::make_shared<OrcaLoadReportHandler>(config, time_system);
  OrcaHostLbPolicyData data(handler, 42,
                            /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
                            /*last_update_time=*/MonotonicTime(std::chrono::seconds(10)));

  xds::data::orca::v3::OrcaLoadReport report;
  // QPS is 0 → invalid report.
  report.set_rps_fractional(0);
  report.set_application_utilization(0.5);

  Envoy::StreamInfo::MockStreamInfo mock_stream_info;
  EXPECT_EQ(data.onOrcaLoadReport(report, mock_stream_info),
            absl::InvalidArgumentError("QPS must be positive"));
  // State should be preserved.
  EXPECT_EQ(data.weight_.load(), 42);
  EXPECT_EQ(data.non_empty_since_.load(), MonotonicTime(std::chrono::seconds(1)));
  EXPECT_EQ(data.last_update_time_.load(), MonotonicTime(std::chrono::seconds(10)));
}

// ============================================================
// OrcaWeightManager tests
// ============================================================

// Helper to create a MockHost that tracks weight and lb policy data state
// (since MOCK_METHOD doesn't store). By default the host's cluster advertises
// the HTTP/2 feature bit (required for OOB reporting) and the host has a
// stable loopback address (safe to log).
std::shared_ptr<NiceMock<Upstream::MockHost>>
makeWeightTrackingMockHost(uint32_t initial_weight = 1,
                           uint64_t cluster_features = Upstream::ClusterInfo::Features::HTTP2) {
  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  auto weight = std::make_shared<uint32_t>(initial_weight);
  ON_CALL(*host, weight()).WillByDefault([weight]() -> uint32_t { return *weight; });
  ON_CALL(*host, weight(::testing::_)).WillByDefault([weight](uint32_t new_weight) {
    *weight = new_weight;
  });
  ON_CALL(host->cluster_, features()).WillByDefault(Return(cluster_features));
  auto address = *Network::Utility::resolveUrl("tcp://10.0.0.1:443");
  ON_CALL(*host, address()).WillByDefault(Return(address));
  return host;
}

// Mock OOB session for testing weight manager OOB lifecycle.
// Uses shared_ptr<bool> for stopped/started flags so they remain accessible
// after the session is destroyed (owned by the weight manager).
class MockOrcaOobSession : public Orca::OrcaOobSession {
public:
  MockOrcaOobSession(Upstream::HostSharedPtr host, Event::Dispatcher& dispatcher,
                     Random::RandomGenerator& random, std::chrono::milliseconds reporting_period,
                     Orca::OrcaOobCallbacks& callbacks, Orca::OrcaOobStats& stats,
                     std::shared_ptr<bool> started, std::shared_ptr<bool> stopped)
      : OrcaOobSession(std::move(host), dispatcher, random, reporting_period, callbacks, stats),
        started_(std::move(started)), stopped_(std::move(stopped)) {}
  // Override start to avoid real connection creation.
  void start() override { *started_ = true; }
  // Call the base stop() so the real teardown path runs. client_,
  // request_encoder_, and reconnect_timer_ are nullptr in the mock path
  // (base stop() guards each), but exercising the full lifecycle keeps the
  // mock robust against future additions to OrcaOobSession.
  void stop() override {
    *stopped_ = true;
    Orca::OrcaOobSession::stop();
  }
  // Override createCodecClient to satisfy the base class (never called since start is overridden).
  Http::CodecClientPtr createCodecClient(Upstream::Host::CreateConnectionData&) override {
    return nullptr;
  }
  std::shared_ptr<bool> started_;
  std::shared_ptr<bool> stopped_;
};

} // namespace

// Test subclass that overrides createOobSession to inject mock sessions.
// Declared outside the anonymous namespace so that the `friend class
// TestOrcaWeightManager;` declaration in OrcaWeightManager (which names a
// class in this enclosing namespace) matches this type.
class TestOrcaWeightManager : public OrcaWeightManager {
public:
  TestOrcaWeightManager(const OrcaWeightManagerConfig& config,
                        const Upstream::PrioritySet& priority_set, TimeSource& time_source,
                        Event::Dispatcher& dispatcher, WeightsUpdatedCb on_weights_updated,
                        Random::RandomGenerator& random, Stats::Scope& stats_scope)
      : OrcaWeightManager(config, priority_set, time_source, dispatcher, on_weights_updated, random,
                          stats_scope) {}

  Orca::OrcaOobSessionPtr createOobSession(const Upstream::HostSharedPtr& host,
                                           Orca::OrcaOobCallbacks& callbacks,
                                           Orca::OrcaOobStats& stats) override {
    auto started = std::make_shared<bool>(false);
    auto stopped = std::make_shared<bool>(false);
    auto session = std::make_unique<MockOrcaOobSession>(
        host, dispatcher_, random_, oob_reporting_period_, callbacks, stats, started, stopped);
    last_session_started_ = started;
    last_session_stopped_ = stopped;
    last_created_callback_ = &callbacks;
    sessions_created_++;
    return session;
  }

  // Test-only accessor: reaches into the private oob_sessions_ map via the
  // friend declaration in OrcaWeightManager so tests can assert lifecycle
  // invariants (e.g. EDS flap and mid-flight removal) without exposing the
  // map on the production surface.
  std::size_t oobSessionCountForTest() const { return oob_sessions_.size(); }

  std::shared_ptr<bool> last_session_started_;
  std::shared_ptr<bool> last_session_stopped_;
  Orca::OrcaOobCallbacks* last_created_callback_{nullptr};
  int sessions_created_{0};
};

namespace {

class OrcaWeightManagerTest : public testing::Test {
protected:
  void SetUp() override {
    config_.metric_names_for_computing_utilization = {"named_metrics.foo"};
    config_.error_utilization_penalty = 0.1;
    config_.blackout_period = std::chrono::milliseconds(10000);
    config_.weight_expiration_period = std::chrono::milliseconds(180000);
    config_.weight_update_period = std::chrono::milliseconds(1000);
  }

  std::unique_ptr<OrcaWeightManager> createManager() {
    return std::make_unique<OrcaWeightManager>(
        config_, priority_set_, time_system_, dispatcher_, [this]() { weights_updated_ = true; },
        random_, *stats_store_.rootScope());
  }

  std::unique_ptr<TestOrcaWeightManager> createTestManager() {
    return std::make_unique<TestOrcaWeightManager>(
        config_, priority_set_, time_system_, dispatcher_, [this]() { weights_updated_ = true; },
        random_, *stats_store_.rootScope());
  }

  // Helper to set up an OOB-enabled TestOrcaWeightManager with the given hosts initialized.
  std::unique_ptr<TestOrcaWeightManager> initializeOobManager(const Upstream::HostVector& hosts) {
    config_.enable_oob_load_report = true;
    // Pre-allocate reconnect timers (one per host, registered before the weight timer
    // because MockTimer expectations are matched in LIFO order).
    for (size_t i = 0; i < hosts.size(); i++) {
      new NiceMock<Event::MockTimer>(&dispatcher_);
    }
    auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
    auto manager = createTestManager();
    auto* host_set = priority_set_.getMockHostSet(0);
    host_set->hosts_ = hosts;
    EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
    auto status = manager->initialize();
    EXPECT_TRUE(status.ok());
    return manager;
  }

  OrcaWeightManagerConfig config_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  Event::SimulatedTimeSystem time_system_;
  NiceMock<Random::MockRandomGenerator> random_;
  Stats::IsolatedStoreImpl stats_store_;
  bool weights_updated_ = false;
};

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_AllValid) {
  auto manager = createManager();

  Upstream::HostVector hosts;
  for (int i = 0; i < 3; ++i) {
    auto host = makeWeightTrackingMockHost();
    host->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
        manager->reportHandler(), 40 + i,
        /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
        /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
    hosts.push_back(host);
  }

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  EXPECT_TRUE(updated);
  EXPECT_EQ(hosts[0]->weight(), 40);
  EXPECT_EQ(hosts[1]->weight(), 41);
  EXPECT_EQ(hosts[2]->weight(), 42);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_Mixed) {
  auto manager = createManager();

  Upstream::HostVector hosts;
  // First host has valid weight.
  auto h1 = makeWeightTrackingMockHost();
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 42,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
  hosts.push_back(h1);

  // Other hosts have no data → default weight.
  for (int i = 0; i < 2; ++i) {
    hosts.push_back(makeWeightTrackingMockHost());
  }

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  EXPECT_TRUE(updated);
  EXPECT_EQ(hosts[0]->weight(), 42);
  // Default is median of [42] = 42.
  EXPECT_EQ(hosts[1]->weight(), 42);
  EXPECT_EQ(hosts[2]->weight(), 42);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_AllDefault) {
  auto manager = createManager();

  Upstream::HostVector hosts;
  for (int i = 0; i < 2; ++i) {
    hosts.push_back(makeWeightTrackingMockHost());
  }

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  // Default weight is 1, same as initial → no update.
  EXPECT_FALSE(updated);
  EXPECT_EQ(hosts[0]->weight(), 1);
  EXPECT_EQ(hosts[1]->weight(), 1);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_EvenMedian) {
  auto manager = createManager();

  Upstream::HostVector hosts;
  auto h1 = makeWeightTrackingMockHost();
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 5,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
  hosts.push_back(h1);

  auto h2 = makeWeightTrackingMockHost();
  h2->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 42,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
  hosts.push_back(h2);

  // Third host has no data.
  hosts.push_back(makeWeightTrackingMockHost());

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  EXPECT_TRUE(updated);
  EXPECT_EQ(hosts[0]->weight(), 5);
  EXPECT_EQ(hosts[1]->weight(), 42);
  // Even median of [5, 42] = (5+42)/2 = 23.
  EXPECT_EQ(hosts[2]->weight(), 23);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_EmptyHosts) {
  auto manager = createManager();
  Upstream::HostVector empty_hosts;
  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(empty_hosts);
  EXPECT_FALSE(updated);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_SingleHostValid) {
  auto manager = createManager();

  auto h1 = makeWeightTrackingMockHost();
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 99,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
  Upstream::HostVector hosts = {h1};

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  EXPECT_TRUE(updated);
  EXPECT_EQ(hosts[0]->weight(), 99);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_SingleHostDefault) {
  auto manager = createManager();

  // Single host with no data — default weight stays at 1.
  Upstream::HostVector hosts = {makeWeightTrackingMockHost()};

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  EXPECT_FALSE(updated);
  EXPECT_EQ(hosts[0]->weight(), 1);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_WeightUnchangedNoCallback) {
  auto manager = createManager();

  // Host with valid weight that equals current weight — no update.
  auto h1 = makeWeightTrackingMockHost(/*initial_weight=*/42);
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 42,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
  Upstream::HostVector hosts = {h1};

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  EXPECT_FALSE(updated);
  EXPECT_EQ(hosts[0]->weight(), 42);
}

TEST_F(OrcaWeightManagerTest, GetWeightIfValidFromHost_NoData) {
  NiceMock<Upstream::MockHost> host;
  EXPECT_FALSE(OrcaWeightManager::getWeightIfValidFromHost(host, MonotonicTime::min(),
                                                           MonotonicTime::max()));
}

TEST_F(OrcaWeightManagerTest, GetWeightIfValidFromHost_Valid) {
  NiceMock<Upstream::MockHost> host;
  host.addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      nullptr, 42,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
  auto result = OrcaWeightManager::getWeightIfValidFromHost(
      host, MonotonicTime(std::chrono::seconds(2)), MonotonicTime(std::chrono::seconds(8)));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 42);
}

// ============================================================
// OrcaWeightManager lifecycle tests (initialize, timer, callbacks)
// ============================================================

TEST_F(OrcaWeightManagerTest, Initialize_AttachesHostDataToExistingHosts) {
  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;
  for (int i = 0; i < 3; ++i) {
    hosts.push_back(makeWeightTrackingMockHost());
  }
  host_set->hosts_ = hosts;

  // Verify no host has LB data before initialize.
  for (const auto& host : hosts) {
    EXPECT_FALSE(host->typedLbPolicyData<OrcaHostLbPolicyData>().has_value());
  }

  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());

  // Verify all hosts now have LB data attached.
  for (const auto& host : hosts) {
    EXPECT_TRUE(host->typedLbPolicyData<OrcaHostLbPolicyData>().has_value());
    auto typed = host->typedLbPolicyData<OrcaHostLbPolicyData>();
    EXPECT_TRUE(typed.has_value());
  }
}

TEST_F(OrcaWeightManagerTest, Initialize_StartsWeightCalculationTimer) {
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());
}

TEST_F(OrcaWeightManagerTest, Initialize_PriorityUpdateCallbackAttachesDataToNewHosts) {
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());

  // Add new hosts via priority update callback.
  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector new_hosts;
  for (int i = 0; i < 2; ++i) {
    new_hosts.push_back(makeWeightTrackingMockHost());
  }
  host_set->hosts_ = new_hosts;

  // Trigger priority update callback with new hosts.
  host_set->runCallbacks(new_hosts, {});

  // Verify new hosts have LB data attached.
  for (const auto& host : new_hosts) {
    EXPECT_TRUE(host->typedLbPolicyData<OrcaHostLbPolicyData>().has_value());
  }
}

TEST_F(OrcaWeightManagerTest, TimerCallback_UpdatesWeightsAndReenablesTimer) {
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());

  // Set up hosts with valid weights so the callback fires on change.
  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;
  auto h1 = makeWeightTrackingMockHost();
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 100,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(50))));
  hosts.push_back(h1);
  host_set->hosts_ = hosts;

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));

  // Timer callback should: update weights, then re-enable timer.
  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  timer->invokeCallback();

  EXPECT_TRUE(weights_updated_);
  EXPECT_EQ(hosts[0]->weight(), 100);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnMainThread_CallbackFiredOnChange) {
  new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  // Set up hosts with valid weights that differ from current weight.
  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;
  auto h1 = makeWeightTrackingMockHost(/*initial_weight=*/1);
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 200,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(50))));
  hosts.push_back(h1);
  host_set->hosts_ = hosts;

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));

  EXPECT_FALSE(weights_updated_);
  manager->updateWeightsOnMainThread();
  EXPECT_TRUE(weights_updated_);
  EXPECT_EQ(hosts[0]->weight(), 200);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnMainThread_NoCallbackWhenNoChange) {
  new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  // Hosts with no data — default weight is 1, same as initial.
  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;
  hosts.push_back(makeWeightTrackingMockHost(/*initial_weight=*/1));
  host_set->hosts_ = hosts;

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));

  manager->updateWeightsOnMainThread();
  // Default weight (1) equals initial weight (1), so no change → no callback.
  EXPECT_FALSE(weights_updated_);
}

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnMainThread_MultiplePriorities) {
  new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  // Priority 0: host with valid weight.
  auto* host_set0 = priority_set_.getMockHostSet(0);
  auto h0 = makeWeightTrackingMockHost(/*initial_weight=*/1);
  h0->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 50,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(50))));
  host_set0->hosts_ = {h0};

  // Priority 1: host with valid weight.
  auto* host_set1 = priority_set_.getMockHostSet(1);
  auto h1 = makeWeightTrackingMockHost(/*initial_weight=*/1);
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 75,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(50))));
  host_set1->hosts_ = {h1};

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));

  manager->updateWeightsOnMainThread();
  EXPECT_TRUE(weights_updated_);
  EXPECT_EQ(h0->weight(), 50);
  EXPECT_EQ(h1->weight(), 75);
}

TEST_F(OrcaWeightManagerTest, AddLbPolicyDataToHosts_SkipsHostsWithExistingData) {
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;

  // Host with existing data.
  auto h1 = makeWeightTrackingMockHost();
  h1->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 42,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
  hosts.push_back(h1);

  // Host without data.
  auto h2 = makeWeightTrackingMockHost();
  hosts.push_back(h2);

  host_set->hosts_ = hosts;

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());

  // h1's existing data should be preserved (weight=42).
  auto typed_h1 = h1->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed_h1.has_value());
  EXPECT_EQ(typed_h1->weight_.load(), 42);

  // h2 should now have fresh data (default weight=1).
  auto typed_h2 = h2->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed_h2.has_value());
  EXPECT_EQ(typed_h2->weight_.load(), 1);
}

TEST_F(OrcaWeightManagerTest, OddMedian) {
  new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  Upstream::HostVector hosts;
  // 3 hosts with valid weights: 10, 20, 30 → median = 20.
  for (uint32_t w : {10u, 20u, 30u}) {
    auto h = makeWeightTrackingMockHost();
    h->addLbPolicyData(std::make_unique<OrcaHostLbPolicyData>(
        manager->reportHandler(), w,
        /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
        /*last_update_time=*/MonotonicTime(std::chrono::seconds(10))));
    hosts.push_back(h);
  }
  // 1 host with no data → gets median default.
  hosts.push_back(makeWeightTrackingMockHost());

  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(30)));
  bool updated = manager->updateWeightsOnHosts(hosts);
  EXPECT_TRUE(updated);
  EXPECT_EQ(hosts[0]->weight(), 10);
  EXPECT_EQ(hosts[1]->weight(), 20);
  EXPECT_EQ(hosts[2]->weight(), 30);
  EXPECT_EQ(hosts[3]->weight(), 20); // Odd median of [10, 20, 30].
}

// ============================================================
// OOB configuration tests
// ============================================================

TEST_F(OrcaWeightManagerTest, RejectsZeroOobReportingPeriod) {
  config_.enable_oob_load_report = true;
  config_.oob_reporting_period = std::chrono::milliseconds(0);
  // Construction succeeds; validation happens in initialize() and is surfaced as a status.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();
  auto status = manager->initialize();
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), absl::StatusCode::kInvalidArgument);
  EXPECT_THAT(std::string(status.message()),
              testing::HasSubstr("oob_reporting_period must be >= 1ms"));
}

TEST_F(OrcaWeightManagerTest, AcceptsOneMsOobReportingPeriod) {
  config_.enable_oob_load_report = true;
  config_.oob_reporting_period = std::chrono::milliseconds(1);
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();
  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  EXPECT_TRUE(manager->initialize().ok());
}

TEST_F(OrcaWeightManagerTest, OobDisabled_NoOobConfiguredOnHostData) {
  config_.enable_oob_load_report = false;
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;
  hosts.push_back(makeWeightTrackingMockHost());
  host_set->hosts_ = hosts;

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());

  // Host should have lb policy data but oob_configured_ should be false.
  auto typed = hosts[0]->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed.has_value());
  EXPECT_FALSE(typed->oobReportingConfigured());
}

TEST_F(OrcaWeightManagerTest, OobEnabled_SetsOobConfiguredOnHostData) {
  // Initialize with OOB enabled but no hosts — this avoids session creation.
  config_.enable_oob_load_report = true;
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());

  // Manually add lb policy data as would happen in addLbPolicyDataToHosts.
  // Verify OrcaHostLbPolicyData is constructed with oob_configured_ = true.
  auto data = std::make_unique<OrcaHostLbPolicyData>(manager->reportHandler(),
                                                     /*oob_configured=*/true);
  EXPECT_TRUE(data->oobReportingConfigured());
  EXPECT_TRUE(data->receivesOrcaLoadReport());
}

TEST_F(OrcaWeightManagerTest, OobEnabled_DestructionIsClean) {
  // Verify destructor doesn't crash when OOB is enabled but no sessions exist.
  config_.enable_oob_load_report = true;
  new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();
  manager.reset(); // Should not crash.
}

TEST_F(OrcaWeightManagerTest, OobDisabled_HostRemovalHandled) {
  config_.enable_oob_load_report = false;
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createManager();

  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;
  auto h1 = makeWeightTrackingMockHost();
  hosts.push_back(h1);
  host_set->hosts_ = hosts;

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());

  // Remove the host via priority callback — should not crash.
  Upstream::HostVector removed = {h1};
  host_set->hosts_.clear();
  host_set->runCallbacks({}, removed);

  manager.reset();
}

// ============================================================
// OOB session management tests (using TestOrcaWeightManager)
// ============================================================

TEST_F(OrcaWeightManagerTest, OobEnabled_CreatesSessionsForHosts) {
  Upstream::HostVector hosts = {makeWeightTrackingMockHost(), makeWeightTrackingMockHost()};
  auto manager = initializeOobManager(hosts);

  EXPECT_EQ(2, manager->sessions_created_);
  EXPECT_TRUE(*manager->last_session_started_);

  for (const auto& host : hosts) {
    auto typed = host->typedLbPolicyData<OrcaHostLbPolicyData>();
    ASSERT_TRUE(typed.has_value());
    EXPECT_TRUE(typed->oobReportingConfigured());
  }
}

TEST_F(OrcaWeightManagerTest, Http2HostCreatesOobSession) {
  // A host whose cluster advertises HTTP/2 should get an OOB session and
  // the non_h2_host_skipped counter should remain zero.
  auto h1 = makeWeightTrackingMockHost(/*initial_weight=*/1,
                                       /*cluster_features=*/Upstream::ClusterInfo::Features::HTTP2);
  auto manager = initializeOobManager({h1});

  EXPECT_EQ(1, manager->sessions_created_);
  EXPECT_TRUE(*manager->last_session_started_);
  EXPECT_EQ(0, stats_store_.counterFromString("orca_oob.non_h2_host_skipped").value());
}

TEST_F(OrcaWeightManagerTest, NonHttp2HostSkippedForOob) {
  // A host whose cluster does NOT advertise HTTP/2 should be skipped: no
  // session created, non_h2_host_skipped counter incremented, no exception.
  auto h_bad = makeWeightTrackingMockHost(/*initial_weight=*/1,
                                          /*cluster_features=*/0);
  auto h_good = makeWeightTrackingMockHost(/*initial_weight=*/1,
                                           /*cluster_features=*/
                                           Upstream::ClusterInfo::Features::HTTP2);
  // Only one reconnect timer is needed (for the HTTP/2 host). The non-H2
  // host is skipped before session creation, so it never creates a timer.
  config_.enable_oob_load_report = true;
  new NiceMock<Event::MockTimer>(&dispatcher_); // reconnect timer for h_good
  auto* weight_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = createTestManager();
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_ = {h_bad, h_good};
  EXPECT_CALL(*weight_timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  // Only the HTTP/2 host got a session.
  EXPECT_EQ(1, manager->sessions_created_);
  EXPECT_EQ(1, stats_store_.counterFromString("orca_oob.non_h2_host_skipped").value());

  // The skipped host still gets LB policy data attached — it's only the
  // OOB session that's gated.
  auto typed_bad = h_bad->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed_bad.has_value());
  EXPECT_TRUE(typed_bad->oobReportingConfigured());
}

TEST_F(OrcaWeightManagerTest, OobEnabled_StopSessionsOnHostRemoval) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});
  EXPECT_EQ(1, manager->sessions_created_);

  auto session_started = manager->last_session_started_;
  auto session_stopped = manager->last_session_stopped_;
  EXPECT_TRUE(*session_started);
  EXPECT_FALSE(*session_stopped);

  // Remove the host via priority callback.
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_.clear();
  host_set->runCallbacks({}, {h1});

  EXPECT_TRUE(*session_stopped);
}

// Regression test for re-entry UAF: OrcaOobSession::stop() calls
// client_->close(NoFlush) which can re-enter via connection close callbacks.
// The session must be handed off to dispatcher_.deferredDelete so that its
// destruction is deferred past any in-flight callbacks rather than destroyed
// synchronously during the map erase.
TEST_F(OrcaWeightManagerTest, StopOobSessionsUsesDeferredDelete) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});
  EXPECT_EQ(1, manager->sessions_created_);

  // Expect deferredDelete to be invoked exactly once when the host is removed.
  EXPECT_CALL(dispatcher_, deferredDelete_(::testing::_));

  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_.clear();
  host_set->runCallbacks({}, {h1});
}

TEST_F(OrcaWeightManagerTest, OobEnabled_DestructorStopsSessions) {
  auto manager = initializeOobManager({makeWeightTrackingMockHost()});

  auto session_stopped = manager->last_session_stopped_;
  EXPECT_FALSE(*session_stopped);

  manager.reset();
  EXPECT_TRUE(*session_stopped);
}

TEST_F(OrcaWeightManagerTest, OobEnabled_DuplicateHostSkipsSessionCreation) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});
  EXPECT_EQ(1, manager->sessions_created_);

  // Re-add the same host — should not create a second session.
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->runCallbacks({h1}, {});
  EXPECT_EQ(1, manager->sessions_created_);
}

TEST_F(OrcaWeightManagerTest, OobEnabled_PriorityUpdateCreatesSessionForNewHost) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});
  EXPECT_EQ(1, manager->sessions_created_);

  // Add a new host via priority callback.
  auto h2 = makeWeightTrackingMockHost();
  new NiceMock<Event::MockTimer>(&dispatcher_); // reconnect timer for new session
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_.push_back(h2);
  host_set->runCallbacks({h2}, {});

  EXPECT_EQ(2, manager->sessions_created_);
  EXPECT_TRUE(*manager->last_session_started_);
  auto typed = h2->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed.has_value());
  EXPECT_TRUE(typed->oobReportingConfigured());
}

TEST_F(OrcaWeightManagerTest, OobEnabled_RemoveUnknownHostNoOp) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});

  auto session_stopped = manager->last_session_stopped_;
  EXPECT_FALSE(*session_stopped);

  // Remove a host that was never in the manager — should not crash or stop existing sessions.
  auto h2 = makeWeightTrackingMockHost();
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->runCallbacks({}, {h2});

  EXPECT_FALSE(*session_stopped);
}

TEST_F(OrcaWeightManagerTest, OobCallbackAdapter_OnOrcaOobReport) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});

  auto typed = h1->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed.has_value());

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_application_utilization(0.5);
  manager->last_created_callback_->onOrcaOobReport(report);

  EXPECT_EQ(typed->weight_.load(), 2000);
}

TEST_F(OrcaWeightManagerTest, OobCallbackAdapter_OnOrcaOobReportMissingHostData) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});

  // Override mock to simulate host with no LB policy data.
  ON_CALL(*h1, lbPolicyDataCount()).WillByDefault(Return(0));
  EXPECT_FALSE(h1->typedLbPolicyData<OrcaHostLbPolicyData>().has_value());

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_application_utilization(0.5);
  // Should not crash — the callback silently skips when host data is absent.
  manager->last_created_callback_->onOrcaOobReport(report);
}

TEST_F(OrcaWeightManagerTest, OobCallbackAdapter_OnOrcaOobStreamFailure) {
  auto manager = initializeOobManager({makeWeightTrackingMockHost()});

  // Should not crash — just logs.
  manager->last_created_callback_->onOrcaOobStreamFailure(
      Grpc::Status::WellKnownGrpcStatus::Unavailable);
}

TEST_F(OrcaWeightManagerTest, OobCallbackAdapter_ReportWithInvalidQps) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});

  auto typed = h1->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed.has_value());

  // Send a report with invalid QPS (0) — handler should return error, weight unchanged.
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(0);
  report.set_application_utilization(0.5);
  manager->last_created_callback_->onOrcaOobReport(report);

  EXPECT_EQ(typed->weight_.load(), 1);
}

// Regression test for the host-removed-mid-flight race: a synthetic OOB
// report arrives on the main dispatcher for a host that is immediately
// removed via stopOobSessions. Thanks to D2's deferredDelete, the session
// lifetime extends past any queued callback — this test verifies that
// invariant by (1) delivering a report to the callback adapter, (2)
// removing the host, and (3) asserting deferredDelete was invoked and the
// map entry is gone.
TEST_F(OrcaWeightManagerTest, HostRemovedMidFlightDoesNotUAF) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});
  ASSERT_EQ(1u, manager->oobSessionCountForTest());
  auto typed = h1->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(typed.has_value());

  // Synthesize an in-flight OOB report arriving for h1 — this exercises the
  // callback path that could race with removal.
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_application_utilization(0.5);
  manager->last_created_callback_->onOrcaOobReport(report);
  EXPECT_EQ(typed->weight_.load(), 2000);

  // Immediately remove the host. The session must be enqueued for deferred
  // delete (not destroyed synchronously), so that any queued callback that
  // still holds a pointer into the session sees a valid object until the
  // dispatcher next runs.
  EXPECT_CALL(dispatcher_, deferredDelete_(::testing::_));
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_.clear();
  host_set->runCallbacks({}, {h1});

  // After removal the session entry is gone from the map; the session
  // object itself lives on the deferred-delete list until the dispatcher
  // next drains it (outside this test's scope).
  EXPECT_EQ(0u, manager->oobSessionCountForTest());
}

// Regression test for rapid EDS churn: the same host is added and removed
// across multiple priority updates. After each add the session map should
// contain exactly one entry; after each remove it should be empty. This
// catches resource leaks (sessions not deregistered) and stale map entries
// (duplicate registration).
TEST_F(OrcaWeightManagerTest, EdsHostFlapRegistersAndDeregistersSessions) {
  auto h1 = makeWeightTrackingMockHost();
  auto manager = initializeOobManager({h1});
  EXPECT_EQ(1u, manager->oobSessionCountForTest());
  EXPECT_EQ(1, manager->sessions_created_);

  auto* host_set = priority_set_.getMockHostSet(0);

  // Flap 5 remove/add cycles. Each remove invokes deferredDelete; the
  // manager destructor at end-of-test invokes it once more for the session
  // still registered after the last re-add, hence kFlapCycles + 1 total.
  constexpr int kFlapCycles = 5;
  EXPECT_CALL(dispatcher_, deferredDelete_(::testing::_)).Times(kFlapCycles + 1);
  for (int i = 0; i < kFlapCycles; ++i) {
    // Remove.
    host_set->hosts_.clear();
    host_set->runCallbacks({}, {h1});
    EXPECT_EQ(0u, manager->oobSessionCountForTest()) << "after remove cycle " << i;

    // Re-add. Each add creates a new reconnect timer for the new session.
    new NiceMock<Event::MockTimer>(&dispatcher_);
    host_set->hosts_ = {h1};
    host_set->runCallbacks({h1}, {});
    EXPECT_EQ(1u, manager->oobSessionCountForTest()) << "after add cycle " << i;
  }

  // One initial session + kFlapCycles new sessions from each re-add.
  EXPECT_EQ(1 + kFlapCycles, manager->sessions_created_);
}

} // namespace
} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
