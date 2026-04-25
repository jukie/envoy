#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "envoy/grpc/status.h"

#include "source/common/buffer/buffer_impl.h"
#include "source/common/grpc/common.h"
#include "source/common/http/codec_client.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/orca/orca_oob_session.h"
#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"

#include "test/common/http/common.h"
#include "test/common/stats/stat_test_utility.h"
#include "test/common/upstream/utility.h"
#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/http/stream_encoder.h"
#include "test/mocks/network/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "absl/strings/str_cat.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace Common {
namespace {

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
// (since MOCK_METHOD doesn't store).
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

// Test factory: records each create() call and returns a null codec client.
// OrcaOobSession treats a null codec client as a transient failure (logged and
// retried via backoff), which keeps the test isolated from real socket /
// connection plumbing.
class FakeOrcaOobCodecClientFactory : public OrcaOobCodecClientFactory {
public:
  Http::CodecClientPtr create(
      Upstream::Host::CreateConnectionData&& /*connection_data*/, Event::Dispatcher& /*dispatcher*/,
      Random::RandomGenerator& /*random*/,
      Network::TransportSocketOptionsConstSharedPtr /*transport_socket_options*/) const override {
    ++create_calls_;
    return nullptr;
  }

  mutable uint32_t create_calls_{0};
};

class OrcaWeightManagerTest : public testing::Test {
protected:
  void SetUp() override {
    config_.metric_names_for_computing_utilization = {"named_metrics.foo"};
    config_.error_utilization_penalty = 0.1;
    config_.blackout_period = std::chrono::milliseconds(10000);
    config_.weight_expiration_period = std::chrono::milliseconds(180000);
    config_.weight_update_period = std::chrono::milliseconds(1000);
  }

  // Helper to build the manager with the test's configured dependencies. By
  // default no factory is injected because OOB is disabled; pass a non-null
  // factory when enabling OOB.
  std::unique_ptr<OrcaWeightManager> makeManager(OrcaOobCodecClientFactoryPtr factory = nullptr) {
    return std::make_unique<OrcaWeightManager>(
        config_, priority_set_, time_system_, dispatcher_, random_, *stats_store_.rootScope(),
        /*transport_socket_options=*/nullptr, std::move(factory),
        [this]() { weights_updated_ = true; });
  }

  OrcaWeightManagerConfig config_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Random::MockRandomGenerator> random_;
  Stats::TestUtil::TestStore stats_store_;
  Event::SimulatedTimeSystem time_system_;
  bool weights_updated_ = false;
};

TEST_F(OrcaWeightManagerTest, UpdateWeightsOnHosts_AllValid) {
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

  EXPECT_CALL(*timer, enableTimer(config_.weight_update_period, nullptr));
  auto status = manager->initialize();
  EXPECT_TRUE(status.ok());
}

TEST_F(OrcaWeightManagerTest, Initialize_PriorityUpdateCallbackAttachesDataToNewHosts) {
  auto* timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
  auto manager = makeManager();

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
// OrcaWeightManager OOB integration tests
// ============================================================

class OrcaWeightManagerOobTest : public OrcaWeightManagerTest {
protected:
  void SetUp() override {
    OrcaWeightManagerTest::SetUp();
    config_.oob_enabled = true;
    config_.oob_reporting_period = std::chrono::milliseconds(10000);
    config_.oob_request_cost_names = {"cost.foo"};
  }

  // Build a manager with OOB enabled and a tracked fake codec client factory.
  // The factory pointer remains valid because the manager owns it via
  // OrcaOobCodecClientFactoryPtr.
  std::unique_ptr<OrcaWeightManager> makeOobManager() {
    auto factory = std::make_unique<FakeOrcaOobCodecClientFactory>();
    factory_ = factory.get();
    return makeManager(std::move(factory));
  }

  FakeOrcaOobCodecClientFactory* factory_{nullptr};
};

// OOB disabled (the default OrcaWeightManagerTest config): we should never
// schedule a stagger timer or invoke the codec client factory, even when hosts
// arrive via the priority update.
TEST_F(OrcaWeightManagerTest, OobDisabled_NoSessionsOrTimers) {
  // Weight calculation timer.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  auto manager = makeManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector new_hosts;
  for (int i = 0; i < 2; ++i) {
    new_hosts.push_back(makeWeightTrackingMockHost());
  }
  host_set->hosts_ = new_hosts;
  host_set->runCallbacks(new_hosts, {});

  // No active sessions, no stream events, factory never called.
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());
  EXPECT_EQ(0U, manager->oobStatsForTest().stream_opens_.value());
}

// NOTE: MockTimer's ctor sets up an EXPECT_CALL(...).WillOnce(Return(this))
// expectation. gmock matches expectations LIFO, so the LAST allocated MockTimer
// satisfies the FIRST createTimer() call. Tests below allocate timers in the
// REVERSE order they will be consumed by the production code:
//   1. session goaway-drain timer (allocated first, consumed last)
//   2. session retry timer
//   3. stagger timer
//   4. weight calculation timer (allocated last, consumed first by ctor)

// OOB enabled + host added: a stagger timer is scheduled, but the codec
// factory is NOT yet invoked. The factory only runs once the timer fires.
TEST_F(OrcaWeightManagerOobTest, HostAdded_SchedulesPendingTimer_NoImmediateStart) {
  // Stagger timer (consumed second).
  auto* stagger_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first by manager ctor).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeOobManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  // Random returns 0 by default → stagger delay should be 0ms (still scheduled
  // through the timer, not invoked synchronously).
  EXPECT_CALL(*stagger_timer, enableTimer(testing::_, testing::_));

  auto* host_set = priority_set_.getMockHostSet(0);
  auto host = makeWeightTrackingMockHost();
  host_set->hosts_ = {host};
  host_set->runCallbacks({host}, {});

  // Factory has NOT been called yet — the stagger timer needs to fire first.
  EXPECT_EQ(0U, factory_->create_calls_);
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());
}

// Stagger timer fires → session is created and the factory is invoked at
// least once.
TEST_F(OrcaWeightManagerOobTest, StaggerTimerFires_FactoryInvokedAndSessionActive) {
  // OrcaOobSession internally allocates a goaway drain timer (consumed last).
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // OrcaOobSession internally allocates a retry timer.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Stagger timer.
  auto* stagger_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeOobManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto* host_set = priority_set_.getMockHostSet(0);
  auto host = makeWeightTrackingMockHost();
  host_set->hosts_ = {host};
  host_set->runCallbacks({host}, {});

  // Fire the stagger timer — this calls startOobSession() which constructs
  // an OrcaOobSession and invokes the codec factory.
  stagger_timer->invokeCallback();

  EXPECT_GE(factory_->create_calls_, 1U);
  EXPECT_EQ(1, manager->oobStatsForTest().active_sessions_.value());
}

// Host removed BEFORE the stagger timer fires: the timer is cancelled and the
// factory is never invoked.
TEST_F(OrcaWeightManagerOobTest, HostRemovedWhilePending_TimerCancelled) {
  // Stagger timer (consumed second).
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeOobManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto* host_set = priority_set_.getMockHostSet(0);
  auto host = makeWeightTrackingMockHost();
  host_set->hosts_ = {host};
  host_set->runCallbacks({host}, {});
  EXPECT_EQ(0U, factory_->create_calls_);

  // Now remove the host before the stagger timer fires.
  host_set->hosts_.clear();
  host_set->runCallbacks({}, {host});

  // Factory must NOT have been called.
  EXPECT_EQ(0U, factory_->create_calls_);
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());
}

// Host removed AFTER the stagger timer fires (i.e. session active): the
// session is closed and deferred-deleted, and the active-sessions gauge is
// decremented.
TEST_F(OrcaWeightManagerOobTest, HostRemovedWhileActive_SessionClosedAndGaugeDecremented) {
  // Session goaway drain + retry timers (consumed last and second-last).
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Stagger timer.
  auto* stagger_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeOobManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto* host_set = priority_set_.getMockHostSet(0);
  auto host = makeWeightTrackingMockHost();
  host_set->hosts_ = {host};
  host_set->runCallbacks({host}, {});

  stagger_timer->invokeCallback();
  EXPECT_EQ(1, manager->oobStatsForTest().active_sessions_.value());

  // We expect at least one deferred-delete call when the host is removed (the
  // session itself; the codec client factory returns null so there's no codec
  // client to deferred-delete).
  EXPECT_CALL(dispatcher_, deferredDelete_(testing::_)).Times(testing::AtLeast(1));

  host_set->hosts_.clear();
  host_set->runCallbacks({}, {host});

  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());
}

// Pre-existing hosts are scheduled when initialize() runs.
TEST_F(OrcaWeightManagerOobTest, ExistingHostsScheduledOnInitialize) {
  // Two stagger timers (consumed after the weight calc timer, in LIFO order).
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto* host_set = priority_set_.getMockHostSet(0);
  Upstream::HostVector hosts;
  for (int i = 0; i < 2; ++i) {
    hosts.push_back(makeWeightTrackingMockHost());
  }
  host_set->hosts_ = hosts;

  auto manager = makeOobManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  // Stagger timers were scheduled but not fired → no factory calls and no
  // active sessions yet.
  EXPECT_EQ(0U, factory_->create_calls_);
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());

  // Each host should have OrcaHostLbPolicyData attached.
  for (const auto& h : hosts) {
    EXPECT_TRUE(h->typedLbPolicyData<OrcaHostLbPolicyData>().has_value());
  }
}

// Duplicate add notifications (e.g. priority moves) must not double-schedule.
TEST_F(OrcaWeightManagerOobTest, DuplicateHostAdd_DoesNotDoubleSchedule) {
  // Only ONE stagger timer expected even though we run the callback twice
  // with the same host. If a second stagger were scheduled, the MockDispatcher
  // would have no matching MockTimer ready (RetiresOnSaturation already
  // consumed both expectations) and the test would fail with an unmatched
  // call.
  new NiceMock<Event::MockTimer>(&dispatcher_); // stagger
  new NiceMock<Event::MockTimer>(&dispatcher_); // weight calc

  auto manager = makeOobManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto* host_set = priority_set_.getMockHostSet(0);
  auto host = makeWeightTrackingMockHost();
  host_set->hosts_ = {host};

  host_set->runCallbacks({host}, {});
  // Same host, second notification — must not allocate another stagger timer.
  host_set->runCallbacks({host}, {});
}

// ============================================================
// OrcaWeightManager OOB session-callback tests (driven via wire)
//
// These tests use a smart codec-client factory that constructs a real
// CodecClientForTest backed by mock connections, so the production
// on_report_cb / on_terminated_cb / on_lifecycle_cb closures captured
// inside startOobSession() are exercised end-to-end. Mirrors the pattern
// in test/common/orca/orca_oob_session_test.cc.
// ============================================================

// Per-attempt mocks: every codec-client construction stashes one of these
// so the test can drive headers/data/trailers over the response decoder
// for that attempt.
struct OobAttempt {
  NiceMock<Network::MockClientConnection>* network_connection{nullptr};
  NiceMock<Http::MockClientConnection>* codec{nullptr};
  NiceMock<Http::MockRequestEncoder> request_encoder;
  Http::ResponseDecoder* response_decoder{nullptr};
  CodecClientForTest* codec_client{nullptr};
};

// Smart factory that builds a real CodecClientForTest per attempt. The
// factory holds a cluster info handle (so it can construct the host
// description used by CodecClient) and keeps every per-attempt mock alive
// for the duration of the test.
// TODO: factor with test/common/orca/orca_oob_session_test.cc
class WireDrivingOrcaOobCodecClientFactory : public OrcaOobCodecClientFactory {
public:
  explicit WireDrivingOrcaOobCodecClientFactory(
      std::shared_ptr<Upstream::MockClusterInfo> cluster_info)
      : cluster_info_(std::move(cluster_info)) {}

  Http::CodecClientPtr create(
      Upstream::Host::CreateConnectionData&& /*connection_data*/, Event::Dispatcher& dispatcher,
      Random::RandomGenerator& /*random*/,
      Network::TransportSocketOptionsConstSharedPtr /*transport_socket_options*/) const override {
    auto attempt = std::make_unique<OobAttempt>();
    attempt->network_connection = new NiceMock<Network::MockClientConnection>();
    attempt->codec = new NiceMock<Http::MockClientConnection>();
    EXPECT_CALL(*attempt->codec, newStream(testing::_))
        .WillOnce(testing::DoAll(Envoy::SaveArgAddress(&attempt->response_decoder),
                                 testing::ReturnRef(attempt->request_encoder)));
    // Accept (and ignore) the request the session sends; we only care
    // about what the manager does with reports.
    EXPECT_CALL(attempt->request_encoder, encodeHeaders(testing::_, false))
        .WillOnce(testing::Return(Http::okStatus()));
    EXPECT_CALL(attempt->request_encoder, encodeData(testing::_, true));

    Network::ClientConnectionPtr conn{attempt->network_connection};
    auto codec_client = std::make_unique<CodecClientForTest>(
        Http::CodecType::HTTP2, std::move(conn), attempt->codec, nullptr,
        Upstream::makeTestHost(cluster_info_, "tcp://127.0.0.1:9000"), dispatcher);
    attempt->codec_client = codec_client.get();
    attempts_.push_back(std::move(attempt));
    return codec_client;
  }

  // Mutable so the const create() above can append.
  mutable std::vector<std::unique_ptr<OobAttempt>> attempts_;

private:
  std::shared_ptr<Upstream::MockClusterInfo> cluster_info_;
};

class OrcaWeightManagerOobWireTest : public OrcaWeightManagerTest {
protected:
  void SetUp() override {
    OrcaWeightManagerTest::SetUp();
    config_.oob_enabled = true;
    config_.oob_reporting_period = std::chrono::milliseconds(10000);
    config_.oob_request_cost_names = {"cpu_utilization"};
    cluster_info_ = std::make_shared<NiceMock<Upstream::MockClusterInfo>>();
  }

  std::unique_ptr<OrcaWeightManager> makeWireManager() {
    auto factory = std::make_unique<WireDrivingOrcaOobCodecClientFactory>(cluster_info_);
    factory_ = factory.get();
    return makeManager(std::move(factory));
  }

  // Drive a single end-to-end attempt: bring a pending host live by firing
  // the stagger timer, then return the OobAttempt for the freshly opened
  // attempt so the caller can deliver headers/data/trailers.
  OobAttempt& startSessionForHost(const std::shared_ptr<NiceMock<Upstream::MockHost>>& host,
                                  NiceMock<Event::MockTimer>* stagger_timer) {
    auto* host_set = priority_set_.getMockHostSet(0);
    host_set->hosts_ = {host};
    host_set->runCallbacks({host}, {});
    stagger_timer->invokeCallback();
    EXPECT_FALSE(factory_->attempts_.empty());
    return *factory_->attempts_.back();
  }

  void respondHeadersOk(OobAttempt& attempt) {
    auto headers =
        std::make_unique<Http::TestResponseHeaderMapImpl>(Http::TestResponseHeaderMapImpl(
            {{":status", "200"}, {"content-type", "application/grpc"}}));
    attempt.response_decoder->decodeHeaders(std::move(headers), false);
  }

  void respondReport(OobAttempt& attempt, const xds::data::orca::v3::OrcaLoadReport& report) {
    auto frame = Grpc::Common::serializeToGrpcFrame(report);
    attempt.response_decoder->decodeData(*frame, /*end_stream=*/false);
  }

  void respondTrailers(OobAttempt& attempt, Grpc::Status::GrpcStatus status,
                       const std::string& message = "") {
    auto trailers = std::make_unique<Http::TestResponseTrailerMapImpl>(
        Http::TestResponseTrailerMapImpl({{"grpc-status", absl::StrCat(status)}}));
    if (!message.empty()) {
      trailers->addCopy(Http::Headers::get().GrpcMessage, message);
    }
    attempt.response_decoder->decodeTrailers(std::move(trailers));
  }

  std::shared_ptr<NiceMock<Upstream::MockClusterInfo>> cluster_info_;
  WireDrivingOrcaOobCodecClientFactory* factory_{nullptr};
};

// Test 1: a successful ORCA report received over the wire increments the
// reports_received counter AND propagates into OrcaHostLbPolicyData via
// the report handler (i.e. report_handler_ was actually invoked).
TEST_F(OrcaWeightManagerOobWireTest, ReportReceived_IncrementsCounterAndUpdatesHostData) {
  // Session goaway drain + retry timers (consumed last by LIFO).
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Stagger timer.
  auto* stagger_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first by manager ctor).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeWireManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto host = makeWeightTrackingMockHost();
  auto& attempt = startSessionForHost(host, stagger_timer);

  // Open the stream.
  respondHeadersOk(attempt);
  EXPECT_EQ(1U, manager->oobStatsForTest().stream_opens_.value());
  EXPECT_EQ(0U, manager->oobStatsForTest().reports_received_.value());

  // Anchor "now" so the report-handler's weight calc updates timestamps
  // we can later verify against.
  time_system_.setMonotonicTime(MonotonicTime(std::chrono::seconds(60)));

  // Deliver a valid ORCA report. The report handler will compute a
  // weight (rps_fractional=1000, application_utilization=0.5 → 2000) and
  // stamp timestamps onto the host's OrcaHostLbPolicyData.
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_application_utilization(0.5);
  respondReport(attempt, report);

  // Counter incremented exactly once via the production on_report_cb.
  EXPECT_EQ(1U, manager->oobStatsForTest().reports_received_.value());
  EXPECT_EQ(0U, manager->oobStatsForTest().report_errors_.value());

  // Side effect: report_handler_->updateClientSideDataFromOrcaLoadReport
  // wrote the new weight into the host's OrcaHostLbPolicyData. We observe
  // that side effect rather than mocking the handler, because the manager
  // owns its handler internally.
  auto data_opt = host->typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(data_opt.has_value());
  EXPECT_EQ(2000U, data_opt->weight_.load());
  EXPECT_EQ(MonotonicTime(std::chrono::seconds(60)), data_opt->last_update_time_.load());

  // Tear down via host removal so the manager defers-deletes cleanly.
  EXPECT_CALL(dispatcher_, deferredDelete_(testing::_)).Times(testing::AtLeast(1));
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_.clear();
  host_set->runCallbacks({}, {host});
}

// Test 2: when the session terminates (UNIMPLEMENTED here, but the
// manager's on_terminated_cb is status-agnostic — it always tears down),
// stream_terminated is incremented, the session is erased from
// oob_sessions_, and the active_sessions gauge is decremented to zero.
//
// NOTE: OrcaOobSession only fires its terminal callback for UNIMPLEMENTED
// (see OrcaOobSession::onRpcComplete). Other gRPC statuses go through
// handleTransientFailure and do not invoke on_terminated_cb. We therefore
// drive UNIMPLEMENTED here; the manager's cleanup logic does not branch
// on status, so this fully exercises the terminal cleanup path that
// would also run for any other status the session might one day deliver.
TEST_F(OrcaWeightManagerOobWireTest, TerminalCallback_ErasesSessionAndDecrementsGauge) {
  // Session goaway drain + retry timers.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Stagger timer.
  auto* stagger_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeWireManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto host = makeWeightTrackingMockHost();
  auto& attempt = startSessionForHost(host, stagger_timer);

  respondHeadersOk(attempt);
  EXPECT_EQ(1, manager->oobStatsForTest().active_sessions_.value());
  EXPECT_EQ(0U, manager->oobStatsForTest().stream_terminated_.value());

  // Expect the terminal callback to deferred-delete the session.
  EXPECT_CALL(dispatcher_, deferredDelete_(testing::_)).Times(testing::AtLeast(1));

  // Drive UNIMPLEMENTED trailers — the only status that crosses the
  // session's terminal boundary into the manager's on_terminated_cb.
  respondTrailers(attempt, Grpc::Status::WellKnownGrpcStatus::Unimplemented, "no service");

  // Production on_terminated_cb ran:
  //  - bumped stream_terminated counter
  //  - erased the session from oob_sessions_ (observed via gauge)
  //  - decremented active_sessions gauge
  EXPECT_EQ(1U, manager->oobStatsForTest().stream_terminated_.value());
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());
}

// Test 3: UNIMPLEMENTED stops the session permanently. After the terminal
// fires and the manager evicts the session, no subsequent activity (no
// new factory invocation, no fresh active session) occurs for that host.
TEST_F(OrcaWeightManagerOobWireTest, UnimplementedStopsRetries_NoFurtherSession) {
  // Session goaway drain + retry timers.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Stagger timer.
  auto* stagger_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer.
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeWireManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto host = makeWeightTrackingMockHost();
  auto& attempt = startSessionForHost(host, stagger_timer);
  ASSERT_EQ(1U, factory_->attempts_.size());

  respondHeadersOk(attempt);

  EXPECT_CALL(dispatcher_, deferredDelete_(testing::_)).Times(testing::AtLeast(1));
  respondTrailers(attempt, Grpc::Status::WellKnownGrpcStatus::Unimplemented);

  // Same teardown invariants as Test 2.
  EXPECT_EQ(1U, manager->oobStatsForTest().stream_terminated_.value());
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());

  // The dispatcher is idle (no test-driven retry timer fire, no host
  // removal/re-add). The manager must not have spun up a replacement
  // session: factory.create() count is unchanged from the single
  // initial attempt.
  EXPECT_EQ(1U, factory_->attempts_.size());
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());
}

// Test 4: after a terminal callback evicts the session, a subsequent
// host-add notification (e.g. a priority shuffle re-adding the same
// HostSharedPtr) MUST be able to schedule a fresh stagger. Regression test
// for the "fired marker" zombie that previously prevented re-scheduling
// because pending_oob_session_timers_ still held the post-fire marker.
//
// Timer LIFO note: MockTimer ctor stacks expectations so the LAST allocated
// is consumed FIRST. Required consumption order (chronological):
//   1. weight calc (manager ctor)
//   2. stagger_1 (first runCallbacks)
//   3. retry_1, 4. goaway_1 (OrcaOobSession ctor for first session)
//   5. stagger_2 (re-add after terminal)
//   6. retry_2, 7. goaway_2 (OrcaOobSession ctor for second session)
// Therefore allocate in the REVERSE of that order below.
TEST_F(OrcaWeightManagerOobWireTest, TerminalCallback_AllowsReschedulingOnReAdd) {
  new NiceMock<Event::MockTimer>(&dispatcher_); // 7. session goaway drain (2)
  new NiceMock<Event::MockTimer>(&dispatcher_); // 6. session retry (2)
  auto* stagger_timer_2 = new NiceMock<Event::MockTimer>(&dispatcher_); // 5. stagger (2)
  new NiceMock<Event::MockTimer>(&dispatcher_); // 4. session goaway drain (1)
  new NiceMock<Event::MockTimer>(&dispatcher_); // 3. session retry (1)
  auto* stagger_timer_1 = new NiceMock<Event::MockTimer>(&dispatcher_); // 2. stagger (1)
  new NiceMock<Event::MockTimer>(&dispatcher_); // 1. weight calc (manager ctor)

  auto manager = makeWireManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto host = makeWeightTrackingMockHost();
  auto& attempt = startSessionForHost(host, stagger_timer_1);
  ASSERT_EQ(1U, factory_->attempts_.size());

  respondHeadersOk(attempt);

  // Drive UNIMPLEMENTED so the session terminates and the manager's
  // on_terminated_cb runs (which must clear the pending marker).
  EXPECT_CALL(dispatcher_, deferredDelete_(testing::_)).Times(testing::AtLeast(1));
  respondTrailers(attempt, Grpc::Status::WellKnownGrpcStatus::Unimplemented);
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());

  // Re-add the same host (without an intervening remove) — simulates a
  // priority shuffle that re-presents an existing host. With the marker
  // cleared this MUST schedule a new stagger; without the fix the
  // schedulePendingOobSession early-out would silently swallow the add.
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->runCallbacks({host}, {});

  // Fire the new stagger timer — a second factory.create() must occur,
  // proving that the marker was cleared and a fresh session was scheduled.
  stagger_timer_2->invokeCallback();
  EXPECT_EQ(2U, factory_->attempts_.size());
  EXPECT_EQ(1, manager->oobStatsForTest().active_sessions_.value());

  // Tear down the second session via host removal.
  EXPECT_CALL(dispatcher_, deferredDelete_(testing::_)).Times(testing::AtLeast(1));
  host_set->hosts_.clear();
  host_set->runCallbacks({}, {host});
  EXPECT_EQ(0, manager->oobStatsForTest().active_sessions_.value());
}

// Manager destruction with active sessions cleans them up via deferredDelete.
TEST_F(OrcaWeightManagerOobTest, DestructionDeferredDeletesActiveSessions) {
  // Session goaway drain + retry timers.
  new NiceMock<Event::MockTimer>(&dispatcher_);
  new NiceMock<Event::MockTimer>(&dispatcher_);
  // Stagger timer.
  auto* stagger_timer = new NiceMock<Event::MockTimer>(&dispatcher_);
  // Weight calc timer (consumed first).
  new NiceMock<Event::MockTimer>(&dispatcher_);

  auto manager = makeOobManager();
  auto status = manager->initialize();
  ASSERT_TRUE(status.ok());

  auto* host_set = priority_set_.getMockHostSet(0);
  auto host = makeWeightTrackingMockHost();
  host_set->hosts_ = {host};
  host_set->runCallbacks({host}, {});

  stagger_timer->invokeCallback();
  EXPECT_EQ(1, manager->oobStatsForTest().active_sessions_.value());

  // Destructor must defer-delete the session.
  EXPECT_CALL(dispatcher_, deferredDelete_(testing::_)).Times(testing::AtLeast(1));
  manager.reset();
}

} // namespace
} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
