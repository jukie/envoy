#include "source/extensions/load_balancing_policies/common/orca_weight_manager.h"

#include "test/mocks/upstream/host.h"

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

} // namespace
} // namespace Common
} // namespace LoadBalancingPolicies
} // namespace Extensions
} // namespace Envoy
