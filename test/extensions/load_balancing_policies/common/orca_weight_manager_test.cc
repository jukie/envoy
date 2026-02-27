#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"

#include "test/mocks/common.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/stream_info/mocks.h"
#include "test/mocks/upstream/host.h"
#include "test/mocks/upstream/host_set.h"
#include "test/mocks/upstream/priority_set.h"
#include "test/test_common/logging.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xds/data/orca/v3/orca_load_report.pb.h"

namespace Envoy {
namespace Extensions {
namespace LoadBalancingPolicies {
namespace Common {
namespace {

// --- OrcaLoadReportHandler: weight calculation ---

TEST(OrcaWeightManagerTest, CalculateWeight_ValidQpsAndUtilization) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(100);
  report.set_cpu_utilization(0.5);

  auto result = OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {}, 0);
  ASSERT_TRUE(result.ok());
  // weight = qps / utilization = 100 / 0.5 = 200
  EXPECT_EQ(200, result.value());
}

TEST(OrcaWeightManagerTest, CalculateWeight_ApplicationUtilizationPriority) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(100);
  report.set_application_utilization(0.25);
  report.set_cpu_utilization(0.5);

  auto result = OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {}, 0);
  ASSERT_TRUE(result.ok());
  // application_utilization takes priority: weight = 100 / 0.25 = 400
  EXPECT_EQ(400, result.value());
}

TEST(OrcaWeightManagerTest, CalculateWeight_ErrorPenalty) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(100);
  report.set_cpu_utilization(0.5);
  report.set_eps(10);

  auto result = OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {}, 1.0);
  ASSERT_TRUE(result.ok());
  // utilization = 0.5 + 1.0 * 10/100 = 0.6
  // weight = 100 / 0.6 = 166.67 -> 166
  EXPECT_EQ(166, result.value());
}

TEST(OrcaWeightManagerTest, CalculateWeight_NoQps) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(0);
  report.set_cpu_utilization(0.5);

  auto result = OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {}, 0);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(absl::StatusCode::kInvalidArgument, result.status().code());
}

TEST(OrcaWeightManagerTest, CalculateWeight_NoUtilization) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);

  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 0.0);
  EXPECT_EQ(result.status(), absl::InvalidArgumentError("Utilization must be positive"));
}

TEST(OrcaWeightManagerTest, CalculateWeight_MaxWeight) {
  xds::data::orca::v3::OrcaLoadReport report;
  // High QPS and low utilization.
  report.set_rps_fractional(10000000000000L);
  report.set_application_utilization(0.0000001);

  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 0.0);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(std::numeric_limits<uint32_t>::max(), result.value());
}

TEST(OrcaWeightManagerTest, CalculateWeight_ValidNoErrorPenalty) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_eps(100);
  report.set_application_utilization(0.5);

  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 0.0);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(2000, result.value());
}

TEST(OrcaWeightManagerTest, CalculateWeight_ValidWithErrorPenalty) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(1000);
  report.set_eps(100);
  report.set_application_utilization(0.5);

  auto result =
      OrcaLoadReportHandler::calculateWeightFromOrcaReport(report, {"named_metrics.foo"}, 2.0);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(1428, result.value());
}

// --- OrcaLoadReportHandler: utilization extraction ---

TEST(OrcaWeightManagerTest, GetUtilization_ApplicationUtilizationFirst) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_application_utilization(0.3);
  report.set_cpu_utilization(0.8);

  double utilization = OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {});
  EXPECT_DOUBLE_EQ(0.3, utilization);
}

TEST(OrcaWeightManagerTest, GetUtilization_ApplicationUtilizationOverNamedMetrics) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_application_utilization(0.5);
  report.mutable_named_metrics()->insert({"foo", 0.3});
  report.set_cpu_utilization(0.6);

  double utilization =
      OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"});
  EXPECT_EQ(0.5, utilization);
}

TEST(OrcaWeightManagerTest, GetUtilization_NamedMetrics) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.mutable_named_metrics()->insert({"foo", 0.3});
  report.set_cpu_utilization(0.6);

  double utilization =
      OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"});
  EXPECT_EQ(0.3, utilization);
}

TEST(OrcaWeightManagerTest, GetUtilization_CpuUtilizationFallback) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.set_cpu_utilization(0.7);

  double utilization = OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {});
  EXPECT_DOUBLE_EQ(0.7, utilization);
}

TEST(OrcaWeightManagerTest, GetUtilization_CpuFallbackWhenNamedMissing) {
  xds::data::orca::v3::OrcaLoadReport report;
  report.mutable_named_metrics()->insert({"bar", 0.3});
  report.set_cpu_utilization(0.6);

  double utilization =
      OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"});
  EXPECT_EQ(0.6, utilization);
}

TEST(OrcaWeightManagerTest, GetUtilization_NoUtilization) {
  xds::data::orca::v3::OrcaLoadReport report;

  double utilization =
      OrcaLoadReportHandler::getUtilizationFromOrcaReport(report, {"named_metrics.foo"});
  EXPECT_EQ(0, utilization);
}

// --- OrcaHostLbPolicyData ---

TEST(OrcaWeightManagerTest, HostLbPolicyData_BlackoutPeriod) {
  // Weight set "now" should be invalid during the blackout period.
  auto now = MonotonicTime(std::chrono::seconds(100));
  // nullptr handler is fine - we only test getWeightIfValid, not onOrcaLoadReport.
  OrcaHostLbPolicyData data(nullptr, 42, now, now);

  // max_non_empty_since = now - 10s = 90s. Since non_empty_since (100s) > 90s, weight is invalid.
  auto max_non_empty_since = MonotonicTime(std::chrono::seconds(90));
  auto min_last_update_time = MonotonicTime(std::chrono::seconds(0));
  EXPECT_EQ(std::nullopt, data.getWeightIfValid(max_non_empty_since, min_last_update_time));

  // After blackout period: max_non_empty_since = 100s, weight should be valid.
  max_non_empty_since = MonotonicTime(std::chrono::seconds(100));
  EXPECT_EQ(42, data.getWeightIfValid(max_non_empty_since, min_last_update_time));
}

TEST(OrcaWeightManagerTest, HostLbPolicyData_WeightExpiration) {
  auto now = MonotonicTime(std::chrono::seconds(100));
  OrcaHostLbPolicyData data(nullptr, 42, now, now);

  // Weight should be valid when min_last_update_time (50s) < last_update_time (100s).
  auto max_non_empty_since = MonotonicTime(std::chrono::seconds(100));
  auto min_last_update_time = MonotonicTime(std::chrono::seconds(50));
  EXPECT_EQ(42, data.getWeightIfValid(max_non_empty_since, min_last_update_time));

  // Weight should be expired when min_last_update_time (200s) > last_update_time (100s).
  // Note: expiration also resets non_empty_since_, so further calls will also return nullopt.
  min_last_update_time = MonotonicTime(std::chrono::seconds(200));
  EXPECT_EQ(std::nullopt, data.getWeightIfValid(max_non_empty_since, min_last_update_time));
}

TEST(OrcaWeightManagerTest, GetWeightIfValidFromHost_NoData) {
  NiceMock<Envoy::Upstream::MockHost> host;
  // Host has no LB policy data, so typedLbPolicyData should return nullopt.
  auto data = host.typedLbPolicyData<OrcaHostLbPolicyData>();
  EXPECT_FALSE(data.has_value());
}

TEST(OrcaWeightManagerTest, GetWeightIfValidFromHost_TooRecent) {
  NiceMock<Envoy::Upstream::MockHost> host;
  host.lb_policy_data_ = std::make_unique<OrcaHostLbPolicyData>(
      nullptr, 42, /*non_empty_since=*/MonotonicTime(std::chrono::seconds(5)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10)));
  // Non empty since is too recent (5 > 2).
  auto data = host.typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(data.has_value());
  EXPECT_FALSE(data->getWeightIfValid(
      /*max_non_empty_since=*/MonotonicTime(std::chrono::seconds(2)),
      /*min_last_update_time=*/MonotonicTime(std::chrono::seconds(8))));
  // non_empty_since_ is not updated.
  EXPECT_EQ(data->non_empty_since_.load(), MonotonicTime(std::chrono::seconds(5)));
}

TEST(OrcaWeightManagerTest, GetWeightIfValidFromHost_TooStale) {
  NiceMock<Envoy::Upstream::MockHost> host;
  host.lb_policy_data_ = std::make_unique<OrcaHostLbPolicyData>(
      nullptr, 42, /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(7)));
  // Last update time is too stale (7 < 8).
  auto data = host.typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(data.has_value());
  EXPECT_FALSE(data->getWeightIfValid(
      /*max_non_empty_since=*/MonotonicTime(std::chrono::seconds(2)),
      /*min_last_update_time=*/MonotonicTime(std::chrono::seconds(8))));
  // Also resets the non_empty_since_ time.
  EXPECT_EQ(data->non_empty_since_.load(), OrcaHostLbPolicyData::kDefaultNonEmptySince);
}

TEST(OrcaWeightManagerTest, GetWeightIfValidFromHost_Valid) {
  NiceMock<Envoy::Upstream::MockHost> host;
  host.lb_policy_data_ = std::make_unique<OrcaHostLbPolicyData>(
      nullptr, 42, /*non_empty_since=*/MonotonicTime(std::chrono::seconds(1)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(10)));
  auto data = host.typedLbPolicyData<OrcaHostLbPolicyData>();
  ASSERT_TRUE(data.has_value());
  // Not empty since is not too recent (1 < 2) and last update time is not too old (10 > 8).
  EXPECT_EQ(42, data->getWeightIfValid(
                        /*max_non_empty_since=*/MonotonicTime(std::chrono::seconds(2)),
                        /*min_last_update_time=*/MonotonicTime(std::chrono::seconds(8)))
                    .value());
  // non_empty_since_ is not updated.
  EXPECT_EQ(data->non_empty_since_.load(), MonotonicTime(std::chrono::seconds(1)));
}

// --- OrcaLoadReportHandler: updateClientSideDataFromOrcaLoadReport ---

TEST(OrcaWeightManagerTest, UpdateClientSideData_Success) {
  OrcaWeightManagerConfig config;
  config.error_utilization_penalty = 0.0;
  NiceMock<MockTimeSystem> time_system;
  auto now = MonotonicTime(std::chrono::seconds(50));
  ON_CALL(time_system, monotonicTime()).WillByDefault(testing::Return(now));

  OrcaLoadReportHandler handler(config, time_system);
  OrcaHostLbPolicyData data(nullptr);

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(100);
  report.set_cpu_utilization(0.5);

  EXPECT_EQ(absl::OkStatus(), handler.updateClientSideDataFromOrcaLoadReport(report, data));
  EXPECT_EQ(200, data.weight_.load());
  EXPECT_EQ(now, data.last_update_time_.load());
  EXPECT_EQ(now, data.non_empty_since_.load());
}

TEST(OrcaWeightManagerTest, UpdateClientSideData_ErrorReturnsStatus) {
  OrcaWeightManagerConfig config;
  config.error_utilization_penalty = 0.0;
  NiceMock<MockTimeSystem> time_system;

  OrcaLoadReportHandler handler(config, time_system);
  OrcaHostLbPolicyData data(nullptr);

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(0); // Invalid QPS.
  report.set_cpu_utilization(0.5);

  auto status = handler.updateClientSideDataFromOrcaLoadReport(report, data);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(absl::StatusCode::kInvalidArgument, status.code());
  // Weight should remain at default since the update failed.
  EXPECT_EQ(1, data.weight_.load());
}

// --- OrcaHostLbPolicyData: onOrcaLoadReport ---

TEST(OrcaWeightManagerTest, OnOrcaLoadReport_Success) {
  OrcaWeightManagerConfig config;
  config.error_utilization_penalty = 0.0;
  NiceMock<MockTimeSystem> time_system;
  auto now = MonotonicTime(std::chrono::seconds(10));
  ON_CALL(time_system, monotonicTime()).WillByDefault(testing::Return(now));

  auto handler = std::make_shared<OrcaLoadReportHandler>(config, time_system);
  OrcaHostLbPolicyData data(handler);

  xds::data::orca::v3::OrcaLoadReport report;
  report.set_rps_fractional(200);
  report.set_application_utilization(0.4);

  NiceMock<StreamInfo::MockStreamInfo> stream_info;
  EXPECT_EQ(absl::OkStatus(), data.onOrcaLoadReport(report, stream_info));
  EXPECT_EQ(500, data.weight_.load()); // 200 / 0.4 = 500
}

TEST(OrcaWeightManagerTest, OnOrcaLoadReport_InvalidReport) {
  OrcaWeightManagerConfig config;
  config.error_utilization_penalty = 0.0;
  NiceMock<MockTimeSystem> time_system;

  auto handler = std::make_shared<OrcaLoadReportHandler>(config, time_system);
  OrcaHostLbPolicyData data(handler);

  xds::data::orca::v3::OrcaLoadReport report;
  // No QPS or utilization set.

  NiceMock<StreamInfo::MockStreamInfo> stream_info;
  auto status = data.onOrcaLoadReport(report, stream_info);
  EXPECT_FALSE(status.ok());
}

// --- OrcaHostLbPolicyData: updateWeightNow ---

TEST(OrcaWeightManagerTest, UpdateWeightNow_SetsNonEmptySinceOnlyOnce) {
  OrcaHostLbPolicyData data(nullptr);
  EXPECT_EQ(OrcaHostLbPolicyData::kDefaultNonEmptySince, data.non_empty_since_.load());

  auto t1 = MonotonicTime(std::chrono::seconds(10));
  data.updateWeightNow(100, t1);
  EXPECT_EQ(100, data.weight_.load());
  EXPECT_EQ(t1, data.non_empty_since_.load());
  EXPECT_EQ(t1, data.last_update_time_.load());

  // Second update should NOT change non_empty_since_.
  auto t2 = MonotonicTime(std::chrono::seconds(20));
  data.updateWeightNow(200, t2);
  EXPECT_EQ(200, data.weight_.load());
  EXPECT_EQ(t1, data.non_empty_since_.load()); // Still t1.
  EXPECT_EQ(t2, data.last_update_time_.load());
}

// --- OrcaWeightManager: updateWeightsOnHosts ---

// Helper to construct an OrcaWeightManager for unit testing.
class OrcaWeightManagerDirectTest : public testing::Test {
public:
  void SetUp() override {
    ON_CALL(time_system_, monotonicTime())
        .WillByDefault(testing::Return(MonotonicTime(std::chrono::seconds(100))));
    // MockTimer must be created before OrcaWeightManager so createTimer_ returns it.
    timer_ = new Event::MockTimer(&dispatcher_);
    ON_CALL(priority_set_, hostSetsPerPriority())
        .WillByDefault(testing::ReturnRef(priority_set_.host_sets_));
  }

  std::unique_ptr<OrcaWeightManager> createManager() {
    return std::make_unique<OrcaWeightManager>(config_, priority_set_, time_system_, dispatcher_,
                                               [this]() { weights_updated_count_++; });
  }

  OrcaWeightManagerConfig config_{
      /*metric_names_for_computing_utilization=*/{},
      /*error_utilization_penalty=*/0.0,
      /*blackout_period=*/std::chrono::milliseconds(10000),
      /*weight_expiration_period=*/std::chrono::milliseconds(180000),
      /*weight_update_period=*/std::chrono::milliseconds(1000),
  };
  NiceMock<MockTimeSystem> time_system_;
  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Upstream::MockPrioritySet> priority_set_;
  Event::MockTimer* timer_ = nullptr;
  int weights_updated_count_ = 0;
};

TEST_F(OrcaWeightManagerDirectTest, UpdateWeightsOnHosts_HostWithoutPolicyData) {
  auto manager = createManager();

  // Host without OrcaHostLbPolicyData.
  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  ON_CALL(*host, weight()).WillByDefault(testing::Return(1));
  Upstream::HostVector hosts{host};

  // No valid ORCA weights → default weight = 1 → matches host weight → no update.
  EXPECT_FALSE(manager->updateWeightsOnHosts(hosts));
  EXPECT_EQ(0, weights_updated_count_);
}

TEST_F(OrcaWeightManagerDirectTest, UpdateWeightsOnHosts_MixedHostsWithTraceLogging) {
  // Enable trace logging so ENVOY_LOG(trace, ...) arguments (including getHostAddress) execute.
  LogLevelSetter log_level(spdlog::level::trace);

  auto manager = createManager();

  // Host 1: valid ORCA data, past blackout and not expired.
  auto host1 = std::make_shared<NiceMock<Upstream::MockHost>>();
  host1->lb_policy_data_ = std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 500,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(50)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(95)));
  ON_CALL(*host1, weight()).WillByDefault(testing::Return(1));

  // Host 2: no ORCA data.
  auto host2 = std::make_shared<NiceMock<Upstream::MockHost>>();
  ON_CALL(*host2, weight()).WillByDefault(testing::Return(1));

  Upstream::HostVector hosts{host1, host2};

  // host1 gets weight 500 from ORCA, host2 gets median (500) as default.
  // Both change from 1 → 500, so weights are updated.
  EXPECT_TRUE(manager->updateWeightsOnHosts(hosts));
}

// --- OrcaWeightManager: initialize ---

// Helper to wire up setLbPolicyData on a MockHost so it actually stores the data.
void setupLbPolicyDataStorage(NiceMock<Upstream::MockHost>& host) {
  ON_CALL(host, setLbPolicyData(testing::_))
      .WillByDefault(testing::Invoke(
          [&host](Upstream::HostLbPolicyDataPtr data) { host.lb_policy_data_ = std::move(data); }));
}

TEST_F(OrcaWeightManagerDirectTest, Initialize_AttachesLbPolicyDataToExistingHosts) {
  // Set up a host in the priority set before initialize().
  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  setupLbPolicyDataStorage(*host);
  ON_CALL(*host, weight()).WillByDefault(testing::Return(1));
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_ = {host};

  auto manager = createManager();
  EXPECT_FALSE(host->lbPolicyData().has_value());

  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(1000), testing::_));
  EXPECT_TRUE(manager->initialize().ok());

  // After initialize, host should have OrcaHostLbPolicyData attached.
  EXPECT_TRUE(host->lbPolicyData().has_value());
  EXPECT_TRUE(host->typedLbPolicyData<OrcaHostLbPolicyData>().has_value());
}

TEST_F(OrcaWeightManagerDirectTest, Initialize_StartsTimer) {
  auto manager = createManager();
  EXPECT_FALSE(timer_->enabled_);

  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(1000), testing::_));
  EXPECT_TRUE(manager->initialize().ok());
  EXPECT_TRUE(timer_->enabled_);
}

TEST_F(OrcaWeightManagerDirectTest, Initialize_PriorityUpdateAttachesDataToNewHosts) {
  auto manager = createManager();

  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(1000), testing::_))
      .Times(testing::AtLeast(1));
  EXPECT_TRUE(manager->initialize().ok());

  // Simulate adding a new host via priority set update.
  auto new_host = std::make_shared<NiceMock<Upstream::MockHost>>();
  setupLbPolicyDataStorage(*new_host);
  ON_CALL(*new_host, weight()).WillByDefault(testing::Return(1));
  EXPECT_FALSE(new_host->lbPolicyData().has_value());

  priority_set_.runUpdateCallbacks(0, {new_host}, {});

  // New host should now have OrcaHostLbPolicyData.
  EXPECT_TRUE(new_host->lbPolicyData().has_value());
  EXPECT_TRUE(new_host->typedLbPolicyData<OrcaHostLbPolicyData>().has_value());
}

// --- OrcaWeightManager: updateWeightsOnMainThread ---

TEST_F(OrcaWeightManagerDirectTest, UpdateWeightsOnMainThread_InvokesCallbackWhenWeightsChange) {
  auto manager = createManager();

  // Set up a host with valid ORCA data whose weight differs from current host weight.
  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  host->lb_policy_data_ = std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 500,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(50)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(95)));
  ON_CALL(*host, weight()).WillByDefault(testing::Return(1));
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_ = {host};

  EXPECT_EQ(0, weights_updated_count_);
  manager->updateWeightsOnMainThread();
  EXPECT_EQ(1, weights_updated_count_);
}

TEST_F(OrcaWeightManagerDirectTest, UpdateWeightsOnMainThread_NoCallbackWhenNoChanges) {
  auto manager = createManager();

  // Host without ORCA data, weight already at default (1).
  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  ON_CALL(*host, weight()).WillByDefault(testing::Return(1));
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_ = {host};

  manager->updateWeightsOnMainThread();
  EXPECT_EQ(0, weights_updated_count_);
}

TEST_F(OrcaWeightManagerDirectTest, TimerCallback_TriggersWeightUpdateAndReschedules) {
  auto manager = createManager();

  auto host = std::make_shared<NiceMock<Upstream::MockHost>>();
  host->lb_policy_data_ = std::make_unique<OrcaHostLbPolicyData>(
      manager->reportHandler(), 300,
      /*non_empty_since=*/MonotonicTime(std::chrono::seconds(50)),
      /*last_update_time=*/MonotonicTime(std::chrono::seconds(95)));
  ON_CALL(*host, weight()).WillByDefault(testing::Return(1));
  auto* host_set = priority_set_.getMockHostSet(0);
  host_set->hosts_ = {host};

  EXPECT_CALL(*timer_, enableTimer(std::chrono::milliseconds(1000), testing::_))
      .Times(testing::AtLeast(1));
  EXPECT_TRUE(manager->initialize().ok());
  EXPECT_TRUE(timer_->enabled_);
  EXPECT_EQ(0, weights_updated_count_);

  // Fire the timer callback.
  timer_->invokeCallback();

  // Callback should have updated weights and re-enabled the timer.
  EXPECT_EQ(1, weights_updated_count_);
  EXPECT_TRUE(timer_->enabled_);
}

} // namespace
} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
